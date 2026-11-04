/*
 * AT6301 power supply link. See psu.h.
 *
 * Command set (from the open source Benchflow driver for this supply, which
 * also matches the OWON SPE6103 it is built on):
 *
 *   *IDN?            Abestop,AT6301,<serial>,FV:<firmware>
 *   MEAS:ALL:INFO?   V,A,W,OVP,OCP,OTP,mode   faults ON/OFF, mode 0..3
 *   VOLT <v> / VOLT?          voltage setpoint, 10 mV
 *   CURR <a> / CURR?          current limit, 1 mA
 *   VOLT:LIM <v> / VOLT:LIM?  over voltage protection
 *   CURR:LIM <a> / CURR:LIM?  over current protection
 *   OUTP ON|OFF / OUTP?
 *   SYST:REM / SYST:LOC       remote lock / back to the front panel
 *
 * The serial port is behind a small shim at the top of this file: termios
 * on macOS and Linux, the Win32 comm API on Windows. Everything from the
 * command queue down is the same on all three.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <termios.h>
#endif

#include "psu.h"

#define QMAX        32     /* command queue depth */
#define REPLY_MS    600    /* time allowed for a query reply */
#define MEAS_MS     200    /* measurement poll interval */
#define SETTINGS_MS 2000   /* re-read settings, catches front panel changes */
#define RETRY_MS    2000   /* reconnect interval */
#define MAXTIMEOUTS 4      /* consecutive missed replies before giving up */

typedef enum { K_WRITE, K_IDN, K_MEAS, K_VSET, K_ISET, K_OVP, K_OCP, K_OUT } kind_t;

typedef struct {
    char   cmd[48];
    kind_t kind;
} qent;

/* link private state: one supply per program */
static int    isopen;                 /* the port is open */
static qent   queue[QMAX];
static int    qhead, qtail;
static kind_t awaiting = K_WRITE;     /* K_WRITE: nothing awaited */
static long   deadline;
static int    timeouts;
static long   next_meas, next_settings, next_retry;
static char   rx[256];
static int    rxlen;
static int    meas_count;

/* simulator: output feeds a fixed resistive load */
#define SIM_LOAD_OHMS 8.0

/* ------------------------------------------------------------ serial port
 *
 * find_port   name of a likely USB serial port, or 0 when there is none
 * port_open   open and set up 115200 8N1, no flow control, non-blocking;
 *             on failure fills err with the reason and returns -1
 * port_read   bytes waiting, 0 when none, -1 when the port has gone away
 * port_write  queue bytes for sending
 * port_drain  wait for queued bytes to go out
 * port_close  close the port
 */

#ifdef _WIN32

static HANDLE hcom = INVALID_HANDLE_VALUE;

static long now_ms(void)
{
    static ULONGLONG t0;

    if (!t0) t0 = GetTickCount64();
    return (long)(GetTickCount64() - t0);
}

static const char *win_error(DWORD e)
{
    static char msg[160];
    size_t      n;

    n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, e, 0, msg, sizeof msg - 1, NULL);
    if (!n) snprintf(msg, sizeof msg, "error %lu", (unsigned long)e);
    while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == '.'))
        msg[--n] = 0;
    return msg;
}

/* Walk the Ports device class for USB serial adapters. The supply's CH340
   (USB\VID_1A86&PID_7523) is preferred, then any other USB or FTDI adapter;
   the motherboard's own COM ports are never used. The result is the plain
   COMn name. */
