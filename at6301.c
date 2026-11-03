/*
 * AT6301 front panel: drive an Abestop AT6301 bench supply from the desktop
 * (macOS or Linux).
 *
 * The window is drawn as the face of the supply: the TFT readout at the
 * top, setting keys and the rotary knob below, and the output switch and
 * terminals along the bottom. Everything is drawn with the AMI graphics
 * calls and scaled to the window, so the panel can be resized.
 *
 * Use:
 *   at6301              find the supply on USB
 *   at6301 -p PORT      use this serial port
 *   at6301 --sim        no hardware: a simulated supply into a load
 *
 * Mouse: click V-SET/I-SET/OVP/OCP to choose the setting, then turn the
 * knob to change the highlighted digit: drag it up or down, use the wheel
 * over the panel, click its left/right half, or press (and hold) the -/+
 * buttons beside it. The arrow keys under the knob move the
 * digit. OUTPUT switches the output. M1..M4 recall a preset; SAVE then an
 * M key stores the present voltage and current there.
 *
 * Keys: v i (choose setting), Tab (next setting), digits and . (type a
 * value, Enter sets, Esc abandons), Up/Down (step), Left/Right (digit),
 * o (output on/off).
 *
 * Graph: GRAPH (or g) turns the readout into a strip chart of measured
 * volts, amps and watts, sampled every 200 ms with 30 minutes kept. Click
 * a legend entry to hide or show its trace, SPAN (or s) to change the time
 * across the chart, CSV (or x) to save the history to the Desktop; c
 * clears the history.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <localdefs.h>
#include <services.h>
#include <graphics.h>

#include "psu.h"

/* the panel is laid out in these units and scaled to the window */
#define PANW 460
#define PANH 720

#define TICK_ID  1
#define TICK     500   /* 50 ms, in 100 us units */
#define SETTLE   150

/* knob centre and radius, panel units */
#define KNOBX 380
#define KNOBY 448
#define KNOBR 48   /* ms of knob rest before a change goes out */

/* settings that can be chosen and edited */
enum { F_V, F_I, F_OVP, F_OCP, NFIELDS };

/* things that can be clicked */
enum {
    H_NONE, H_VSET, H_ISET, H_OVP, H_OCP, H_M1, H_M2, H_M3, H_M4,
    H_SAVE, H_LOCAL, H_LEFT, H_RIGHT, H_KNOB, H_MINUS, H_PLUS, H_OUTPUT,
    H_GRAPH, H_SPAN, H_CSV, H_TV, H_TI, H_TP, H_FIELD0
};

typedef struct { int x1, y1, x2, y2, id; } hitbox;

static psu_state psu;

/* window scaling */

/* clickable areas, rebuilt on each draw */
static hitbox hits[40];
static int    nhits;
static int    pressed = H_NONE;
static int    mx, my;

/* knob dragging and the held -/+ buttons */
#define DRAGSTEP 8     /* panel units of vertical drag per step */
#define REPDELAY 400   /* ms before a held -/+ starts repeating */
#define REPRATE  80    /* ms between repeats */
static int    dragging, drag_y, drag_moved;
static long   repeat_at;

/* editing */
static int    sel = F_V;                      /* chosen setting */
static int    digit[NFIELDS] = { 1, 2, 1, 2 }; /* active digit, 0 = leftmost */
static double pend[NFIELDS];                  /* knob value not yet sent */
static int    pending[NFIELDS];
static long   settle_at;
static char   entry[16];                      /* typed value */
static int    entering;
static int    saving;                         /* SAVE pressed, waiting for M */
static double knob_angle = -2.2;

/* presets */
static double preset_v[4] = { 3.3, 5.0, 12.0, 24.0 };
static double preset_i[4] = { 1.0, 1.0, 2.0, 2.0 };
static char   rcpath[512];

/* measurement history for the graph: a ring of fixed rate samples */
#define HIST_MS 200
#define HIST_N  9000                          /* 30 minutes */
#define GCOLS   328                           /* plot width, panel units */
typedef struct { long t; float v, i, p; char ok; } sample;
static sample hist[HIST_N];
static int    hist_head, hist_len;
static long   hist_next;
static int    graphing;
static int    span_ix = 1;
static const int   span_s[] = { 30, 60, 300, 600, 1800 };
static const char *span_l[] = { "30s", "1m", "5m", "10m", "30m" };
#define NSPANS ((int)(sizeof span_s / sizeof span_s[0]))
static int    show_tr[3] = { 1, 1, 1 };       /* V, I, P */
static char   note[128];                      /* brief message on the graph */
static long   note_until;

/* field layout: integer digits and decimals, as shown on the display */
static const int fint[NFIELDS] = { 2, 2, 2, 2 };
static const int fdec[NFIELDS] = { 2, 3, 2, 3 };
static const double flimit[NFIELDS] = { PSU_VMAX, PSU_IMAX, PSU_OVPMAX, PSU_OCPMAX };
static const char *fname[NFIELDS] = { "V-SET", "I-SET", "OVP", "OCP" };
static const char *funit[NFIELDS] = { "V", "A", "V", "A" };

/* milliseconds since the first call. ami_clock wraps every 48 hours, so whole
   milliseconds are taken off the elapsed count each call and accumulated */
static long now_ms(void)
{
    static ami_long ref;
    static long     acc;
    static int      started;
    ami_long        ms;

    if (!started) { ref = ami_clock(); started = 1; }
    ms = ami_elapsed(ref) / 10;
    if (ms > 0) { ref += ms * 10; acc += ms; }
    return acc;
}

