/*
 * AT6301 power supply link: USB virtual serial port, SCPI-like commands.
 *
 * The supply appears on the Mac as /dev/cu.usbserial-* (or cu.wchusbserial*
 * with the WCH driver) and on Linux as /dev/ttyUSB* (the CH340 driver is in
 * the kernel), 115200 8N1, lines ended with CR LF. Set commands send no
 * reply; queries reply with one line.
 *
 * All I/O is non-blocking and driven by psu_poll(), which the GUI calls from
 * a periodic timer, so the window never stalls on the serial port. Only one
 * command is in flight at a time, which also paces writes to the supply.
 */
#ifndef PSU_H
#define PSU_H

#define PSU_VMAX   60.0   /* rated output voltage */
#define PSU_IMAX   10.0   /* rated output current */
#define PSU_OVPMAX 62.0   /* protection limits may sit a little above rating */
#define PSU_OCPMAX 10.5

typedef enum { PSU_STANDBY = 0, PSU_CV = 1, PSU_CC = 2, PSU_FAULT = 3 } psu_mode;

typedef struct {
    /* link */
    int  connected;      /* port open and supply answering */
    int  sim;            /* simulated supply, no hardware */
    int  remote;         /* we have put the supply in remote (SYST:REM) */
    char port[128];      /* device path in use, or wanted */
    int  port_fixed;     /* port given by the user: do not auto-detect */
    char idn[128];       /* *IDN? reply */
    char status[256];    /* short human readable link status */

    /* settings, as last read back (or optimistically set) */
    double vset, iset, ovp, ocp;
    int    out;          /* output enabled */
    int    have_settings;

    /* live measurement */
    double vmeas, imeas, pmeas;
    int    ov, oc, ot;   /* protection trips */
    int    mode;         /* psu_mode */
    int    have_meas;
} psu_state;

/* Open the supply. port NULL or "" auto-detects. sim nonzero simulates. */
void psu_init(psu_state *s, const char *port, int sim);

/* Service the link: call every ~50 ms. Returns nonzero if state changed. */
int psu_poll(psu_state *s);

/* Return the supply to front panel control and close the port. */
void psu_shutdown(psu_state *s);

/* Requests. Values are clamped and quantized to the supply's resolution. */
void psu_set_voltage(psu_state *s, double v);
void psu_set_current(psu_state *s, double a);
void psu_set_ovp(psu_state *s, double v);
void psu_set_ocp(psu_state *s, double a);
void psu_set_output(psu_state *s, int on);
void psu_local(psu_state *s);   /* hand control back to the front panel */

#endif