static int find_port(char *out, size_t outlen)
{
    HDEVINFO        set;
    SP_DEVINFO_DATA dev;
    DWORD           i, len, type;
    HKEY            key;
    char            hwid[256], name[32], best[32] = "";
    int             rank, bestrank = 0;

    set = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return 0;
    dev.cbSize = sizeof dev;
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        /* the first id of the hardware id list says what bus it is on */
        memset(hwid, 0, sizeof hwid);
        if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_HARDWAREID, NULL,
                                               (BYTE *)hwid, sizeof hwid - 1, NULL))
            continue;
        if (!strncmp(hwid, "USB\\", 4)) rank = strstr(hwid, "VID_1A86") ? 2 : 1;
        else if (!strncmp(hwid, "FTDIBUS\\", 8)) rank = 1;
        else continue;
        if (rank <= bestrank) continue;
        key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        memset(name, 0, sizeof name);
        len = sizeof name - 1;
        if (RegQueryValueExA(key, "PortName", NULL, &type, (BYTE *)name, &len) == ERROR_SUCCESS
            && type == REG_SZ && !strncmp(name, "COM", 3)) {
            snprintf(best, sizeof best, "%s", name);
            bestrank = rank;
        }
        RegCloseKey(key);
        if (bestrank == 2) break;
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!bestrank) return 0;
    snprintf(out, outlen, "%s", best);
    return 1;
}

static int port_open(const char *path, char *err, size_t errlen)
{
    char         full[160];
    DCB          dcb;
    COMMTIMEOUTS to;
    HANDLE       h;
    DWORD        e;

    /* COM10 and up only open with the device prefix; it is harmless below */
    if (!strncmp(path, "\\\\.\\", 4)) snprintf(full, sizeof full, "%s", path);
    else snprintf(full, sizeof full, "\\\\.\\%s", path);
    h = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        e = GetLastError();
        if (e == ERROR_ACCESS_DENIED)
            snprintf(err, errlen, "%s is in use by another program", path);
        else if (e == ERROR_FILE_NOT_FOUND)
            snprintf(err, errlen, "No %s", path);
        else
            snprintf(err, errlen, "Cannot open %s: %s", path, win_error(e));
        return -1;
    }
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) {
        snprintf(err, errlen, "%s is not a serial port", path);
        CloseHandle(h);
        return -1;
    }
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = TRUE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;   /* as a termios open leaves them */
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) {
        snprintf(err, errlen, "Cannot set up %s: %s", path, win_error(GetLastError()));
        CloseHandle(h);
        return -1;
    }
    /* reads return at once with whatever has arrived, like O_NONBLOCK;
       a write that cannot complete gives up rather than hang the GUI */
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout = MAXDWORD;
    to.WriteTotalTimeoutConstant = 200;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
    hcom = h;
    isopen = 1;
    return 0;
}

static int port_read(char *buf, int len)
{
    DWORD n = 0;

    if (!ReadFile(hcom, buf, (DWORD)len, &n, NULL)) return -1;
    return (int)n;
}

static void port_write(const char *buf, size_t n)
{
    DWORD w;

    WriteFile(hcom, buf, (DWORD)n, &w, NULL);
}

static void port_drain(void)
{
    FlushFileBuffers(hcom);
}

static void port_close(void)
{
    if (isopen) CloseHandle(hcom);
    hcom = INVALID_HANDLE_VALUE;
    isopen = 0;
}

#else /* macOS, Linux */

static int fd = -1;

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Find a likely USB serial port for the supply: the macOS names first, then
   the Linux ones (ttyUSB* for the CH340 or FTDI adapters, ttyACM* for a CDC
   device). Only the names the running system has will match. */
static int find_port(char *out, size_t outlen)
{
    static const char *prefixes[] = {
        "cu.usbserial", "cu.wchusbserial", "cu.SLAB_USBtoUART", "cu.usbmodem",
        "ttyUSB", "ttyACM", NULL
    };
    DIR *d;
    struct dirent *e;
    int i, found = 0;

    d = opendir("/dev");
    if (!d) return 0;
    for (i = 0; prefixes[i] && !found; i++) {
        rewinddir(d);
        while ((e = readdir(d)) != NULL) {
            if (!strncmp(e->d_name, prefixes[i], strlen(prefixes[i]))) {
                snprintf(out, outlen, "/dev/%s", e->d_name);
                found = 1;
                break;
            }
        }
    }
    closedir(d);
    return found;
}