/* a file in the user's directory, or in a subdirectory of it when sub is given */
static void user_file(char *fn, int fnl, const char *sub, const char *name, const char *ext)
{
    char usr[256], dir[512];

    ami_getusr(usr, sizeof usr - 1);
    usr[sizeof usr - 1] = 0;
    if (sub) {
        ami_maknam(dir, sizeof dir - 1, usr, (char *)sub, "");
        dir[sizeof dir - 1] = 0;
    } else strcpy(dir, usr);
    ami_maknam(fn, fnl - 1, dir, (char *)name, (char *)ext);
    fn[fnl - 1] = 0;
}

/* local wall clock time broken into year, month, day, hour, minute, second */
static void local_time(ami_long t, int *y, int *mo, int *d, int *h, int *mi, int *sec)
{
    long days = (long)(t / 86400), rem = (long)(t % 86400), era, doe, yoe, doy, mp;

    if (rem < 0) { rem += 86400; days--; }
    *h = (int)(rem / 3600); *mi = (int)(rem / 60 % 60); *sec = (int)(rem % 60);
    /* civil date from a day count, the day count moved to start on 0000-03-01 */
    days += 10957 + 719468;
    era = (days >= 0 ? days : days - 146096) / 146097;
    doe = days - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*mo <= 2));
}

/* ---------------------------------------------------------------- presets */

static void load_presets(void)
{
    FILE  *f;
    char   line[128];
    int    m;
    double v, a;

    user_file(rcpath, sizeof rcpath, NULL, ".at6301rc", "");
    f = fopen(rcpath, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "M%d %lf %lf", &m, &v, &a) == 3 && m >= 1 && m <= 4) {
            preset_v[m - 1] = v;
            preset_i[m - 1] = a;
        }
    }
    fclose(f);
}

static void save_presets(void)
{
    FILE *f;
    int   m;

    f = fopen(rcpath, "w");
    if (!f) return;
    fprintf(f, "# AT6301 presets: M<n> volts amps\n");
    for (m = 0; m < 4; m++) fprintf(f, "M%d %.2f %.3f\n", m + 1, preset_v[m], preset_i[m]);
    fclose(f);
}

/* ---------------------------------------------------------------- values */

static double field_value(int f)
{
    if (pending[f]) return pend[f];
    switch (f) {
        case F_V:   return psu.vset;
        case F_I:   return psu.iset;
        case F_OVP: return psu.ovp;
        default:    return psu.ocp;
    }
}

static void apply_field(int f, double v)
{
    switch (f) {
        case F_V:   psu_set_voltage(&psu, v); break;
        case F_I:   psu_set_current(&psu, v); break;
        case F_OVP: psu_set_ovp(&psu, v); break;
        default:    psu_set_ocp(&psu, v); break;
    }
    pending[f] = 0;
}

static void flush_pending(void)
{
    int f;

    for (f = 0; f < NFIELDS; f++)
        if (pending[f]) apply_field(f, pend[f]);
}

static double digit_step(int f)
{
    return pow(10.0, fint[f] - 1 - digit[f]);
}

/* turn the knob: step the active digit of the chosen setting */
static void knob(int dir)
{
    double v = field_value(sel) + dir * digit_step(sel);

    if (entering) return;
    if (v < 0) v = 0;
    if (v > flimit[sel]) v = flimit[sel];
    pend[sel] = v;
    pending[sel] = 1;
    settle_at = now_ms() + SETTLE;
    knob_angle += dir * 0.35;
}

static void move_digit(int dir)
{
    int n = fint[sel] + fdec[sel];

    digit[sel] = (digit[sel] + dir + n) % n;
}

static void choose(int f)
{
    if (entering && f != sel) entering = 0;
    sel = f;
}

static void recall(int m)
{
    if (saving) {
        preset_v[m] = field_value(F_V);
        preset_i[m] = field_value(F_I);
        save_presets();
        saving = 0;
        return;
    }
    pending[F_V] = pending[F_I] = 0;
    psu_set_voltage(&psu, preset_v[m]);
    psu_set_current(&psu, preset_i[m]);
}

static void set_output(int on)
{
    flush_pending();
    psu_set_output(&psu, on);
}

/* ---------------------------------------------------------------- history */

static void set_note(const char *s)
{
    snprintf(note, sizeof note, "%s", s);
    note_until = now_ms() + 4000;
}

/* take a sample when one is due; returns nonzero if one was taken */
static int record_sample(void)
{
    long    t = now_ms();
    sample *s;

    if (t < hist_next) return 0;
    /* stay on a fixed grid, but do not try to catch up after a stall */
    hist_next = (hist_next && t - hist_next < HIST_MS) ? hist_next + HIST_MS : t + HIST_MS;
    s = &hist[hist_head];
    s->t = t;
    s->ok = (char)(psu.connected && psu.have_meas);
    s->v = s->ok ? (float)psu.vmeas : 0;
    s->i = s->ok ? (float)psu.imeas : 0;
    s->p = s->ok ? (float)psu.pmeas : 0;
    hist_head = (hist_head + 1) % HIST_N;
    if (hist_len < HIST_N) hist_len++;
    return 1;
}

static void clear_history(void)
{
    hist_len = 0;
    set_note("History cleared");
}

