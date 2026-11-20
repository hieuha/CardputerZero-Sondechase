/* Drawing primitives, the top and nav bars, and small helpers shared by every screen. */
#define _GNU_SOURCE
#include "ui_priv.h"
#include <dirent.h>
#include <stdarg.h>

static int batt_pct = -1;
static double batt_t;

/* ---------------- primitives ---------------- */
void ln(double x1, double y1, double x2, double y2, uint32_t col, int w)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(col);
    d.width = w;
    d.p1.x = (lv_value_precise_t)x1;
    d.p1.y = (lv_value_precise_t)y1;
    d.p2.x = (lv_value_precise_t)x2;
    d.p2.y = (lv_value_precise_t)y2;
    lv_draw_line(LY, &d);
}

/* The part [t0, t1] of the segment inside a rectangle (Liang-Barsky); 0 if none of it is. */
static int clip(double x1, double y1, double x2, double y2, int cx0, int cy0, int cx1, int cy1, double *t0,
                double *t1)
{
    double dx = x2 - x1, dy = y2 - y1;
    double p[4] = {-dx, dx, -dy, dy}, q[4] = {x1 - cx0, cx1 - x1, y1 - cy0, cy1 - y1};
    int i;
    *t0 = 0;
    *t1 = 1;
    for (i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) return 0;
        } else {
            double r = q[i] / p[i];
            if (p[i] < 0) { if (r > *t1) return 0; if (r > *t0) *t0 = r; }
            else { if (r < *t0) return 0; if (r < *t1) *t1 = r; }
        }
    }
    return 1;
}

/* Dashes only where the line is on screen: a landing point thousands of km away at a high zoom (or across the date
 * line) is a line millions of pixels long, and one draw task per dash would stall the app. The pattern stays put. */
void dash(double x1, double y1, double x2, double y2, uint32_t col, int w, double on, double off)
{
    double len = hypot(x2 - x1, y2 - y1), p, t0, t1;
    if (!(len >= 0.5) || !clip(x1, y1, x2, y2, -w, -w, 319 + w, 169 + w, &t0, &t1)) return;
    for (p = floor(t0 * len / (on + off)) * (on + off); p < t1 * len; p += on + off) {
        double q = p + on > len ? len : p + on;
        ln(x1 + (x2 - x1) * p / len, y1 + (y2 - y1) * p / len, x1 + (x2 - x1) * q / len,
           y1 + (y2 - y1) * q / len, col, w);
    }
}

/* Line clipped to a rectangle. */
void ln_clip(double x1, double y1, double x2, double y2, int cx0, int cy0, int cx1, int cy1,
             uint32_t col, int w)
{
    double t0, t1;
    if (clip(x1, y1, x2, y2, cx0, cy0, cx1, cy1, &t0, &t1))
        ln(x1 + t0 * (x2 - x1), y1 + t0 * (y2 - y1), x1 + t1 * (x2 - x1), y1 + t1 * (y2 - y1), col, w);
}

void fill_opa(int x, int y, int w, int h, uint32_t col, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(col);
    d.bg_opa = opa;
    lv_draw_rect(LY, &d, &a);
}

void fill(int x, int y, int w, int h, uint32_t col)
{
    fill_opa(x, y, w, h, col, LV_OPA_COVER);
}

void txt(int x, int y, int w, const lv_font_t *f, uint32_t col, int align, const char *fmt, ...)
{
    char buf[96];
    lv_draw_label_dsc_t d;
    lv_area_t a = {x, y, x + w - 1, y + lv_font_get_line_height(f) - 1};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = lv_color_hex(col);
    d.align = (lv_text_align_t)align;
    d.text = buf;
    d.text_local = 1; /* rendering is deferred: LVGL keeps its own copy of the string */
    lv_draw_label(LY, &d, &a);
}

int text_w(const lv_font_t *f, const char *s)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* Panel with chamfered top-right and bottom-left corners. */
/* Panel with the top-right and bottom-left corners cut at 45 degrees (h >= 10). Drawn pixel by pixel: 1-px anti-aliased
 * diagonals come out as faint half-tone dots on this screen, so the cut edges are solid pixel steps. */
void box(int x, int y, int w, int h, uint32_t col, uint32_t bg)
{
    int i, c = 5, r = x + w - 1, b = y + h - 1;
    fill(x, y + c, w, h - 2 * c, bg);
    for (i = 0; i < c; i++) { /* rows beside the cut corners: what is underneath (map, grid) stays visible */
        fill(x, y + i, w - c + i, 1, bg);
        fill(x + c - i, b - i, w - c + i, 1, bg);
    }
    for (i = 0; i <= c; i++) { /* the cut edges */
        fill(r - c + i, y + i, 1, 1, col);
        fill(x + c - i, b - i, 1, 1, col);
    }
    fill(x, y, r - c - x, 1, col);         /* top */
    fill(r, y + c, 1, b - y - c + 1, col); /* right */
    fill(x + c, b, r - x - c + 1, 1, col); /* bottom */
    fill(x, y, 1, b - c - y + 1, col);     /* left */
}