static int port_open(const char *path, char *err, size_t errlen)
{
    struct termios t;
    int f;

    f = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (f < 0) {
        if (errno == EACCES)
            snprintf(err, errlen, "No permission for %s (join the dialout group)", path);
        else
            snprintf(err, errlen, "Cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (tcgetattr(f, &t) < 0) {
        snprintf(err, errlen, "%s is not a serial port", path);
        close(f);
        return -1;
    }
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= (CLOCAL | CREAD);
    t.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    t.c_cflag = (t.c_cflag & ~CSIZE) | CS8;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(f, TCSANOW, &t) < 0) {
        snprintf(err, errlen, "Cannot set up %s: %s", path, strerror(errno));
        close(f);
        return -1;
    }
    tcflush(f, TCIOFLUSH);
    fd = f;
    isopen = 1;
    return 0;
}

static int port_read(char *buf, int len)
{
    ssize_t n = read(fd, buf, (size_t)len);

    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

static void port_write(const char *buf, size_t n)
{
    if (write(fd, buf, n) < 0) { /* the next timeout handles a dead port */ }
}

static void port_drain(void)
{
    tcdrain(fd);
}

static void port_close(void)
{
    if (isopen) close(fd);
    fd = -1;
    isopen = 0;
}

#endif

/* ----------------------------------------------------------- command link */

static double clampq(double v, double lo, double hi, double res)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return floor(v / res + 0.5) * res;
}

static void enqueue(const char *cmd, kind_t kind)
{
    int next = (qtail + 1) % QMAX;

    if (next == qhead) return; /* full: drop, the next poll recovers */
    strncpy(queue[qtail].cmd, cmd, sizeof queue[qtail].cmd - 1);
    queue[qtail].cmd[sizeof queue[qtail].cmd - 1] = 0;
    queue[qtail].kind = kind;
    qtail = next;
}

static void queue_clear(void)
{
    qhead = qtail = 0;
    awaiting = K_WRITE;
    rxlen = 0;
}

static void write_line(const char *cmd)
{
    char   buf[64];
    size_t n;

    if (!isopen) return;
    n = (size_t)snprintf(buf, sizeof buf, "%s\r\n", cmd);
    port_write(buf, n);
}

static void disconnect(psu_state *s, const char *why)
{
    port_close();
    s->connected = 0;
    s->remote = 0;
    s->have_meas = 0;
    queue_clear();
    snprintf(s->status, sizeof s->status, "%s", why);
    next_retry = now_ms() + RETRY_MS;
}

/* Queue the start-up reads: identity and every setting. Read only: the
   output is never switched on by connecting. */
static void queue_settings(void)
{
    enqueue("OUTP?", K_OUT);
    enqueue("VOLT?", K_VSET);
    enqueue("CURR?", K_ISET);
    enqueue("VOLT:LIM?", K_OVP);
    enqueue("CURR:LIM?", K_OCP);
}

static void try_connect(psu_state *s)
{
    char path[128];

    if (!s->port_fixed) {
        if (!find_port(path, sizeof path)) {
            snprintf(s->status, sizeof s->status, "No supply found on USB");
            next_retry = now_ms() + RETRY_MS;
            return;
        }
        snprintf(s->port, sizeof s->port, "%s", path);
    }
    if (port_open(s->port, s->status, sizeof s->status) < 0) {
        next_retry = now_ms() + RETRY_MS;
        return;
    }
    queue_clear();
    timeouts = 0;
    s->idn[0] = 0;
    snprintf(s->status, sizeof s->status, "Connecting to %s", s->port);
    enqueue("*IDN?", K_IDN);
    queue_settings();
    next_meas = now_ms();
    next_settings = now_ms() + SETTINGS_MS;
}

static int parse_onoff(const char *v)
{
    while (*v == ' ') v++;
    return !strncmp(v, "ON", 2) || !strncmp(v, "on", 2) || v[0] == '1';
}