/* write the whole history to a CSV file on the desktop */
static void save_csv(void)
{
    char       path[600], name[64], msg[128];
    ami_long   wall = ami_local(ami_time());
    long       mono = now_ms();
    FILE      *f;
    const char *where = "Desktop/";
    int        k, n = 0, y, mo, d, h, mi, sec;

    local_time(wall, &y, &mo, &d, &h, &mi, &sec);
    snprintf(name, sizeof name, "AT6301_%04d%02d%02d_%02d%02d%02d", y, mo, d, h, mi, sec);
    user_file(path, sizeof path, "Desktop", name, "csv");
    f = fopen(path, "w");
    if (!f) {
        /* no Desktop folder (some Linux desktops): the home directory instead */
        where = "";
        user_file(path, sizeof path, NULL, name, "csv");
        f = fopen(path, "w");
    }
    if (!f) { set_note("Could not write the CSV file"); return; }
    fprintf(f, "time,seconds,volts,amps,watts\n");
    for (k = hist_len - 1; k >= 0; k--) {
        sample *s = &hist[(hist_head - 1 - k + HIST_N) % HIST_N];
        double  age = (mono - s->t) / 1000.0;
        char    ts[32];

        if (!s->ok) continue;
        local_time(wall - (ami_long)(age + 0.5), &y, &mo, &d, &h, &mi, &sec);
        snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, sec);
        fprintf(f, "%s,%.1f,%.3f,%.3f,%.3f\n", ts, (s->t - hist[(hist_head - hist_len + HIST_N)
                % HIST_N].t) / 1000.0, s->v, s->i, s->p);
        n++;
    }
    fclose(f);
    snprintf(msg, sizeof msg, "Saved %d samples to %s%s.csv", n, where, name);
    set_note(msg);
}

/* ---------------------------------------------------------------- drawing */

/* The panel is laid out in panel units, PANW by PANH, and drawn on an integer
   grid GRID times finer, so fractional positions survive. AMI's view scale and
   offset then fit the grid to the window, text and line widths included. */
#define GRID 10

static int X(double x) { return (int)lround(x * GRID); }
static int Y(double y) { return (int)lround(y * GRID); }
static int S(double d) { int v = (int)lround(d * GRID); return v < 1 ? 1 : v; }

static ami_long cv(int v) { return (ami_long)v * (LONG_MAX / 255); }
static void color(int r, int g, int b) { ami_fcolorg(stdout, cv(r), cv(g), cv(b)); }

static void addhit(double x1, double y1, double x2, double y2, int id)
{
    if (nhits >= (int)(sizeof hits / sizeof hits[0])) return;
    hits[nhits].x1 = X(x1); hits[nhits].y1 = Y(y1);
    hits[nhits].x2 = X(x2); hits[nhits].y2 = Y(y2);
    hits[nhits].id = id;
    nhits++;
}

static int hit(int x, int y)
{
    int i;

    for (i = nhits - 1; i >= 0; i--)
        if (x >= hits[i].x1 && x <= hits[i].x2 && y >= hits[i].y1 && y <= hits[i].y2)
            return hits[i].id;
    return H_NONE;
}

static void frect(double x1, double y1, double x2, double y2)
{
    ami_frect(stdout, X(x1), Y(y1), X(x2), Y(y2));
}

static void frrect(double x1, double y1, double x2, double y2, double r)
{
    ami_frrect(stdout, X(x1), Y(y1), X(x2), Y(y2), S(r), S(r));
}

static void rrect(double x1, double y1, double x2, double y2, double r, double w)
{
    ami_linewidth(stdout, S(w));
    ami_rrect(stdout, X(x1), Y(y1), X(x2), Y(y2), S(r), S(r));
}

static void fcircle(double cx, double cy, double r)
{
    ami_fellipse(stdout, X(cx - r), Y(cy - r), X(cx + r), Y(cy + r));
}

static void line(double x1, double y1, double x2, double y2, double w)
{
    ami_linewidth(stdout, S(w));
    ami_line(stdout, X(x1), Y(y1), X(x2), Y(y2));
}

static void font(int face, double size, int bold)
{
    ami_font(stdout, face);
    ami_fontsiz(stdout, S(size));
    ami_bold(stdout, bold);
}

/* width of a string in panel units */
static double tw(const char *s) { return (double)ami_strsiz(stdout, s) / GRID; }

/* text with its top left at x,y; align -1 left, 0 centre, 1 right of x */
static void text(double x, double y, int align, const char *s)
{
    double w = tw(s);

    if (align == 0) x -= w / 2;
    else if (align > 0) x -= w;
    ami_cursorg(stdout, X(x), Y(y));
    /* text background off: the second page does not keep it reliably */
    ami_binvis(stdout);
    fputs(s, stdout);
}

/* text centred in a box */
static void ctext(double x1, double y1, double x2, double y2, const char *s)
{
    double h = (double)ami_chrsizy(stdout) / GRID;

    text((x1 + x2) / 2, (y1 + y2) / 2 - h / 2, 0, s);
}

static void fmt_field(int f, double v, char *buf, size_t len)
{
    snprintf(buf, len, "%0*.*f", fint[f] + 1 + fdec[f], fdec[f], v);
}

/* round up to 1, 2 or 5 times a power of ten, so the grid reads well */
static double nice_ceil(double x, double least)
{
    double e, f;

    if (x < least) x = least;
    e = pow(10, floor(log10(x)));
    f = x / e;
    f = f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10;
    return f * e;
}

static void fmt_axis(double v, char *buf, size_t len)
{
    snprintf(buf, len, "%.4g", v);
}