void kv(int x, int y, int w, const char *k, uint32_t vc, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    txt(x, y, w, F_S, C_DIM, AL_L, "%s", k);
    txt(x, y, w, F_S, vc, AL_R, "%s", buf);
}

void pol(double cx, double cy, double r, double deg, double *x, double *y)
{
    double a = deg * M_PI / 180.0;
    *x = cx + r * sin(a);
    *y = cy - r * cos(a);
}

void fmt_dist(char *num, size_t n, const char **unit, double m)
{
    if (m < 1000.0) { snprintf(num, n, "%.0f", m); *unit = "m"; }
    else if (m < 100000.0) { snprintf(num, n, "%.2f", m / 1000.0); *unit = "km"; }
    else { snprintf(num, n, "%.0f", m / 1000.0); *unit = "km"; }
}

int fresh(void)
{
    return A->have_sonde && sc_now() - A->sonde.t_rx <= A->cfg.stale_s;
}

int land_rem_s(void)
{
    double r = A->pred.t_s - (sc_now() - A->sonde.t_rx);
    return r < 0 ? 0 : (int)r;
}

void read_battery(void)
{
    DIR *d;
    struct dirent *e;
    double now = sc_now();
    if (now - batt_t < 10.0) return;
    batt_t = now;
    batt_pct = -1;
    d = opendir("/sys/class/power_supply");
    if (!d) return;
    while ((e = readdir(d)) != NULL && batt_pct < 0) {
        char p[300];
        FILE *f;
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
        f = fopen(p, "r");
        if (!f) continue;
        if (fscanf(f, "%d", &batt_pct) != 1) batt_pct = -1;
        fclose(f);
    }
    closedir(d);
}

/* ---------------- chrome ---------------- */
void top_bar(const char *name, uint32_t chip, const char *mid)
{
    int i, cw = text_w(F_S, name) + 14; /* the chip grows to fit a page number, "Graph 1/2" */
    double now = sc_now();
    if (cw < 44) cw = 44;
    fill(0, 0, 320, 15, C_G);
    fill(0, 0, cw, 14, chip);
    for (i = 0; i < 6; i++) ln(cw - 6 + i, 14 - (i + 1) * 14 / 6.0, cw - 6 + i, 14, C_G, 1);
    txt(5, 0, cw, F_S, C_G, AL_L, "%s", name);
    if (mid) txt(cw + 4, 0, 194, F_S, C_TX, AL_L, "%s", mid);
    else if (A->freq_hz > 0)
        txt(cw + 4, 0, 150, F_S, C_TX, AL_L, "%.3f MHz  %s", A->freq_hz / 1e6, A->have_sonde ? A->sonde.id : "");
    if (A->state == ST_DECODE && A->have_sonde) {
        if (fresh()) txt(196, 0, 44, F_S, C_CY, AL_R, "RX %.0f s", now - A->sonde.t_rx);
        else txt(196, 0, 44, F_S, C_RD, AL_R, "RX lost");
    } else if (A->state == ST_SCAN) { /* what the receiver does, on every screen; the Radio screen does not repeat it */
        txt(196, 0, 44, F_S, C_YL, AL_R, "scan");
    } else if (A->state == ST_LISTEN) {
        txt(196, 0, 44, F_S, C_YL, AL_R, "listen");
    } else if (A->state == ST_DECODE) {
        txt(196, 0, 44, F_S, C_YL, AL_R, A->hunt ? "try" : "decode");
    }
    if (gps_ok(&A->gps, now)) {
        if (gps_sky(&A->gps, now)) txt(244, 0, 40, F_S, C_TX, AL_R, "%dD %d/%d", A->gps.mode, A->gps.n_used, A->gps.n_sat);
        else txt(244, 0, 40, F_S, C_TX, AL_R, A->gps.mode == 3 ? "GPS 3D" : "GPS 2D");
    } else { /* decoding continues; only distance, bearing and the beeper need this fix */
        fill(243, 1, 42, 13, C_RD);
        if (gps_sky(&A->gps, now)) /* receiver plugged in, no fix yet: satellites used / in view */
            txt(243, 0, 42, F_S, C_G, AL_C, "GPS %d/%d", A->gps.n_used, A->gps.n_sat);
        else txt(243, 0, 42, F_S, C_G, AL_C, A->gps.t_fix > 0 ? "GPS lost" : "No GPS");
    }
    if (A->prox.muted) txt(286, 0, 30, F_S, C_RD, AL_R, "mute");
    else if (batt_pct >= 0) txt(286, 0, 30, F_S, C_DIM, AL_R, "%d%%", batt_pct);
    ln(0, 15, 320, 15, C_L, 1);
}

void nav_bar(int active)
{
    static const char *nm[] = {"4 Track", "5 Map", "6 Graph", "7 Radio", "8 Info"};
    int i;
    fill(0, 156, 320, 14, C_P);
    for (i = 0; i < 5; i++) {
        if (i == active) fill(i * 64, 156, 64, 2, C_CY);
        txt(i * 64, 157, 64, F_S, i == active ? C_CY : C_DIM, AL_C, "%s", nm[i]);
    }
}