/* Handle one reply line for the awaited query. Returns nonzero on change. */
static int handle_reply(psu_state *s, kind_t kind, char *line)
{
    char  *f[8];
    int    n = 0;
    char  *p;

    switch (kind) {
        case K_IDN:
            snprintf(s->idn, sizeof s->idn, "%s", line);
            s->connected = 1;
            snprintf(s->status, sizeof s->status, "%s", s->port);
            return 1;
        case K_MEAS:
            for (p = strtok(line, ","); p && n < 8; p = strtok(NULL, ",")) f[n++] = p;
            if (n != 7) return 0;
            s->vmeas = atof(f[0]);
            s->imeas = atof(f[1]);
            s->pmeas = atof(f[2]);
            s->ov = parse_onoff(f[3]);
            s->oc = parse_onoff(f[4]);
            s->ot = parse_onoff(f[5]);
            s->mode = atoi(f[6]);
            /* standby means output off; CV or CC means on */
            if (s->mode == PSU_STANDBY) s->out = 0;
            else if (s->mode == PSU_CV || s->mode == PSU_CC) s->out = 1;
            s->have_meas = 1;
            s->connected = 1;
            return 1;
        case K_VSET: s->vset = atof(line); s->have_settings = 1; return 1;
        case K_ISET: s->iset = atof(line); return 1;
        case K_OVP:  s->ovp = atof(line); return 1;
        case K_OCP:  s->ocp = atof(line); return 1;
        case K_OUT:  s->out = parse_onoff(line); return 1;
        default:     return 0;
    }
}

static int sim_poll(psu_state *s)
{
    static long last;
    long t = now_ms();
    double v, i, noise;

    if (t - last < MEAS_MS) return 0;
    last = t;
    if (s->ov || s->oc) s->out = 0;
    if (!s->out) {
        s->vmeas = s->imeas = s->pmeas = 0;
        s->mode = (s->ov || s->oc || s->ot) ? PSU_FAULT : PSU_STANDBY;
    } else {
        noise = ((rand() % 7) - 3) * 0.001;
        if (s->vset / SIM_LOAD_OHMS <= s->iset) {
            v = s->vset;
            i = v / SIM_LOAD_OHMS;
            s->mode = PSU_CV;
        } else {
            i = s->iset;
            v = i * SIM_LOAD_OHMS;
            s->mode = PSU_CC;
        }
        s->vmeas = v > 0 ? v + noise : 0;
        s->imeas = i > 0 ? i + noise / 10 : 0;
        s->pmeas = s->vmeas * s->imeas;
        if (s->vmeas > s->ovp) { s->ov = 1; s->out = 0; s->mode = PSU_FAULT; }
        if (s->imeas > s->ocp) { s->oc = 1; s->out = 0; s->mode = PSU_FAULT; }
    }
    s->have_meas = 1;
    return 1;
}

void psu_init(psu_state *s, const char *port, int sim)
{
    memset(s, 0, sizeof *s);
    s->sim = sim;
    s->vset = 5.0;
    s->iset = 1.0;
    s->ovp = 61.0;
    s->ocp = 10.2;
    if (sim) {
        s->connected = 1;
        s->remote = 1;
        s->have_settings = 1;
        snprintf(s->port, sizeof s->port, "simulator");
        snprintf(s->idn, sizeof s->idn, "Abestop,AT6301,SIMULATED,FV:0.0");
        snprintf(s->status, sizeof s->status, "Simulated supply (%.0f ohm load)", SIM_LOAD_OHMS);
        return;
    }
    if (port && *port) {
        snprintf(s->port, sizeof s->port, "%s", port);
        s->port_fixed = 1;
    }
    try_connect(s);
}