/* the readout area as a strip chart of the measurements */
static void draw_graph(void)
{
    static float cmin[3][GCOLS], cmax[3][GCOLS], csum[3][GCOLS];
    static int   cn[GCOLS];
    static char  cbrk[GCOLS];
    static const int tcol[3][3] = { { 250, 215, 50 }, { 70, 210, 240 }, { 120, 230, 120 } };
    static const char *tunit[3] = { "V", "A", "W" };
    const double px1 = 66, px2 = px1 + GCOLS, py1 = 120, py2 = 244;
    double  vmax[3] = { 0, 0, 0 }, fs[3], val[3], x, y, px[3] = { 0 }, py[3] = { 0 };
    long    now = now_ms(), spanms = span_s[span_ix] * 1000L;
    int     k, c, tr, have[3];
    char    buf[48];

    /* legend: live values, each one switches its trace */
    val[0] = psu.vmeas; val[1] = psu.imeas; val[2] = psu.pmeas;
    for (tr = 0; tr < 3; tr++) {
        double lx = 38 + tr * 86;

        if (psu.connected && psu.have_meas)
            snprintf(buf, sizeof buf, tr == 2 ? "%.2f%s" : "%.3f%s", val[tr], tunit[tr]);
        else snprintf(buf, sizeof buf, "--.---%s", tunit[tr]);
        if (show_tr[tr]) color(tcol[tr][0], tcol[tr][1], tcol[tr][2]);
        else color(70, 75, 90);
        frect(lx, 102, lx + 10, 110);
        font(AMI_FONT_TERM, 16, TRUE);
        text(lx + 14, 97, -1, buf);
        addhit(lx, 96, lx + 82, 116, H_TV + tr);
    }
    font(AMI_FONT_SIGN, 11, TRUE);
    color(40, 52, 85);
    frrect(300, 98, 344, 114, 3);
    frrect(350, 98, 424, 114, 3);
    color(200, 210, 230);
    ctext(300, 98, 344, 114, "CSV");
    snprintf(buf, sizeof buf, "SPAN %s", span_l[span_ix]);
    ctext(350, 98, 424, 114, buf);
    addhit(300, 98, 344, 114, H_CSV);
    addhit(350, 98, 424, 114, H_SPAN);

    /* bin the samples in the span into plot columns */
    memset(cn, 0, sizeof cn);
    memset(cbrk, 0, sizeof cbrk);
    for (k = 0; k < hist_len; k++) {
        sample *s = &hist[(hist_head - 1 - k + HIST_N) % HIST_N];
        long    age = now - s->t;
        float   sv[3];

        if (age > spanms) break;
        c = GCOLS - 1 - (int)((double)age * GCOLS / spanms);
        if (c < 0) c = 0;
        if (!s->ok) { cbrk[c] = 1; continue; }
        sv[0] = s->v; sv[1] = s->i; sv[2] = s->p;
        for (tr = 0; tr < 3; tr++) {
            if (!cn[c]) { cmin[tr][c] = cmax[tr][c] = sv[tr]; csum[tr][c] = 0; }
            if (sv[tr] < cmin[tr][c]) cmin[tr][c] = sv[tr];
            if (sv[tr] > cmax[tr][c]) cmax[tr][c] = sv[tr];
            csum[tr][c] += sv[tr];
            if (sv[tr] > vmax[tr]) vmax[tr] = sv[tr];
        }
        cn[c]++;
    }
    fs[0] = nice_ceil(vmax[0] * 1.05, 1.0);
    fs[1] = nice_ceil(vmax[1] * 1.05, 0.1);
    fs[2] = nice_ceil(vmax[2] * 1.05, 1.0);

    /* grid and scales: volts on the left, amps on the right */
    color(14, 20, 36);
    frect(px1, py1, px2, py2);
    font(AMI_FONT_SIGN, 10, FALSE);
    for (k = 0; k <= 5; k++) {
        y = py2 - (py2 - py1) * k / 5;
        color(34, 44, 70);
        line(px1, y, px2, y, k == 0 ? 1.5 : 1);
        if (show_tr[0]) {
            color(200, 175, 60);
            fmt_axis(fs[0] * k / 5, buf, sizeof buf);
            text(px1 - 4, y - 6, 1, buf);
        }
        if (show_tr[1]) {
            color(70, 180, 205);
            fmt_axis(fs[1] * k / 5, buf, sizeof buf);
            text(px2 + 4, y - 6, -1, buf);
        }
    }
    for (k = 0; k <= 6; k++) {
        x = px1 + (px2 - px1) * k / 6;
        color(34, 44, 70);
        line(x, py1, x, py2, 1);
    }
    color(130, 140, 165);
    snprintf(buf, sizeof buf, "-%s", span_l[span_ix]);
    text(px1, py2 + 2, -1, buf);
    text(px2, py2 + 2, 1, "now");
    if (show_tr[2]) {
        color(100, 190, 100);
        fmt_axis(fs[2], buf, sizeof buf);
        text(px1 + 4, py1 + 2, -1, "W 0-");
        text(px1 + 4 + tw("W 0-"), py1 + 2, -1, buf);
    }

    /* traces: power under current under voltage */
    for (tr = 2; tr >= 0; tr--) {
        if (!show_tr[tr]) continue;
        have[tr] = 0;
        color(tcol[tr][0], tcol[tr][1], tcol[tr][2]);
        for (c = 0; c < GCOLS; c++) {
            if (cbrk[c]) have[tr] = 0;
            if (!cn[c]) continue;
            x = px1 + c;
            y = py2 - (py2 - py1) * (csum[tr][c] / cn[c]) / fs[tr];
            /* the spread within a column, when there is one */
            if (cmax[tr][c] - cmin[tr][c] > fs[tr] / (py2 - py1))
                line(x, py2 - (py2 - py1) * cmin[tr][c] / fs[tr], x,
                     py2 - (py2 - py1) * cmax[tr][c] / fs[tr], 1);
            if (have[tr]) line(px[tr], py[tr], x, y, 1.6);
            else line(x, y, x, y, 1.6);
            px[tr] = x;
            py[tr] = y;
            have[tr] = 1;
        }
    }

    if (note[0] && now < note_until) {
        font(AMI_FONT_SIGN, 12, TRUE);
        color(40, 52, 85);
        frect(px1 + 20, py1 + 50, px2 - 20, py1 + 74);
        color(235, 235, 245);
        ctext(px1 + 20, py1 + 50, px2 - 20, py1 + 74, note);
    }
}

/* the big readouts: measured while the output is on; with it off there is
   nothing to measure, so show the settings, as the supply itself does */
static void draw_readouts(int showset)
{
    char buf[32];

    font(AMI_FONT_TERM, 64, TRUE);
    if (showset) snprintf(buf, sizeof buf, "%05.2f", field_value(F_V));
    else if (psu.connected && psu.have_meas) snprintf(buf, sizeof buf, "%06.3f", psu.vmeas);
    else snprintf(buf, sizeof buf, "--.---");
    color(250, 215, 50);
    text(378, 96, 1, buf);
    if (showset) snprintf(buf, sizeof buf, "%05.3f", field_value(F_I));
    else if (psu.connected && psu.have_meas) snprintf(buf, sizeof buf, "%06.3f", psu.imeas);
    else snprintf(buf, sizeof buf, "--.---");
    color(70, 210, 240);
    text(378, 166, 1, buf);
    font(AMI_FONT_SIGN, 36, TRUE);
    color(250, 215, 50);
    text(386, 116, -1, "V");
    color(70, 210, 240);
    text(386, 186, -1, "A");
    if (showset) {
        font(AMI_FONT_SIGN, 13, TRUE);
        color(150, 160, 180);
        text(40, 104, -1, "SET");
        text(40, 174, -1, "SET");
    }

}

/* the TFT screen */
static void draw_display(void)
{
    char   buf[160];
    double x, cw, top;
    int    f, ci, fault, showset;
    const char *mode;

    /* bezel and glass */
    color(12, 12, 14);
    frrect(22, 58, 438, 322, 8);
    color(6, 10, 22);
    frect(30, 64, 430, 316);

    /* status bar */
    color(22, 30, 54);
    frect(30, 64, 430, 92);
    font(AMI_FONT_SIGN, 15, TRUE);
    if (!psu.connected) { mode = "----"; color(110, 110, 120); }
    else if (psu.mode == PSU_CV) { mode = "CV"; color(40, 200, 90); }
    else if (psu.mode == PSU_CC) { mode = "CC"; color(235, 70, 60); }
    else if (psu.mode == PSU_FAULT) { mode = "PROT"; color(235, 70, 60); }
    else { mode = "OFF"; color(120, 120, 130); }
    frrect(36, 68, 90, 88, 3);
    color(6, 10, 22);
    ctext(36, 68, 90, 88, mode);

    font(AMI_FONT_SIGN, 12, FALSE);
    color(160, 175, 200);
    if (psu.sim) snprintf(buf, sizeof buf, "SIMULATED");
    else if (psu.connected) snprintf(buf, sizeof buf, "%s", psu.port + (strncmp(psu.port, "/dev/", 5) ? 0 : 5));
    else snprintf(buf, sizeof buf, "NO DEVICE");
    text(100, 71, -1, buf);
    font(AMI_FONT_SIGN, 11, TRUE);
    color(graphing ? 70 : 40, graphing ? 110 : 52, graphing ? 200 : 85);
    frrect(222, 69, 280, 87, 3);
    color(graphing ? 255 : 200, graphing ? 255 : 210, graphing ? 255 : 230);
    ctext(222, 69, 280, 87, "GRAPH");
    addhit(222, 69, 280, 87, H_GRAPH);
    font(AMI_FONT_SIGN, 13, TRUE);
    if (psu.out) { color(40, 200, 90); text(330, 70, 0, "OUT ON"); }
    else { color(110, 110, 120); text(330, 70, 0, "OUT OFF"); }
    color(psu.remote ? 230 : 110, psu.remote ? 180 : 110, psu.remote ? 40 : 120);
    text(422, 70, 1, psu.remote ? "RMT" : "LOC");

    showset = psu.connected && !psu.out;
    if (graphing) draw_graph();
    else draw_readouts(showset);

    /* power, or a fault banner, or the link status */
    fault = psu.connected && (psu.ov || psu.oc || psu.ot);
    if (fault) {
        color(200, 30, 30);
        frect(36, 236, 424, 262);
        font(AMI_FONT_SIGN, 16, TRUE);
        color(255, 255, 255);
        snprintf(buf, sizeof buf, "%s%s%sTRIPPED - set OUTPUT on to reset",
                 psu.ov ? "OVP " : "", psu.oc ? "OCP " : "", psu.ot ? "OTP " : "");
        ctext(36, 236, 424, 262, buf);
    } else if (!psu.connected) {
        if (graphing) { color(30, 18, 18); frect(36, 236, 424, 262); }
        font(AMI_FONT_SIGN, 14, FALSE);
        color(235, 120, 80);
        ctext(36, 236, 424, 262, psu.status);
    } else if (!graphing) {
        font(AMI_FONT_TERM, 26, TRUE);
        color(120, 230, 120);
        snprintf(buf, sizeof buf, "%07.2f", psu.have_meas ? psu.pmeas : 0.0);
        text(378, 236, 1, buf);
        font(AMI_FONT_SIGN, 20, TRUE);
        text(386, 240, -1, "W");
    }

    /* the four settings along the bottom of the screen */
    for (f = 0; f < NFIELDS; f++) {
        double bx1 = 36 + f * 98, bx2 = bx1 + 92;

        top = 266;
        if (f == sel) {
            color(30, 50, 95);
            frect(bx1, top, bx2, 312);
            color(220, 220, 240);
            rrect(bx1, top, bx2, 312, 3, 1.5);
        }
        addhit(bx1, top, bx2, 312, H_FIELD0 + f);
        font(AMI_FONT_SIGN, 11, TRUE);
        color(f == sel ? 230 : 140, f == sel ? 230 : 150, f == sel ? 240 : 170);
        text(bx1 + 6, top + 3, -1, fname[f]);
        if (pending[f]) { color(250, 180, 40); text(bx2 - 6, top + 3, 1, "*"); }

        font(AMI_FONT_TERM, 19, TRUE);
        if (f == sel && entering) {
            snprintf(buf, sizeof buf, "%s_", entry);
            color(255, 255, 255);
            text(bx1 + 6, top + 20, -1, buf);
            continue;
        }
        if (!psu.connected && !psu.sim) {
            snprintf(buf, sizeof buf, "--.--");
        } else {
            fmt_field(f, field_value(f), buf, sizeof buf);
        }
        color(f == F_V || f == F_OVP ? 250 : 70, f == F_V || f == F_OVP ? 215 : 210,
              f == F_V || f == F_OVP ? 50 : 240);
        x = bx1 + 6;
        text(x, top + 20, -1, buf);
        /* underline the digit the knob turns */
        if (f == sel) {
            cw = tw("0");
            ci = digit[f] + (digit[f] >= fint[f] ? 1 : 0);
            color(255, 255, 255);
            frect(x + ci * cw, top + 41, x + (ci + 1) * cw - 1, top + 43);
        }
        font(AMI_FONT_SIGN, 12, FALSE);
        color(150, 160, 180);
        text(bx2 - 6, top + 26, 1, funit[f]);
    }
}