int psu_poll(psu_state *s)
{
    long    t = now_ms();
    char    buf[128];
    int     n;
    char   *nl;
    int     changed = 0;
    qent   *q;

    if (s->sim) return sim_poll(s);

    if (!isopen) {
        if (t >= next_retry) { try_connect(s); changed = 1; }
        return changed;
    }

    /* gather input */
    while ((n = port_read(buf, sizeof buf)) > 0) {
        if (rxlen + n >= (int)sizeof rx) rxlen = 0; /* garbage: drop it */
        memcpy(rx + rxlen, buf, (size_t)n);
        rxlen += n;
    }
    if (n < 0) {
        disconnect(s, "Supply unplugged");
        return 1;
    }

    /* complete lines */
    while ((nl = memchr(rx, '\n', (size_t)rxlen)) != NULL) {
        int len = (int)(nl - rx);
        char line[256];

        memcpy(line, rx, (size_t)len);
        line[len] = 0;
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = 0;
        memmove(rx, nl + 1, (size_t)(rxlen - len - 1));
        rxlen -= len + 1;
        if (awaiting != K_WRITE && line[0]) {
            changed |= handle_reply(s, awaiting, line);
            awaiting = K_WRITE;
            timeouts = 0;
        }
    }

    /* missed reply */
    if (awaiting != K_WRITE && t > deadline) {
        awaiting = K_WRITE;
        if (++timeouts >= MAXTIMEOUTS) {
            disconnect(s, s->connected ? "Supply stopped answering" : "No answer from port");
            return 1;
        }
    }

    /* periodic reads, only when nothing else is waiting */
    if (awaiting == K_WRITE && qhead == qtail) {
        if (t >= next_settings) {
            queue_settings();
            next_settings = t + SETTINGS_MS;
        } else if (t >= next_meas) {
            enqueue("MEAS:ALL:INFO?", K_MEAS);
            next_meas = t + MEAS_MS;
            meas_count++;
        }
    }

    /* send the next command */
    if (awaiting == K_WRITE && qhead != qtail) {
        q = &queue[qhead];
        qhead = (qhead + 1) % QMAX;
        if (q->kind != K_WRITE) {
            rxlen = 0;                /* stale input belongs to nothing */
            awaiting = q->kind;
            deadline = t + REPLY_MS;
        }
        write_line(q->cmd);
    }

    return changed;
}

/* Every change first asserts remote, as the front panel may have taken the
   supply back to local in the meantime. */
static void send_set(psu_state *s, const char *cmd, const char *readback, kind_t kind)
{
    if (!isopen) return;
    enqueue("SYST:REM", K_WRITE);
    s->remote = 1;
    enqueue(cmd, K_WRITE);
    enqueue(readback, kind);
}

void psu_set_voltage(psu_state *s, double v)
{
    char c[48];

    s->vset = clampq(v, 0, PSU_VMAX, 0.01);
    if (s->sim) return;
    snprintf(c, sizeof c, "VOLT %.2f", s->vset);
    send_set(s, c, "VOLT?", K_VSET);
}

void psu_set_current(psu_state *s, double a)
{
    char c[48];

    s->iset = clampq(a, 0, PSU_IMAX, 0.001);
    if (s->sim) return;
    snprintf(c, sizeof c, "CURR %.3f", s->iset);
    send_set(s, c, "CURR?", K_ISET);
}

void psu_set_ovp(psu_state *s, double v)
{
    char c[48];

    s->ovp = clampq(v, 0, PSU_OVPMAX, 0.01);
    if (s->sim) { s->ov = 0; return; }
    snprintf(c, sizeof c, "VOLT:LIM %.2f", s->ovp);
    send_set(s, c, "VOLT:LIM?", K_OVP);
}

void psu_set_ocp(psu_state *s, double a)
{
    char c[48];

    s->ocp = clampq(a, 0, PSU_OCPMAX, 0.001);
    if (s->sim) { s->oc = 0; return; }
    snprintf(c, sizeof c, "CURR:LIM %.3f", s->ocp);
    send_set(s, c, "CURR:LIM?", K_OCP);
}

void psu_set_output(psu_state *s, int on)
{
    s->out = on ? 1 : 0;
    if (s->sim) {
        if (on) s->ov = s->oc = 0;
        return;
    }
    send_set(s, on ? "OUTP ON" : "OUTP OFF", "OUTP?", K_OUT);
}

void psu_local(psu_state *s)
{
    if (s->sim) return;
    if (isopen) enqueue("SYST:LOC", K_WRITE);
    s->remote = 0;
}

void psu_shutdown(psu_state *s)
{
    if (isopen) {
        if (s->remote) {
            write_line("SYST:LOC");
            port_drain();
        }
        port_close();
    }
    s->connected = 0;
    s->remote = 0;
}