/* one front panel key */
static void key(double x1, double y1, double x2, double y2, const char *label, int id, int lit)
{
    int down = pressed == id;

    color(18, 18, 20);
    frrect(x1, y1 + 3, x2, y2 + 3, 6);
    if (lit) color(70, 110, 170);
    else color(down ? 50 : 72, down ? 52 : 75, down ? 58 : 84);
    frrect(x1, y1 + (down ? 2 : 0), x2, y2 + (down ? 2 : 0), 6);
    color(255, 255, 255);
    font(AMI_FONT_SIGN, 14, TRUE);
    ctext(x1, y1 + (down ? 2 : 0), x2, y2 + (down ? 2 : 0), label);
    addhit(x1, y1, x2, y2 + 3, id);
}

/* round -/+ button beside the knob */
static void stepbutton(double cx, double cy, const char *label, int id)
{
    int down = pressed == id;

    color(18, 18, 20);
    fcircle(cx, cy + 2, 12);
    color(down ? 50 : 72, down ? 52 : 75, down ? 58 : 84);
    fcircle(cx, cy + (down ? 2 : 0), 12);
    color(255, 255, 255);
    font(AMI_FONT_SIGN, 18, TRUE);
    ctext(cx - 12, cy - 12 + (down ? 2 : 0), cx + 12, cy + 12 + (down ? 2 : 0), label);
    addhit(cx - 12, cy - 12, cx + 12, cy + 14, id);
}

static void draw_knob(void)
{
    double cx = KNOBX, cy = KNOBY, r = KNOBR;
    int    i;
    double a;

    /* skirt with scale ticks */
    color(110, 112, 118);
    for (i = 0; i < 24; i++) {
        a = i * M_PI / 12;
        line(cx + cos(a) * (r + 6), cy + sin(a) * (r + 6),
             cx + cos(a) * (r + 12), cy + sin(a) * (r + 12), 1.5);
    }
    color(20, 20, 22);
    fcircle(cx, cy + 3, r);
    color(60, 62, 68);
    fcircle(cx, cy, r);
    color(84, 86, 94);
    fcircle(cx, cy, r - 8);
    color(100, 102, 110);
    fcircle(cx - 6, cy - 6, r - 26);
    /* grip ridges */
    color(45, 46, 50);
    for (i = 0; i < 36; i++) {
        a = i * M_PI / 18;
        line(cx + cos(a) * (r - 4), cy + sin(a) * (r - 4), cx + cos(a) * r, cy + sin(a) * r, 2);
    }
    /* pointer dimple */
    color(230, 230, 235);
    fcircle(cx + cos(knob_angle) * (r - 16), cy + sin(knob_angle) * (r - 16), 5);

    font(AMI_FONT_SIGN, 11, FALSE);
    color(170, 172, 180);
    text(cx, cy + r + 16, 0, "ADJUST");
    addhit(cx - r, cy - r, cx + r, cy + r, H_KNOB);
    stepbutton(cx - r - 2, cy + r + 10, "-", H_MINUS);
    stepbutton(cx + r + 2, cy + r + 10, "+", H_PLUS);
}

static void draw_bottom(void)
{
    char buf[32];

    /* power rocker: the real one is mains, so this is decoration */
    color(16, 16, 18);
    frrect(30, 600, 86, 676, 6);
    color(40, 40, 44);
    frrect(36, 606, 80, 670, 4);
    color(60, 62, 66);
    frrect(36, 606, 80, 636, 4);
    font(AMI_FONT_SIGN, 14, TRUE);
    color(200, 200, 205);
    ctext(36, 606, 80, 636, "I");
    ctext(36, 638, 80, 670, "O");
    font(AMI_FONT_SIGN, 11, FALSE);
    color(170, 172, 180);
    text(58, 682, 0, "POWER");

    /* USB charge port */
    color(16, 16, 18);
    frect(104, 626, 150, 646);
    color(70, 110, 190);
    frect(108, 630, 146, 636);
    color(170, 172, 180);
    text(127, 652, 0, "5V 1A");

    /* output key with its lamp */
    color(psu.out ? 60 : 30, psu.out ? 240 : 50, psu.out ? 90 : 34);
    fcircle(208, 598, 6);
    font(AMI_FONT_SIGN, 11, FALSE);
    color(170, 172, 180);
    text(208, 682, 0, "ON/OFF");
    key(170, 612, 246, 668, "OUTPUT", H_OUTPUT, psu.out);

    /* terminals: positive, negative, earth */
    {
        static const struct { double x; int r, g, b; const char *l; } t[3] = {
            { 290, 200, 30, 30, "+" }, { 346, 25, 25, 28, "-" }, { 402, 30, 150, 60, "GND" }
        };
        int i;

        for (i = 0; i < 3; i++) {
            color(14, 14, 16);
            fcircle(t[i].x, 642, 25);
            color(t[i].r, t[i].g, t[i].b);
            fcircle(t[i].x, 640, 22);
            color(t[i].r + 40 > 255 ? 255 : t[i].r + 40, t[i].g + 40 > 255 ? 255 : t[i].g + 40,
                  t[i].b + 40 > 255 ? 255 : t[i].b + 40);
            fcircle(t[i].x - 5, 634, 8);
            color(10, 10, 10);
            fcircle(t[i].x, 640, 5);
            font(AMI_FONT_SIGN, 13, TRUE);
            color(200, 200, 205);
            text(t[i].x, 676, 0, t[i].l);
        }
        font(AMI_FONT_SIGN, 10, FALSE);
        color(150, 152, 160);
        snprintf(buf, sizeof buf, "0-%.0fV  0-%.0fA", PSU_VMAX, PSU_IMAX);
        text(346, 598, 0, buf);
    }
}

static void redraw(void)
{
    static int buf = 1;
    int    w, h, k;
    double scale;
    double ksx = 24, ksy = 346, kw = 60, kh = 42, kg = 8;
    static const char *klab[3][4] = {
        { "V-SET", "I-SET", "OVP", "OCP" },
        { "M1", "M2", "M3", "M4" },
        { "SAVE", "LOCAL", "<", ">" }
    };
    static const int kid[3][4] = {
        { H_VSET, H_ISET, H_OVP, H_OCP },
        { H_M1, H_M2, H_M3, H_M4 },
        { H_SAVE, H_LOCAL, H_LEFT, H_RIGHT }
    };
    int r, c, lit;

    /* draw off screen, then show it: no flicker at the readout rate */
    buf = 3 - buf;
    ami_select(stdout, buf, 3 - buf);
    /* text and cursor modes belong to each screen, not the window */
    ami_auto(stdout, FALSE);
    ami_curvis(stdout, FALSE);
    ami_binvis(stdout);

    /* clear the whole window in pixels, then fit the panel grid to it, centred */
    ami_viewoffg(stdout, 0, 0);
    ami_viewscale(stdout, 1, 1);
    w = (int)ami_maxxg(stdout);
    h = (int)ami_maxyg(stdout);
    /* the backdrop is also the background, which AMI paints space a resize
       uncovers with before the next frame */
    ami_bcolorg(stdout, cv(30), cv(31), cv(34));
    ami_fcolorg(stdout, cv(30), cv(31), cv(34));
    ami_frect(stdout, 1, 1, w, h);
    scale = fmin((double)w / (PANW * GRID), (double)h / (PANH * GRID));
    ami_viewoffg(stdout, (ami_long)((w - PANW * GRID * scale) / 2),
                 (ami_long)((h - PANH * GRID * scale) / 2));
    ami_viewscale(stdout, (float)scale, (float)scale);
    nhits = 0;

    /* the case */
    color(20, 20, 22);
    frrect(2, 2, PANW - 2, PANH - 2, 18);
    color(46, 48, 53);
    frrect(8, 8, PANW - 8, PANH - 8, 14);

    font(AMI_FONT_SIGN, 24, TRUE);
    color(235, 235, 240);
    text(24, 20, -1, "ABESTOP");
    font(AMI_FONT_SIGN, 12, FALSE);
    color(170, 172, 180);
    text(436, 18, 1, "AT6301 DC POWER SUPPLY");
    text(436, 34, 1, "60V 10A 300W");

    draw_display();

    /* keys */
    for (r = 0; r < 3; r++)
        for (c = 0; c < 4; c++) {
            k = kid[r][c];
            lit = (r == 0 && c == sel) || (k == H_SAVE && saving);
            key(ksx + c * (kw + kg), ksy + r * (kh + 18), ksx + c * (kw + kg) + kw,
                ksy + r * (kh + 18) + kh, klab[r][c], k, lit);
        }
    font(AMI_FONT_SIGN, 10, FALSE);
    color(150, 152, 160);
    text(ksx, ksy + 3 * (kh + 18) - 12, -1,
         saving ? "Press M1-M4 to store V/I" : "M: recall   SAVE+M: store");

    draw_knob();

    /* separator above the bottom row */
    color(36, 37, 41);
    frect(20, 582, PANW - 20, 584);

    draw_bottom();

    ami_select(stdout, buf, buf);
}

/* ---------------------------------------------------------------- input */

static void click(int id)
{
    switch (id) {
        case H_VSET: case H_FIELD0 + F_V:   choose(F_V); break;
        case H_ISET: case H_FIELD0 + F_I:   choose(F_I); break;
        case H_OVP:  case H_FIELD0 + F_OVP: choose(F_OVP); break;
        case H_OCP:  case H_FIELD0 + F_OCP: choose(F_OCP); break;
        case H_M1: case H_M2: case H_M3: case H_M4: recall(id - H_M1); break;
        case H_SAVE:   saving = !saving; break;
        case H_LOCAL:  flush_pending(); psu_local(&psu); break;
        case H_LEFT:   move_digit(-1); break;
        case H_RIGHT:  move_digit(1); break;
        case H_OUTPUT: set_output(!psu.out); break;
        case H_GRAPH:  graphing = !graphing; break;
        case H_SPAN:   span_ix = (span_ix + 1) % NSPANS; break;
        case H_CSV:    save_csv(); break;
        case H_TV: case H_TI: case H_TP: show_tr[id - H_TV] = !show_tr[id - H_TV]; break;
        default: break;
    }
}

static void start_entry(char ch)
{
    if (!entering) { entering = 1; entry[0] = 0; }
    if (strlen(entry) < 7 && !(ch == '.' && strchr(entry, '.'))) {
        size_t n = strlen(entry);

        entry[n] = ch;
        entry[n + 1] = 0;
    }
}

static void finish_entry(void)
{
    if (entering && entry[0]) {
        double v = atof(entry);

        if (v > flimit[sel]) v = flimit[sel];
        apply_field(sel, v);
    }
    entering = 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-p port] [--sim]\n", prog);
    exit(1);
}

int main(int argc, char *argv[])
{
    ami_evtrec er;
    ami_long   wx, wy;
    const char *port = NULL;
    int sim = 0, i, id, dirty;
    char title[160];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim") || !strcmp(argv[i], "-s")) sim = 1;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = argv[++i];
        else usage(argv[0]);
    }

    load_presets();
    psu_init(&psu, port, sim);

    ami_autohold(FALSE);
    ami_auto(stdout, FALSE);
    ami_curvis(stdout, FALSE);
    ami_binvis(stdout);
    snprintf(title, sizeof title, "AT6301%s", sim ? " (simulated)" : "");
    ami_title(stdout, title);
    /* the buffer follows the window, so the scroll bars a buffer larger than
       the client calls for would only flash on each step of a shrink */
    ami_autobuf(stdout, FALSE);
    ami_winclientg(stdout, PANW, PANH, &wx, &wy,
                   BIT(ami_wmframe) | BIT(ami_wmsize) | BIT(ami_wmsysbar));
    ami_setsizg(stdout, wx, wy);
    ami_timer(stdout, TICK_ID, TICK, TRUE);
    redraw();

    do {
        ami_event(stdin, &er);
        dirty = 1;
        switch (er.etype) {
            case ami_ettim:
                dirty = psu_poll(&psu);
                if (record_sample() && graphing) dirty = 1;
                if (note_until && now_ms() >= note_until) { note_until = 0; dirty = 1; }
                if (settle_at && now_ms() >= settle_at) {
                    settle_at = 0;
                    flush_pending();
                    dirty = 1;
                }
                /* a held -/+ repeats while the pointer stays on it */
                if ((pressed == H_MINUS || pressed == H_PLUS) && repeat_at &&
                    now_ms() >= repeat_at && hit(mx, my) == pressed) {
                    knob(pressed == H_PLUS ? 1 : -1);
                    repeat_at = now_ms() + REPRATE;
                    dirty = 1;
                }
                break;
            case ami_etmoumovg:
                /* pointer in grid units, through the view transform */
                mx = (int)ami_scalex(stdout, er.moupxg);
                my = (int)ami_scaley(stdout, er.moupyg);
                dirty = 0;
                /* dragging the knob: up turns it up, down turns it down */
                if (dragging) {
                    int step = S(DRAGSTEP);

                    while (drag_y - my >= step) { knob(1); drag_y -= step; drag_moved = dirty = 1; }
                    while (my - drag_y >= step) { knob(-1); drag_y += step; drag_moved = dirty = 1; }
                }
                break;
            case ami_etmouba:
                if (er.amoubn == 4) { knob(1); break; }
                if (er.amoubn == 5) { knob(-1); break; }
                if (er.amoubn != 1) { dirty = 0; break; }
                id = hit(mx, my);
                if (id == H_KNOB) {
                    dragging = 1;
                    drag_y = my;
                    drag_moved = 0;
                    dirty = 0;
                } else {
                    pressed = id;
                    if (id == H_MINUS || id == H_PLUS) {
                        knob(id == H_PLUS ? 1 : -1);
                        repeat_at = now_ms() + REPDELAY;
                    }
                }
                break;
            case ami_etmoubd:
                if (er.dmoubn == 1 && dragging) {
                    /* a click without a drag steps by the side clicked */
                    if (!drag_moved) knob(mx < X(KNOBX) ? -1 : 1);
                    dragging = 0;
                } else if (er.dmoubn == 1 && pressed != H_NONE) {
                    if (hit(mx, my) == pressed) click(pressed);
                    pressed = H_NONE;
                    repeat_at = 0;
                } else dirty = 0;
                break;
            case ami_etchar:
                if ((er.echar >= '0' && er.echar <= '9') || er.echar == '.') start_entry(er.echar);
                else if (er.echar == 'v' || er.echar == 'V') choose(F_V);
                else if (er.echar == 'i' || er.echar == 'I') choose(F_I);
                else if (er.echar == 'o' || er.echar == 'O') set_output(!psu.out);
                else if (er.echar == 'g' || er.echar == 'G') graphing = !graphing;
                else if (er.echar == 's' || er.echar == 'S') span_ix = (span_ix + 1) % NSPANS;
                else if (er.echar == 'x' || er.echar == 'X') save_csv();
                else if (er.echar == 'c' || er.echar == 'C') clear_history();
                else if (er.echar == '+' || er.echar == '=') knob(1);
                else if (er.echar == '-') knob(-1);
                else dirty = 0;
                break;
            case ami_etenter: finish_entry(); break;
            case ami_etcan:   entering = 0; saving = 0; break;
            case ami_etdelcb:
                if (entering && entry[0]) entry[strlen(entry) - 1] = 0;
                else entering = 0;
                break;
            case ami_ettab:   choose((sel + 1) % NFIELDS); break;
            case ami_etup:    knob(1); break;
            case ami_etdown:  knob(-1); break;
            case ami_etleft:  move_digit(-1); break;
            case ami_etright: move_digit(1); break;
            case ami_etresize:
                ami_sizbufg(stdout, er.rszxg, er.rszyg);
                break;
            case ami_etredraw: break;
            default: dirty = 0; break;
        }
        if (dirty && er.etype != ami_etterm) redraw();
    } while (er.etype != ami_etterm);

    psu_shutdown(&psu);
    return 0;
}
