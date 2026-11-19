/* sondechase UI: every screen is painted from one LVGL draw callback with three primitives
 * (line, rectangle, label) plus map tile images. Coordinates match the 320x170 mockup. */
#define _GNU_SOURCE
#include "ui.h"
#include "lvgl.h"
#include "lvgl_private.h" /* lv_image_decoder_dsc_t layout, lv_image_cache_drop */
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define C_G 0x0A0614  /* ground */
#define C_P 0x150D2A  /* panel */
#define C_L 0x3A2466  /* grid line */
#define C_CY 0x19E6FF /* this device, chrome */
#define C_PK 0xFF3D9A /* sonde */
#define C_YL 0xFFE14A /* prediction */
#define C_RD 0xFF6A4D /* signal lost */
#define C_GN 0x6CFF9A /* wind */
#define C_TX 0xE9E4FF
#define C_DIM 0xA79BD1
#define C_AD 0x27407F /* dry adiabats */

enum { PG_TRACK, PG_MAP, PG_GRAPH, PG_RADIO, PG_INFO };
enum { AL_L = LV_TEXT_ALIGN_LEFT, AL_C = LV_TEXT_ALIGN_CENTER, AL_R = LV_TEXT_ALIGN_RIGHT };

static app_t *A;
static lv_obj_t *root;
static lv_layer_t *LY;
static const lv_font_t *F_S, *F_M, *F_NUM, *F_BIG, *F_TTL; /* 11, 12, Orbitron 17 / 30 / 25 */
static int page, gpage, quit, entry, entry_len, map_follow, map_zoom = -1, batt_pct = -1;
static char entry_buf[8];
static double batt_t;

/* Decoded map tiles kept in RAM as RGB565. Pixel buffers are static, so a descriptor handed
 * to LVGL stays valid for the whole frame even when another slot is replaced. */
#define TC_N 12
static struct { int z, x, y, valid; unsigned stamp; lv_image_dsc_t dsc; } tc[TC_N];
static uint8_t tc_px[TC_N][TILE_PX * TILE_PX * 2];
static unsigned tc_stamp;

/* ---------------- primitives ---------------- */
static void ln(double x1, double y1, double x2, double y2, uint32_t col, int w)
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

static void dash(double x1, double y1, double x2, double y2, uint32_t col, int w, double on, double off)
{
    double len = hypot(x2 - x1, y2 - y1), p;
    if (len < 0.5) return;
    for (p = 0; p < len; p += on + off) {
        double q = p + on > len ? len : p + on;
        ln(x1 + (x2 - x1) * p / len, y1 + (y2 - y1) * p / len, x1 + (x2 - x1) * q / len,
           y1 + (y2 - y1) * q / len, col, w);
    }
}

/* Line clipped to a rectangle (Liang-Barsky). */
static void ln_clip(double x1, double y1, double x2, double y2, int cx0, int cy0, int cx1, int cy1,
                    uint32_t col, int w)
{
    double t0 = 0, t1 = 1, dx = x2 - x1, dy = y2 - y1;
    double p[4] = {-dx, dx, -dy, dy}, q[4] = {x1 - cx0, cx1 - x1, y1 - cy0, cy1 - y1};
    int i;
    for (i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) return;
        } else {
            double r = q[i] / p[i];
            if (p[i] < 0) { if (r > t1) return; if (r > t0) t0 = r; }
            else { if (r < t0) return; if (r < t1) t1 = r; }
        }
    }
    ln(x1 + t0 * dx, y1 + t0 * dy, x1 + t1 * dx, y1 + t1 * dy, col, w);
}

static void fill(int x, int y, int w, int h, uint32_t col)
{
    lv_draw_rect_dsc_t d;
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(col);
    d.bg_opa = LV_OPA_COVER;
    lv_draw_rect(LY, &d, &a);
}

static void txt(int x, int y, int w, const lv_font_t *f, uint32_t col, int align, const char *fmt, ...)
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

static int text_w(const lv_font_t *f, const char *s)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* Panel with chamfered top-right and bottom-left corners. */
static void box(int x, int y, int w, int h, uint32_t col, uint32_t bg)
{
    int i, c = 5, r = x + w - 1, b = y + h - 1;
    fill(x, y, w, h, bg);
    for (i = 0; i < c; i++) {
        ln(r - c + 1 + i, y + i, r + 1, y + i, C_G, 1);
        ln(x, b - i, x + c - i, b - i, C_G, 1);
    }
    ln(x, y, r - c, y, col, 1);
    ln(r - c, y, r, y + c, col, 1);
    ln(r, y + c, r, b, col, 1);
    ln(r, b, x + c, b, col, 1);
    ln(x + c, b, x, b - c, col, 1);
    ln(x, b - c, x, y, col, 1);
}

static void kv(int x, int y, int w, const char *k, uint32_t vc, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    txt(x, y, w, F_S, C_DIM, AL_L, "%s", k);
    txt(x, y, w, F_S, vc, AL_R, "%s", buf);
}

static void pol(double cx, double cy, double r, double deg, double *x, double *y)
{
    double a = deg * M_PI / 180.0;
    *x = cx + r * sin(a);
    *y = cy - r * cos(a);
}

static void fmt_dist(char *num, size_t n, const char **unit, double m)
{
    if (m < 1000.0) { snprintf(num, n, "%.0f", m); *unit = "m"; }
    else if (m < 100000.0) { snprintf(num, n, "%.2f", m / 1000.0); *unit = "km"; }
    else { snprintf(num, n, "%.0f", m / 1000.0); *unit = "km"; }
}

static int fresh(void)
{
    return A->have_sonde && sc_now() - A->sonde.t_rx <= A->cfg.stale_s;
}

static int land_rem_s(void)
{
    double r = A->pred.t_s - (sc_now() - A->sonde.t_rx);
    return r < 0 ? 0 : (int)r;
}

static void read_battery(void)
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
static void top_bar(const char *name, uint32_t chip, const char *mid)
{
    int i, cw = 44;
    double now = sc_now();
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
    } else if (A->state == ST_SCAN) {
        txt(196, 0, 44, F_S, C_YL, AL_R, "scan");
    } else if (A->state == ST_LISTEN) {
        txt(196, 0, 44, F_S, C_YL, AL_R, "listen");
    }
    if (gps_ok(&A->gps, now)) {
        txt(244, 0, 40, F_S, C_TX, AL_R, A->gps.mode == 3 ? "GPS 3D" : "GPS 2D");
    } else { /* decoding continues; only distance, bearing and the beeper need this fix */
        fill(243, 1, 42, 13, C_RD);
        txt(243, 0, 42, F_S, C_G, AL_C, A->gps.t_fix > 0 ? "GPS lost" : "No GPS");
    }
    if (A->prox.muted) txt(286, 0, 30, F_S, C_RD, AL_R, "mute");
    else if (batt_pct >= 0) txt(286, 0, 30, F_S, C_DIM, AL_R, "%d%%", batt_pct);
    ln(0, 15, 320, 15, C_L, 1);
}

static void nav_bar(int active)
{
    static const char *nm[] = {"4 Track", "5 Map", "6 Graph", "7 Radio", "8 Info"};
    int i;
    fill(0, 156, 320, 14, C_P);
    for (i = 0; i < 5; i++) {
        if (i == active) fill(i * 64, 156, 64, 2, C_CY);
        txt(i * 64, 157, 64, F_S, i == active ? C_CY : C_DIM, AL_C, "%s", nm[i]);
    }
}

/* ---------------- 4 Track ---------------- */
static void compass(double hdg, int have_rel, double rel, uint32_t col, int dashed, int have_land, double land_rel)
{
    const double cx = 68, cy = 86;
    double x0, y0, x1, y1, tx, ty, bx, by, hx, hy, lx, ly, rx, ry;
    static const char *card[] = {"N", "E", "S", "W"};
    int k;
    for (k = 0; k < 72; k++) { /* outer ring as a 72-gon */
        pol(cx, cy, 54, k * 5, &x0, &y0);
        pol(cx, cy, 54, k * 5 + 5, &x1, &y1);
        ln(x0, y0, x1, y1, C_L, 2);
    }
    for (k = 0; k < 36; k++) {
        int major = k % 9 == 0, mid = k % 3 == 0;
        pol(cx, cy, major ? 45 : (mid ? 49 : 52), k * 10 - hdg, &x0, &y0);
        pol(cx, cy, 54, k * 10 - hdg, &x1, &y1);
        ln(x0, y0, x1, y1, major ? C_TX : C_DIM, major ? 2 : 1);
    }
    for (k = 0; k < 4; k++) {
        pol(cx, cy, 38, k * 90 - hdg, &x0, &y0);
        txt((int)x0 - 8, (int)y0 - 6, 16, F_S, k == 0 ? C_CY : C_DIM, AL_C, "%s", card[k]);
    }
    ln(cx, cy - 55, cx - 5, cy - 63, C_CY, 2); /* heading index */
    ln(cx, cy - 55, cx + 5, cy - 63, C_CY, 2);
    ln(cx - 5, cy - 63, cx + 5, cy - 63, C_CY, 2);
    if (have_land) { /* predicted landing: diamond on the ring */
        pol(cx, cy, 54, land_rel, &x0, &y0);
        ln(x0, y0 - 5, x0 + 5, y0, C_YL, 2);
        ln(x0 + 5, y0, x0, y0 + 5, C_YL, 2);
        ln(x0, y0 + 5, x0 - 5, y0, C_YL, 2);
        ln(x0 - 5, y0, x0, y0 - 5, C_YL, 2);
    }
    if (have_rel) {
        pol(cx, cy, 30, rel, &tx, &ty);
        pol(cx, cy, -24, rel, &bx, &by);
        pol(cx, cy, 47, rel, &hx, &hy);
        pol(tx, ty, 9, rel - 90, &lx, &ly);
        pol(tx, ty, 9, rel + 90, &rx, &ry);
        if (dashed) dash(bx, by, tx, ty, col, 3, 5, 4);
        else ln(bx, by, tx, ty, col, 3);
        ln(hx, hy, lx, ly, col, 2);
        ln(hx, hy, rx, ry, col, 2);
        ln(lx, ly, rx, ry, col, 2);
        if (!dashed) ln(hx, hy, tx, ty, col, 4);
    }
    ln(cx, cy - 6, cx + 5, cy + 5, C_CY, 2); /* this device */
    ln(cx + 5, cy + 5, cx, cy + 2, C_CY, 2);
    ln(cx, cy + 2, cx - 5, cy + 5, C_CY, 2);
    ln(cx - 5, cy + 5, cx, cy - 6, C_CY, 2);
}

static void compass_off(void)
{
    const double cx = 68, cy = 86;
    double x0, y0, x1, y1;
    int k;
    for (k = 0; k < 72; k += 2) { /* dashed ring: no bearing available */
        pol(cx, cy, 54, k * 5, &x0, &y0);
        pol(cx, cy, 54, k * 5 + 5, &x1, &y1);
        ln(x0, y0, x1, y1, C_L, 2);
    }
    for (k = 0; k < 36; k += 3) {
        pol(cx, cy, k % 9 == 0 ? 45 : 49, k * 10, &x0, &y0);
        pol(cx, cy, 54, k * 10, &x1, &y1);
        ln(x0, y0, x1, y1, C_L, 2);
    }
    txt(22, 70, 92, F_M, C_RD, AL_C, "No bearing");
    txt(22, 86, 92, F_S, C_DIM, AL_C, "needs a GPS fix");
    txt(22, 97, 92, F_S, C_DIM, AL_C, "on this device");
}

static void ll(char *out, size_t n, double lat, double lon)
{
    snprintf(out, n, "%.5f %c  %.5f %c", fabs(lat), lat < 0 ? 'S' : 'N', fabs(lon), lon < 0 ? 'W' : 'E');
}

/* Track without a position for this device: the sonde is still decoded and shown in full;
 * distance, bearing and elevation are replaced by absolute coordinates. */
static void p_track_nogps(void)
{
    const sonde_t *s = &A->sonde;
    int ok = fresh();
    char c[48];
    compass_off();
    if (ok) {
        box(140, 20, 62, 14, C_PK, C_G);
        txt(140, 20, 62, F_S, C_PK, AL_C, "%s", A->flight.phase == PH_ASCENT ? "Ascending" : A->flight.phase == PH_DESCENT ? "Descending" : A->flight.phase == PH_LANDED ? "Landed" : "Tracking");
    } else {
        fill(140, 20, 88, 14, C_RD);
        txt(140, 20, 88, F_S, C_G, AL_C, "Signal lost %.0f s", sc_now() - s->t_rx);
    }
    if (A->gps.t_fix > 0) txt(232, 21, 82, F_S, C_RD, AL_R, "GPS lost %.0f s", sc_now() - A->gps.t_fix);
    else txt(232, 21, 82, F_S, C_RD, AL_R, "Waiting for GPS");
    snprintf(c, sizeof c, "%.0f", s->alt);
    txt(140, 36, 150, F_BIG, ok ? C_TX : C_RD, AL_L, "%s", c);
    txt(146 + text_w(F_BIG, c), 54, 70, F_M, C_DIM, AL_L, "m altitude");
    kv(140, 76, 68, "V/S", C_TX, "%+.1f", fabs(A->vv_s) < 0.05 ? 0.0 : A->vv_s);
    kv(216, 76, 98, "Drift", C_TX, "%.1f m/s %03.0f°", s->vel_h, s->heading);
    if (s->temp > -270) kv(140, 90, 68, "Temp", C_TX, "%.1f °C", s->temp);
    if (s->humidity >= 0) kv(216, 90, 98, "RH", C_TX, "%.0f %%", s->humidity);
    ll(c, sizeof c, s->lat, s->lon);
    kv(140, 104, 174, "Sonde", C_TX, "%s", c);
    if (ok && A->pred.valid) {
        ll(c, sizeof c, A->pred.lat, A->pred.lon);
        box(140, 132, 174, 18, C_YL, C_P);
        txt(146, 134, 40, F_S, C_YL, AL_L, "Landing");
        txt(186, 134, 122, F_S, C_YL, AL_R, "%s", c);
    }
}

static const char *phase_name(void)
{
    static const char *n[] = {"Tracking", "Ascending", "Descending", "Landed"};
    return n[A->flight.phase];
}

static void p_track(void)
{
    const sonde_t *s = &A->sonde;
    static const char *src[] = {"none", "GPS", "mag", "held"};
    int ok = fresh(), have_h = A->hdg.src != HDG_NONE;
    uint32_t col = ok ? C_PK : C_RD;
    double hdg = have_h ? A->hdg.deg : 0.0;
    char num[16];
    const char *unit = "";

    top_bar("Track", C_CY, NULL);
    if (A->have_sonde && !A->have_geo) {
        p_track_nogps();
        nav_bar(PG_TRACK);
        return;
    }
    compass(hdg, A->have_geo, A->rel, col, !ok, A->have_land && ok, norm180(A->land_brg - hdg));
    if (!A->have_sonde && A->nofix.t_rx > 0 && sc_now() - A->nofix.t_rx < 10.0) {
        txt(140, 44, 174, F_M, C_CY, AL_L, "%s", A->nofix.id);
        txt(140, 60, 174, F_M, C_TX, AL_L, "Frame %d   batt %.1f V", A->nofix.frame, A->nofix.batt);
        txt(140, 80, 174, F_S, C_RD, AL_L, "Sonde has no GPS fix (%d sats)", A->nofix.sats);
        txt(140, 94, 174, F_S, C_DIM, AL_L, "Position appears once it locks");
        nav_bar(PG_TRACK);
        return;
    }
    if (!A->have_sonde) {
        txt(140, 60, 174, F_M, C_DIM, AL_L, A->state == ST_DECODE ? "Waiting for the first frame" : "No frequency tuned");
        txt(140, 76, 174, F_S, C_DIM, AL_L, "7 radio     F type a frequency");
        nav_bar(PG_TRACK);
        return;
    }
    if (ok) {
        box(140, 20, 62, 14, C_PK, C_G);
        txt(140, 20, 62, F_S, C_PK, AL_C, "%s", phase_name());
    } else {
        fill(140, 20, 88, 14, C_RD);
        txt(140, 20, 88, F_S, C_G, AL_C, "Signal lost %.0f s", sc_now() - s->t_rx);
    }
    if (have_h) txt(232, 21, 82, F_S, C_DIM, AL_R, "Hdg %03.0f° %s", hdg, src[A->hdg.src]);
    else txt(232, 21, 82, F_S, C_DIM, AL_R, "North up");

    fmt_dist(num, sizeof num, &unit, A->dist);
    txt(140, 36, 150, F_BIG, ok ? C_TX : C_RD, AL_L, "%s", num);
    txt(146 + text_w(F_BIG, num), 54, 40, F_M, C_DIM, AL_L, "%s", unit);
    kv(140, 76, 68, "Brg", C_TX, "%03.0f°", A->brg);
    if (have_h) kv(216, 76, 98, "Rel", C_TX, "%+04.0f°", A->rel);
    kv(140, 90, 68, "Elev", C_TX, "%+.1f°", fabs(A->elev) < 0.05 ? 0.0 : A->elev);
    kv(216, 90, 98, "Alt", C_TX, "%.0f m", s->alt);
    kv(140, 104, 68, "V/S", C_TX, "%+.1f", fabs(A->vv_s) < 0.05 ? 0.0 : A->vv_s);
    kv(216, 104, 98, "Drift", C_TX, "%.1f m/s %03.0f°", s->vel_h, s->heading);

    if (ok && A->have_land) {
        fmt_dist(num, sizeof num, &unit, A->land_dist);
        box(140, 132, 174, 18, C_YL, C_P);
        txt(146, 134, 92, F_S, C_YL, AL_L, "Landing %s %s", num, unit);
        txt(236, 134, 28, F_S, C_YL, AL_C, "%03.0f°", A->land_brg);
        txt(262, 134, 46, F_S, C_YL, AL_R, "T-%02d:%02d", land_rem_s() / 60, land_rem_s() % 60);
    } else if (!ok) {
        box(140, 132, 174, 18, C_RD, C_P);
        txt(146, 134, 40, F_S, C_DIM, AL_L, "Last fix");
        txt(186, 134, 122, F_S, C_TX, AL_R, "%.5f %c  %.5f %c", fabs(s->lat), s->lat < 0 ? 'S' : 'N', fabs(s->lon), s->lon < 0 ? 'W' : 'E');
    }
    nav_bar(PG_TRACK);
}

/* ---------------- 5 Map ---------------- */
static const lv_image_dsc_t *tile_get(int z, int x, int y)
{
    static unsigned char rgb[TILE_PX * TILE_PX * 3];
    char path[320], fsp[330];
    lv_image_decoder_dsc_t dec;
    int i, slot = 0, kind, ok = 0;
    for (i = 0; i < TC_N; i++)
        if (tc[i].valid && tc[i].z == z && tc[i].x == x && tc[i].y == y) {
            tc[i].stamp = ++tc_stamp;
            return &tc[i].dsc;
        }
    kind = tf_find(&A->tf, &A->cfg, z, x, y, path, sizeof path);
    if (!kind) return NULL;
    snprintf(fsp, sizeof fsp, "A:%s", path);
    if (lv_image_decoder_open(&dec, fsp, NULL) == LV_RESULT_OK) { /* LVGL's PNG decoder (lodepng) */
        const lv_draw_buf_t *b = dec.decoded;
        int cf = b ? (int)b->header.cf : -1, bpp = cf == LV_COLOR_FORMAT_RGB888 ? 3 : 4, xx, yy;
        if (b && b->header.w == TILE_PX && b->header.h == TILE_PX &&
            (cf == LV_COLOR_FORMAT_ARGB8888 || cf == LV_COLOR_FORMAT_XRGB8888 || cf == LV_COLOR_FORMAT_RGB888)) {
            for (yy = 0; yy < TILE_PX; yy++) {
                const uint8_t *row = b->data + (size_t)yy * b->header.stride; /* stored as B, G, R(, A) */
                for (xx = 0; xx < TILE_PX; xx++) {
                    rgb[3 * (yy * TILE_PX + xx)] = row[xx * bpp + 2];
                    rgb[3 * (yy * TILE_PX + xx) + 1] = row[xx * bpp + 1];
                    rgb[3 * (yy * TILE_PX + xx) + 2] = row[xx * bpp];
                }
            }
            ok = 1;
        }
        lv_image_decoder_close(&dec);
        lv_image_cache_drop(fsp); /* the RGB565 copy below is the only one kept */
    }
    if (!ok) {
        if (kind == 2) unlink(path); /* damaged download: fetch it again */
        return NULL;
    }
    if (kind == 2) /* downloaded tiles keep the server's light colours on disk */
        tile_recolor(rgb, TILE_PX * TILE_PX, A->cfg.map_simple);
    for (i = 0; i < TC_N; i++) {
        if (!tc[i].valid) { slot = i; break; }
        if (tc[i].stamp < tc[slot].stamp) slot = i;
    }
    for (i = 0; i < TILE_PX * TILE_PX; i++) {
        uint16_t v = (uint16_t)(((rgb[3 * i] >> 3) << 11) | ((rgb[3 * i + 1] >> 2) << 5) | (rgb[3 * i + 2] >> 3));
        tc_px[slot][2 * i] = (uint8_t)(v & 0xff);
        tc_px[slot][2 * i + 1] = (uint8_t)(v >> 8);
    }
    lv_image_cache_drop(&tc[slot].dsc);
    memset(&tc[slot].dsc, 0, sizeof tc[slot].dsc);
    tc[slot].dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    tc[slot].dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    tc[slot].dsc.header.w = TILE_PX;
    tc[slot].dsc.header.h = TILE_PX;
    tc[slot].dsc.header.stride = TILE_PX * 2;
    tc[slot].dsc.data = tc_px[slot];
    tc[slot].dsc.data_size = sizeof tc_px[slot];
    tc[slot].z = z;
    tc[slot].x = x;
    tc[slot].y = y;
    tc[slot].valid = 1;
    tc[slot].stamp = ++tc_stamp;
    return &tc[slot].dsc;
}

static double m_cx, m_cy; /* map centre in world pixels at the current zoom */
static int m_z;

static void m_xy(double lat, double lon, double *x, double *y)
{
    double tx, ty;
    tile_frac(lat, lon, m_z, &tx, &ty);
    *x = tx * TILE_PX - m_cx + 160.0;
    *y = ty * TILE_PX - m_cy + 86.0;
}

static void p_map(void)
{
    const gps_t *g = &A->gps;
    const sonde_t *s = &A->sonde;
    int have_me = gps_ok(g, sc_now()), have_s = A->have_sonde, i, tx, ty, any = 0, missing = 0, loading = 0;
    double clat, clon, x, y, x2, y2, tfx, tfy;
    static const char *fm[] = {"both", "device", "sonde"};

    if (have_me && (map_follow == 1 || !have_s)) { clat = g->lat; clon = g->lon; }
    else if (have_s && (map_follow == 2 || !have_me)) { clat = s->lat; clon = s->lon; }
    else if (have_me && have_s) { clat = (g->lat + s->lat) / 2; clon = (g->lon + s->lon) / 2; }
    else { clat = A->cfg.map_lat; clon = A->cfg.map_lon; } /* no position yet: still show the map */
    m_z = map_zoom >= 0 ? map_zoom
        : (have_me && have_s && map_follow == 0)
              ? tile_fit_zoom(g->lat, g->lon, s->lat, s->lon, 260, 100, A->cfg.zoom_min, A->cfg.zoom_max)
              : (A->cfg.zoom_max < 13 ? A->cfg.zoom_max : 13);
    tile_frac(clat, clon, m_z, &tfx, &tfy);
    m_cx = tfx * TILE_PX;
    m_cy = tfy * TILE_PX;

    /* tiles: preloaded pack first, then the download cache; a missing tile is requested from the
     * network only now, while the map is showing it */
    for (ty = (int)floor((m_cy - 70) / TILE_PX); ty <= (int)floor((m_cy + 70) / TILE_PX); ty++)
        for (tx = (int)floor((m_cx - 160) / TILE_PX); tx <= (int)floor((m_cx + 160) / TILE_PX); tx++) {
            const lv_image_dsc_t *img;
            int x0 = (int)floor(tx * TILE_PX - m_cx + 160.0), y0 = (int)floor(ty * TILE_PX - m_cy + 86.0);
            if (tx < 0 || ty < 0 || tx >= (1 << m_z) || ty >= (1 << m_z)) continue;
            img = tile_get(m_z, tx, ty);
            if (img) {
                lv_draw_image_dsc_t d;
                lv_area_t a = {x0, y0, x0 + TILE_PX - 1, y0 + TILE_PX - 1};
                lv_draw_image_dsc_init(&d);
                d.src = img;
                lv_draw_image(LY, &d, &a);
                any = 1;
            } else {
                missing++;
                tf_request(&A->tf, &A->cfg, m_z, tx, ty, sc_now());
                loading += tf_busy(&A->tf, m_z, tx, ty);
                for (i = 0; i < TILE_PX; i += 32) { /* empty cell: plain graticule */
                    ln_clip(x0 + i, y0, x0 + i, y0 + TILE_PX - 1, 0, 16, 319, 155, C_P, 1);
                    ln_clip(x0, y0 + i, x0 + TILE_PX - 1, y0 + i, 0, 16, 319, 155, C_P, 1);
                }
            }
        }

    for (i = 1; i < A->hist.n; i++) { /* received track */
        m_xy(A->hist.p[i - 1].lat, A->hist.p[i - 1].lon, &x, &y);
        m_xy(A->hist.p[i].lat, A->hist.p[i].lon, &x2, &y2);
        ln_clip(x, y, x2, y2, 0, 16, 319, 155, C_PK, 2);
    }
    if (have_s && A->pred.valid && fresh()) {
        m_xy(s->lat, s->lon, &x, &y);
        m_xy(A->pred.lat, A->pred.lon, &x2, &y2);
        dash(x, y, x2, y2, C_YL, 2, 3, 3);
        ln(x2 - 5, y2 - 5, x2 + 5, y2 + 5, C_YL, 2);
        ln(x2 - 5, y2 + 5, x2 + 5, y2 - 5, C_YL, 2);
        {
            int lx = (int)x2 + 9 + 86 > 318 ? (int)x2 - 9 - 86 : (int)x2 + 9, ly = (int)y2 + 5;
            if (lx < 2) lx = 2;
            fill(lx, ly, 86, 13, C_G);
            txt(lx, ly, 86, F_S, C_YL, AL_C, "Landing T-%02d:%02d", land_rem_s() / 60, land_rem_s() % 60);
        }
    }
    if (have_s) {
        uint32_t col = fresh() ? C_PK : C_RD;
        m_xy(s->lat, s->lon, &x, &y);
        for (i = 0; i < 7; i++) { /* filled diamond */
            ln(x - i, y - 7 + i, x + i, y - 7 + i, col, 1);
            ln(x - i, y + 7 - i, x + i, y + 7 - i, col, 1);
        }
        ln(x - 7, y, x + 7, y, col, 1);
        fill((int)x - 21, (int)y - 24, 42, 13, C_G);
        txt((int)x - 21, (int)y - 24, 42, F_S, C_TX, AL_C, "%.0f m", s->alt);
    }
    if (have_me) {
        double h = A->hdg.src != HDG_NONE ? A->hdg.deg : 0, ax, ay, bx, by, cx2, cy2, dx, dy;
        double mpp = tile_mpp(g->lat, m_z), r = 5000.0 / mpp;
        m_xy(g->lat, g->lon, &x, &y);
        if (r > 20 && r < 200)
            for (i = 0; i < 48; i += 2) { /* 5 km range ring */
                pol(x, y, r, i * 7.5, &ax, &ay);
                pol(x, y, r, i * 7.5 + 7.5, &bx, &by);
                ln_clip(ax, ay, bx, by, 0, 16, 319, 155, C_DIM, 1);
            }
        pol(x, y, 8, h, &ax, &ay);
        pol(x, y, 8, h + 143, &bx, &by);
        pol(x, y, 2, h + 180, &cx2, &cy2);
        pol(x, y, 8, h - 143, &dx, &dy);
        ln(ax, ay, bx, by, C_CY, 2);
        ln(bx, by, cx2, cy2, C_CY, 2);
        ln(cx2, cy2, dx, dy, C_CY, 2);
        ln(dx, dy, ax, ay, C_CY, 2);
    }

    top_bar("Map", C_CY, have_me || have_s ? NULL : "No position yet: default area");
    box(4, 20, 96, 16, C_L, C_P);
    txt(9, 21, 40, F_S, C_TX, AL_L, "Zoom %d%s", m_z, map_zoom < 0 ? "" : "*");
    txt(49, 21, 46, F_S, C_DIM, AL_R, "%s", fm[map_follow]);
    box(104, 20, 52, 16, C_L, C_P);
    txt(108, 21, 44, F_S, C_DIM, AL_C, "%s", A->cfg.map_simple ? "simple" : "detailed");
    if (missing) { /* say why part of the map is empty */
        const char *why = loading ? "Loading map" : !A->cfg.tile_online ? "No map stored for this area"
                        : (A->tf.failed && A->tf.t_last_fail >= A->tf.t_last_ok) ? "No network: stored map only" : "Loading map";
        fill(4, 39, 150, 14, C_G);
        txt(9, 39, 144, F_S, loading ? C_CY : C_RD, AL_L, "%s", why);
    }
    if (any) { /* attribution required for OpenStreetMap-derived tiles */
        fill(112, 141, 98, 12, C_G);
        txt(112, 140, 98, F_S, C_DIM, AL_C, "© OpenStreetMap");
    }
    { /* scale bar: largest of 0.5/1/2/5/10/20/50 km that fits in 60 px */
        static const double st[] = {50000, 20000, 10000, 5000, 2000, 1000, 500, 200, 100};
        double mpp = tile_mpp(clat, m_z);
        for (i = 0; i < 8 && st[i] / mpp > 60; i++) {}
        fill(4, 138, (int)(st[i] / mpp) + 44, 15, C_G);
        ln(8, 147, 8, 150, C_TX, 2);
        ln(8, 150, 8 + st[i] / mpp, 150, C_TX, 2);
        ln(8 + st[i] / mpp, 150, 8 + st[i] / mpp, 147, C_TX, 2);
        if (st[i] >= 1000) txt((int)(12 + st[i] / mpp), 140, 40, F_S, C_TX, AL_L, "%.0f km", st[i] / 1000);
        else txt((int)(12 + st[i] / mpp), 140, 40, F_S, C_TX, AL_L, "%.0f m", st[i]);
    }
    if (A->have_geo) {
        char num[16];
        const char *unit;
        fmt_dist(num, sizeof num, &unit, A->dist);
        box(214, 136, 102, 17, C_CY, C_P);
        txt(220, 138, 60, F_S, C_TX, AL_L, "%s %s", num, unit);
        txt(280, 138, 30, F_S, C_TX, AL_R, "%03.0f°", A->brg);
    }
    nav_bar(PG_MAP);
}

/* ---------------- 6 Graph ---------------- */
#define SK_X0 30
#define SK_X1 222
#define SK_YB 143
#define SK_YT 33
static double sk_y(double p)
{
    if (p < 100) p = 100;
    if (p > 1050) p = 1050;
    return SK_YB - log(1050.0 / p) / log(10.5) * (SK_YB - SK_YT);
}

static double sk_x(double t, double p)
{
    return SK_X0 + (t + 35.0) * (SK_X1 - SK_X0) / 80.0 + (SK_YB - sk_y(p));
}

static void sk_ln(double t1, double p1, double t2, double p2, uint32_t col, int w)
{
    ln_clip(sk_x(t1, p1), sk_y(p1), sk_x(t2, p2), sk_y(p2), SK_X0, SK_YT, SK_X1, SK_YB, col, w);
}

static void p_skewt(void)
{
    static const int isob[] = {1000, 850, 700, 500, 300, 200, 100};
    static const double wl[] = {900, 700, 500, 400, 300, 200, 150};
    const hist_t *h = &A->hist;
    const sonde_t *s = &A->sonde;
    double zero_alt = -1, trop_alt = -1, tmin = 1e9;
    int i, th, t, p;

    top_bar("Graph", C_CY, NULL);
    fill(SK_X0, SK_YT, SK_X1 - SK_X0, SK_YB - SK_YT, C_P);
    for (th = -20; th < 180; th += 20) /* dry adiabats: T = theta (p/1000)^0.2857 */
        for (p = 1050; p > 100; p -= 50)
            sk_ln((th + 273.15) * pow(p / 1000.0, 0.2857) - 273.15, p,
                  (th + 273.15) * pow((p - 50) / 1000.0, 0.2857) - 273.15, p - 50, C_AD, 1);
    for (t = -120; t < 60; t += 20) sk_ln(t, 1050, t, 100, t == 0 ? C_DIM : C_L, 1);
    for (i = 0; i < 6; i++) ln(SK_X0, sk_y(isob[i]), SK_X1, sk_y(isob[i]), C_L, 1);

    for (i = 1; i < h->n; i++) {
        const hist_pt_t *a = &h->p[i - 1], *b = &h->p[i];
        if (a->pres < 100 && b->pres < 100) continue;
        if (a->temp > -270 && b->temp > -270) {
            sk_ln(a->temp, a->pres, b->temp, b->pres, C_CY, 2);
            if (a->rh >= 0 && b->rh >= 0 && (i & 1)) /* dashed: every second segment */
                sk_ln(dewpoint(a->temp, a->rh), a->pres, dewpoint(b->temp, b->rh), b->pres, C_PK, 2);
            if (zero_alt < 0 && (a->temp > 0) != (b->temp > 0)) zero_alt = b->alt;
            if (b->alt > 5000 && b->temp < tmin) { tmin = b->temp; trop_alt = b->alt; }
        }
    }
    if (A->flight.max_alt < 12000) trop_alt = -1; /* tropopause not reached yet */
    for (i = 0; i < 7; i++) { /* wind by level: line from the centre towards where the wind comes from */
        int k, best = -1;
        double bd = 30.0, x = 234, y = sk_y(wl[i]), ax, ay, bx, by;
        for (k = 0; k < h->n; k++)
            if (fabs(h->p[k].pres - wl[i]) < bd) { bd = fabs(h->p[k].pres - wl[i]); best = k; }
        if (best < 0) continue;
        pol(x, y, 6, h->p[best].wd, &ax, &ay);
        pol(x, y, 6, h->p[best].wd + 180, &bx, &by);
        ln(ax, ay, bx, by, C_GN, 2);
        fill((int)bx - 1, (int)by - 1, 4, 4, C_GN);
        txt(240, (int)y - 6, 15, F_S, C_GN, AL_R, "%.0f", h->p[best].ws);
    }
    if (A->have_sonde && s->pressure >= 100) {
        dash(SK_X0, sk_y(s->pressure), SK_X1, sk_y(s->pressure), C_TX, 1, 2, 3);
    }
    ln(SK_X0, SK_YT, SK_X1, SK_YT, C_DIM, 1);
    ln(SK_X1, SK_YT, SK_X1, SK_YB, C_DIM, 1);
    ln(SK_X1, SK_YB, SK_X0, SK_YB, C_DIM, 1);
    ln(SK_X0, SK_YB, SK_X0, SK_YT, C_DIM, 1);
    for (i = 0; i < 7; i++) /* 850 hPa has a grid line but no label: it would collide with 700 and 1000 */
        if (isob[i] != 850) txt(0, (int)sk_y(isob[i]) - (i == 0 ? 10 : 6), SK_X0 - 3, F_S, C_DIM, AL_R, "%d", isob[i]);
    for (t = -20; t <= 40; t += 20) txt((int)sk_x(t, 1050) - 12, 144, 24, F_S, C_DIM, AL_C, t ? "%+d" : "%d", t);
    txt(226, 144, 29, F_S, C_GN, AL_R, "m/s");
    txt(SK_X0, 18, 100, F_M, C_TX, AL_L, "Skew-T log-P");
    txt(SK_X1 - 30, 19, 30, F_S, C_DIM, AL_R, "1/2");

    if (A->have_sonde) {
        kv(258, 20, 60, "P", C_TX, "%s%.0f hPa", s->pres_est ? "~" : "", s->pressure);
        if (s->temp > -270) kv(258, 34, 60, "T", C_CY, "%.1f °C", s->temp);
        if (s->temp > -270 && s->humidity >= 0) {
            kv(258, 48, 60, "Td", C_PK, "%.1f °C", dewpoint(s->temp, s->humidity));
            kv(258, 62, 60, "RH", C_TX, "%.0f %%", s->humidity);
        }
        kv(258, 76, 60, "Wind", C_GN, "%.0f m/s", s->vel_h);
        ln(259, 93, 317, 93, C_L, 1);
        if (zero_alt >= 0) kv(258, 97, 60, "0 °C", C_TX, "%.1f km", zero_alt / 1000);
        if (trop_alt >= 0) kv(258, 111, 60, "Trop", C_TX, "%.1f km", trop_alt / 1000);
    }
    nav_bar(PG_GRAPH);
}

static void p_flight(void)
{
    const hist_t *h = &A->hist;
    const int X0 = 26, X1 = 312, YT = 36, YB = 104, VT = 116, VB = 146;
    double tmax = 30, amax = 10, t_last = h->n ? h->p[h->n - 1].t_s / 60.0 : 0;
    int i, m, a;

    top_bar("Graph", C_CY, NULL);
    if (t_last * 1.15 > tmax) tmax = ceil(t_last * 1.15 / 30.0) * 30.0;
    if (A->flight.max_alt / 1000.0 > amax) amax = ceil(A->flight.max_alt / 10000.0) * 10.0;
#define FX(tm) (X0 + (tm) / tmax * (X1 - X0))
#define FA(km) (YB - (km) / amax * (YB - YT))
#define FV(v) (VT + (10.0 - ((v) < -60 ? -60 : ((v) > 10 ? 10 : (v)))) / 70.0 * (VB - VT))
    for (a = 0; a <= (int)amax; a += 10) {
        ln(X0, FA(a), X1, FA(a), C_L, 1);
        txt(0, (int)FA(a) - 6, X0 - 4, F_S, C_DIM, AL_R, "%d", a);
    }
    for (m = 0; m <= (int)tmax; m += (int)(tmax / 5 > 30 ? 60 : 30)) {
        ln(FX(m), YT, FX(m), VB, C_P, 1);
        if (FX(m) < X1 - 34) txt((int)FX(m) - 12, 144, 24, F_S, C_DIM, AL_C, "%d", m);
    }
    txt(X1 - 22, 144, 22, F_S, C_DIM, AL_R, "min");
    ln(X0, FV(0), X1, FV(0), C_DIM, 1);
    txt(0, (int)FV(0) - 6, X0 - 4, F_S, C_DIM, AL_R, "0");
    txt(0, (int)FV(-60) - 9, X0 - 4, F_S, C_DIM, AL_R, "-60");
    for (i = 1; i < h->n; i++) {
        const hist_pt_t *p = &h->p[i - 1], *q = &h->p[i];
        ln(FX(p->t_s / 60.0), FA(p->alt / 1000.0), FX(q->t_s / 60.0), FA(q->alt / 1000.0), C_CY, 2);
        ln(FX(p->t_s / 60.0), FV(p->vv), FX(q->t_s / 60.0), FV(q->vv), C_PK, 2);
    }
    if (h->n && A->pred.valid && fresh()) {
        double ground = A->cfg.ground_alt > -900 ? A->cfg.ground_alt : 0;
        dash(FX(t_last), FA(A->sonde.alt / 1000.0), FX(t_last + land_rem_s() / 60.0), FA(ground / 1000.0), C_YL, 2, 3, 3);
    }
    if (A->flight.burst && h->n) {
        double bt = (A->flight.burst_t - h->t0) / 60.0, bx = FX(bt), by = FA(A->flight.burst_alt / 1000.0);
        ln(bx - 3, by - 3, bx + 3, by + 3, C_TX, 2);
        ln(bx - 3, by + 3, bx + 3, by - 3, C_TX, 2);
        txt((int)bx - 98, (int)by - 15, 90, F_S, C_TX, AL_R, "Burst %.1f km", A->flight.burst_alt / 1000.0);
    }
    if (A->have_sonde) {
        txt(26, 18, 80, F_M, C_CY, AL_L, "Alt %.0f m", A->sonde.alt);
        txt(106, 18, 86, F_M, C_PK, AL_L, "V/S %+.1f m/s", A->vv_s);
        if (A->pred.valid && fresh()) txt(192, 18, 96, F_M, C_YL, AL_L, "Landing T-%02d:%02d", land_rem_s() / 60, land_rem_s() % 60);
    }
    txt(286, 19, 30, F_S, C_DIM, AL_R, "2/2");
    nav_bar(PG_GRAPH);
}

/* ---------------- 7 Radio: spectrum, listening, decoding ---------------- */
/* Arrow key glyph centred on (x, y), the fonts have none: dir 0 left, 1 right, 2 up, 3 down. */
static void key_arrow(int x, int y, int dir, uint32_t col)
{
    static const int dx[] = {-1, 1, 0, 0}, dy[] = {0, 0, -1, 1};
    double tx = x + 4 * dx[dir], ty = y + 4 * dy[dir]; /* tip */
    ln(x - 4 * dx[dir], y - 4 * dy[dir], tx, ty, col, 1);
    ln(tx, ty, tx - 3 * dx[dir] - 3 * dy[dir], ty - 3 * dy[dir] + 3 * dx[dir], col, 1);
    ln(tx, ty, tx - 3 * dx[dir] + 3 * dy[dir], ty - 3 * dy[dir] - 3 * dx[dir], col, 1);
}

static void scan_hints(const char *third, const char *fourth)
{
    key_arrow(10, 147, 0, C_DIM);
    key_arrow(23, 147, 1, C_DIM);
    txt(32, 140, 60, F_S, C_DIM, AL_L, "10 kHz");
    key_arrow(92, 147, 2, C_DIM);
    key_arrow(103, 147, 3, C_DIM);
    txt(112, 140, 60, F_S, C_DIM, AL_L, "100 kHz");
    txt(176, 140, 80, F_S, C_DIM, AL_L, "%s", third);
    txt(254, 140, 60, F_S, C_DIM, AL_R, "%s", fourth);
}

/* Spectrum trace between YT and YB over cells c0 .. c0 + nc - 1: one column = max of the cells it covers;
 * dB range lo..hi. */
static void spec_trace(const spectrum_t *sp, int c0, int nc, int X0, int W, int YT, int YB, double lo, double hi)
{
    double prev_y = 0;
    int x, have = 0;
    for (x = 0; x < W && nc > 0; x++) {
        int b0 = c0 + x * nc / W, b1 = c0 + (x + 1) * nc / W, b, ok = 0;
        double v = -1e9, y;
        for (b = b0; b < b1 || b == b0; b++)
            if (b >= 0 && b < sp->n && sp->ok[b]) { ok = 1; if (sp->db[b] > v) v = sp->db[b]; }
        if (!ok) { have = 0; continue; }
        y = YB - ((v < lo ? lo : v) - lo) / (hi - lo) * (YB - YT - 14);
        if (y < YT) y = YT;
        if (have) ln(X0 + x - 1, prev_y, X0 + x, y, C_CY, 2);
        prev_y = y;
        have = 1;
    }
}

/* Tuned view, like an SDR app: the receiver's own samples around the tuned frequency, which stays in the
 * middle while Left/Right and Up/Down move it and the spectrum scrolls past; its audio plays meanwhile.
 * Enter sets the decode on it. */
static void p_scan_live(void)
{
    spectrum_t *sp = &A->live;
    const int X0 = 6, W = 308, YT = 22, YB = 122, XC = X0 + W / 2;
    const long lo_hz = A->freq_hz - LIVE_HALF_SPAN_HZ, span = 2 * LIVE_HALF_SPAN_HZ;
    const known_t *kn = app_known(A, A->freq_hz);
    peak_t pk[MAX_PEAKS];
    double lo, hi;
    char mid[64];
    long f;
    spec_peaks(sp, 8.0, pk, MAX_PEAKS); /* floor and max for the scale */
    lo = sp->floor_db - 4;
    hi = sp->max_db > sp->floor_db + 26 ? sp->max_db + 4 : sp->floor_db + 30;
    if (A->state == ST_LISTEN)
        snprintf(mid, sizeof mid, "%.3f MHz   %s", A->freq_hz / 1e6, kn ? "listening, RS41 seen here" : "listening");
    else if (kn && sc_now() - kn->t <= A->cfg.stale_s)
        snprintf(mid, sizeof mid, "%.3f MHz   RS41 %s   receiving", A->freq_hz / 1e6, kn->id);
    else
        snprintf(mid, sizeof mid, "%.3f MHz   decoding", A->freq_hz / 1e6);
    top_bar("Radio", C_CY, mid);
    fill(X0, YT, W, YB - YT, C_P);
    for (f = (lo_hz + 99999) / 100000 * 100000; f <= lo_hz + span; f += 100000) { /* 100 kHz grid */
        int x = X0 + (int)((double)(f - lo_hz) / span * W);
        ln(x, YT, x, YB, C_L, 1);
        if (f % 200000 == 0 && x > X0 + 18 && x < X0 + W - 18)
            txt(x - 20, 124, 40, F_S, C_DIM, AL_C, "%.1f", f / 1e6);
    }
    ln(XC, YT, XC, YB, C_PK, 1);
    spec_trace(sp, (int)((lo_hz - sp->f0) / sp->step), (int)(span / sp->step), X0, W, YT, YB, lo, hi);
    fill(XC - 24, YT + 2, 48, 13, C_PK);
    txt(XC - 24, YT + 2, 48, F_S, C_G, AL_C, "%.3f", A->freq_hz / 1e6);
    if (kn) {
        fill(XC - 30, YT + 16, 60, 12, C_GN);
        txt(XC - 30, YT + 15, 60, F_S, C_G, AL_C, "%s", kn->id);
    }
    { /* the channel being heard: a bar under the spectrum, one-sided for USB / LSB */
        const radio_t *r = &A->radio;
        int bw = (int)ceil((double)r->bw_hz / span * W), x0 = r->mode == DEMOD_USB ? XC : (r->mode == DEMOD_LSB ? XC - bw : XC - bw / 2);
        fill(x0, YB - 3, bw > 1 ? bw : 2, 3, C_YL);
        txt(X0 + 4, YT + 2, 90, F_S, C_YL, AL_L, "%s %.1f kHz", radio_mode_name(r->mode), r->bw_hz / 1000.0);
        txt(X0 + W - 104, YT + 2, 100, F_S, C_DIM, AL_R, "B  width   D  mode");
    }
    scan_hints(A->state == ST_LISTEN ? "Enter  decode" : "S  band", A->state == ST_LISTEN ? "S  band" : "F  freq");
    nav_bar(PG_RADIO);
}

/* Band overview: the whole scan band swept continuously, a free cursor to pick where to listen. */
static void p_scan(void)
{
    spectrum_t *sp = &A->spec;
    const int X0 = 6, W = 308, YT = 22, YB = 122;
    double fspan = (double)(A->cfg.scan_hi_hz - A->cfg.scan_lo_hz), lo, hi, xc;
    const known_t *kn;
    peak_t pk[MAX_PEAKS];
    char mid[96];
    int i, cx;

    if ((A->state == ST_LISTEN || A->state == ST_DECODE) && A->dsdr) {
        p_scan_live();
        return;
    }
    spec_peaks(sp, 8.0, pk, MAX_PEAKS); /* floor and max for the scale */
    lo = sp->floor_db - 4;
    hi = sp->max_db > sp->floor_db + 26 ? sp->max_db + 4 : sp->floor_db + 30;
    kn = app_known(A, A->cursor_hz);
    if (A->state == ST_SCAN)
        snprintf(mid, sizeof mid, "%.3f MHz   %s", A->cursor_hz / 1e6, kn ? kn->id : "scanning");
    else
        snprintf(mid, sizeof mid, "%s", A->status);
    top_bar("Radio", C_CY, mid);
    fill(X0, YT, W, YB - YT, C_P);
    for (i = 1; i < 6; i++) ln(X0 + i * W / 6.0, YT, X0 + i * W / 6.0, YB, C_L, 1);
    spec_trace(sp, 0, sp->n, X0, W, YT, YB, lo, hi);
    for (i = 0; i < MAX_KNOWN; i++) { /* sondes decoded this session, named where they were */
        const known_t *k = &A->known[i];
        double xk = X0 + (k->f_hz - A->cfg.scan_lo_hz) / fspan * W;
        int lx = (int)xk - 30 < X0 ? X0 : ((int)xk + 30 > X0 + W ? X0 + W - 60 : (int)xk - 30);
        if (!k->f_hz || k->f_hz < A->cfg.scan_lo_hz || k->f_hz > A->cfg.scan_hi_hz) continue;
        fill(lx, YT + 16, 60, 12, C_GN);
        txt(lx, YT + 15, 60, F_S, C_G, AL_C, "%s", k->id);
    }
    if (A->state == ST_SCAN) {
        xc = X0 + (A->cursor_hz - A->cfg.scan_lo_hz) / fspan * W;
        cx = (int)xc - 24 < X0 ? X0 : ((int)xc + 24 > X0 + W ? X0 + W - 48 : (int)xc - 24);
        ln(xc, YT, xc, YB, C_PK, 1);
        fill(cx, YT + 2, 48, 13, C_PK);
        txt(cx, YT + 2, 48, F_S, C_G, AL_C, "%.3f", A->cursor_hz / 1e6);
    }
    for (i = 0; i <= 6; i++)
        txt(X0 + i * W / 6 - (i == 0 ? 0 : (i == 6 ? 40 : 20)), 124, 40, F_S, C_DIM,
            i == 0 ? AL_L : (i == 6 ? AL_R : AL_C), "%.0f", (A->cfg.scan_lo_hz + i * fspan / 6) / 1e6);
    if (A->state == ST_SCAN) scan_hints("Enter  listen", "F  freq");
    else txt(6, 140, 308, F_S, C_DIM, AL_L, "S  scan the band      F  enter a frequency");
    nav_bar(PG_RADIO);
}

/* ---------------- 8 Info ---------------- */
static void p_info(void)
{
    static const char *src[] = {"none", "from GPS", "from compass", "held"};
    const sonde_t *s = &A->sonde;
    /* until the sonde's GPS locks, its telemetry comes from the frames without a position */
    const sonde_t *t = A->have_sonde ? s : (A->nofix.t_rx > 0 ? &A->nofix : NULL);
    top_bar("Info", C_CY, NULL);
    box(4, 19, 154, 114, C_PK, C_P);
    txt(11, 22, 100, F_S, C_PK, AL_L, "Sonde");
    if (t) {
        kv(11, 36, 140, "Serial", C_TX, "%s", t->id);
        kv(11, 50, 140, "Model", C_TX, "%s", t->subtype[0] ? t->subtype : "RS41");
        kv(11, 64, 140, "Frame", C_TX, "%d", t->frame);
        kv(11, 78, 140, "Satellites", t->sats < 4 ? C_RD : C_TX, "%d", t->sats);
        kv(11, 92, 140, "Battery", C_TX, "%.1f V", t->batt);
        if (A->have_sonde) kv(11, 106, 140, "Phase", C_TX, "%s", phase_name());
        else kv(11, 106, 140, "Phase", C_RD, "no GPS fix");
    } else {
        txt(11, 36, 140, F_S, C_DIM, AL_L, "No frame decoded yet");
    }
    box(162, 19, 154, 70, C_L, C_P);
    txt(169, 22, 100, F_S, C_TX, AL_L, "Reception");
    kv(169, 36, 140, "Frames decoded", C_TX, "%u", A->frames);
    if (t) { /* newest frame, with or without a position */
        double ago = sc_now() - (A->nofix.t_rx > s->t_rx ? A->nofix.t_rx : s->t_rx);
        kv(169, 50, 140, "Last frame", ago <= A->cfg.stale_s ? C_TX : C_RD, "%.0f s ago", ago);
    }
    kv(169, 64, 140, "Gain", C_TX, "%s", A->cfg.gain[0] ? A->cfg.gain : "auto");
    kv(169, 76, 140, "Correction", C_TX, "%d ppm", A->cfg.ppm);
    box(162, 91, 154, 42, C_CY, C_P);
    txt(169, 93, 100, F_S, C_CY, AL_L, "This device");
    txt(169, 93, 140, F_S, C_DIM, AL_R, "v" SC_VERSION);
    kv(169, 105, 140, "GPS", C_TX, "%s", gps_ok(&A->gps, sc_now()) ? (A->gps.mode == 3 ? "3D fix" : "2D fix") : "no fix");
    if (A->hdg.src != HDG_NONE) kv(169, 117, 140, "Heading", C_TX, "%03.0f° %s", A->hdg.deg, src[A->hdg.src]);
    else kv(169, 117, 140, "Heading", C_DIM, "unknown");
    if (A->have_sonde) {
        int pred = A->pred.valid && fresh();
        double la = pred ? A->pred.lat : s->lat, lo = pred ? A->pred.lon : s->lon;
        box(4, 136, 312, 17, pred ? C_YL : C_RD, C_P);
        txt(11, 138, 110, F_S, pred ? C_YL : C_RD, AL_L, pred ? "Predicted landing" : "Last fix");
        txt(120, 138, 190, F_S, C_TX, AL_R, "%.5f %c     %.5f %c", fabs(la), la < 0 ? 'S' : 'N', fabs(lo), lo < 0 ? 'W' : 'E');
    }
    nav_bar(PG_INFO);
}

/* ---------------- overlays ---------------- */
static void p_freq(void)
{
    top_bar("Tune", C_CY, NULL);
    box(44, 30, 232, 92, C_CY, C_P);
    txt(58, 40, 200, F_M, C_DIM, AL_L, "Frequency, kHz");
    txt(58, 56, 200, F_BIG, C_TX, AL_L, "%s_", entry_buf);
    txt(58, 100, 210, F_S, C_DIM, AL_L, "%ld to %ld, in 10 kHz steps", A->cfg.scan_lo_hz / 1000, A->cfg.scan_hi_hz / 1000);
    txt(44, 130, 50, F_S, C_DIM, AL_L, "0-9  type");
    txt(100, 130, 54, F_S, C_DIM, AL_L, "Bksp  del");
    txt(156, 130, 56, F_S, C_DIM, AL_L, "Enter  tune");
    txt(222, 130, 54, F_S, C_DIM, AL_R, "Esc  cancel");
    nav_bar(PG_RADIO);
}

static void stripes(int y)
{
    int x, i;
    fill(0, y, 320, 6, C_YL);
    for (x = -6; x < 320; x += 12)
        for (i = 0; i < 6; i++) ln(x + i, y + 6, x + i + 6, y, C_G, 1);
}

static void stat_box(int x, int w, const char *k, uint32_t col, const char *unit, const char *fmt, ...)
{
    char buf[24];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    box(x, 82, w, 44, col, C_P);
    txt(x + 7, 85, w - 10, F_S, C_DIM, AL_L, "%s", k);
    txt(x + 7, 99, w - 10, F_NUM, col, AL_L, "%s", buf);
    txt(x + 7, 106, w - 14, F_S, C_DIM, AL_R, "%s", unit);
}

static void p_burst(void)
{
    char u[24];
    double fl = A->flight.burst_t - A->flight.t_first;
    top_bar("Alert", C_YL, NULL);
    stripes(16);
    txt(10, 26, 300, F_TTL, C_YL, AL_L, "Balloon burst");
    txt(10, 60, 300, F_M, C_TX, AL_L, "At %.0f m after %d h %02d min. The sonde is now falling.",
        A->flight.burst_alt, (int)(fl / 3600), (int)(fl / 60) % 60);
    stat_box(10, 92, "Falling at", C_PK, "m/s", "%.0f", -A->vv_s);
    if (A->pred.valid) stat_box(108, 92, "Landing in", C_YL, "min", "%d", (land_rem_s() + 30) / 60);
    if (A->have_land) {
        snprintf(u, sizeof u, "km  %03.0f°", A->land_brg);
        stat_box(206, 104, "Landing point", C_YL, u, "%.1f", A->land_dist / 1000.0);
    }
    txt(10, 134, 300, F_S, C_DIM, AL_L, "Enter  show landing point on map");
    txt(10, 134, 300, F_S, C_DIM, AL_R, "Esc  dismiss");
    stripes(150);
    nav_bar(page);
}

static void stripes_col(int y, uint32_t col)
{
    int x, i;
    fill(0, y, 320, 6, col);
    for (x = -6; x < 320; x += 12)
        for (i = 0; i < 6; i++) ln(x + i, y + 6, x + i + 6, y, C_G, 1);
}

/* Blocking screen: nothing can be received without the USB receiver. It clears by itself
 * when the device appears (app_poll re-checks USB every 2 s). */
static void p_nosdr(void)
{
    int i;
    fill(0, 0, 44, 14, C_RD);
    for (i = 0; i < 6; i++) ln(38 + i, 14 - (i + 1) * 14 / 6.0, 38 + i, 14, C_G, 1);
    txt(5, 0, 44, F_S, C_G, AL_L, "Error");
    txt(160, 0, 155, F_S, C_DIM, AL_R, "Receiver stopped");
    ln(0, 15, 320, 15, C_L, 1);
    stripes_col(16, C_RD);
    txt(10, 28, 300, F_TTL, C_RD, AL_L, "No SDR device found");
    txt(10, 60, 300, F_M, C_TX, AL_L, "An RTL-SDR on USB is required to receive.");
    txt(10, 82, 12, F_M, C_RD, AL_L, "1");
    txt(24, 82, 290, F_M, C_TX, AL_L, "Plug the RTL-SDR into a USB port.");
    txt(10, 98, 12, F_M, C_RD, AL_L, "2");
    txt(24, 98, 290, F_M, C_TX, AL_L, "Set the Host / Slave switch (left side) to Host.");
    txt(10, 114, 12, F_M, C_RD, AL_L, "3");
    txt(24, 114, 290, F_M, C_TX, AL_L, "Reception resumes when the device appears.");
    txt(10, 142, 300, F_S, C_DIM, AL_L, "Checking USB every 2 s");
    txt(10, 142, 300, F_S, C_DIM, AL_R, "Q  quit");
    stripes_col(164, C_RD);
}

/* ---------------- LVGL glue ---------------- */
static void draw_cb(lv_event_t *e)
{
    LY = lv_event_get_layer(e);
    read_battery();
    fill(0, 0, 320, 170, C_G);
    if (!A->sdr_ok) p_nosdr();
    else if (A->burst_alert) p_burst();
    else if (entry) p_freq();
    else if (page == PG_TRACK) p_track();
    else if (page == PG_MAP) p_map();
    else if (page == PG_GRAPH) { if (gpage == 0) p_skewt(); else p_flight(); }
    else if (page == PG_RADIO) p_scan();
    else p_info();
    { /* hold-Esc-to-quit countdown over every screen; a short Esc press never shows it */
        double h = keys_esc_held_s();
        if (h >= 0.4) {
            double f = h >= ESC_QUIT_S ? 1.0 : h / ESC_QUIT_S;
            box(60, 56, 200, 58, C_RD, C_G);
            txt(60, 61, 200, F_M, C_TX, AL_C, "Hold Esc to quit");
            fill(76, 84, 168, 8, C_P);
            fill(76, 84, (int)(168 * f), 8, C_RD);
            txt(60, 95, 200, F_S, C_DIM, AL_C, "Quitting in %.0f s", ceil(ESC_QUIT_S - h));
        }
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    lv_obj_invalidate(root);
}

static const lv_font_t *load_font(const char *file, int size, const lv_font_t *fallback)
{
    char p[256];
    lv_font_t *f;
    snprintf(p, sizeof p, "A:%s/%s", A->cfg.font_dir, file);
    if (access(p + 2, R_OK) != 0) return fallback;
    f = lv_tiny_ttf_create_file(p, size);
    return f ? f : fallback;
}

void ui_init(app_t *a)
{
    A = a;
    F_S = load_font("Rajdhani-SemiBold.ttf", 12, &lv_font_montserrat_10);
    F_M = load_font("Rajdhani-SemiBold.ttf", 14, &lv_font_montserrat_12);
    F_NUM = load_font("Orbitron-Bold.ttf", 17, &lv_font_montserrat_16);
    F_TTL = load_font("Orbitron-Bold.ttf", 25, &lv_font_montserrat_24);
    F_BIG = load_font("Orbitron-Bold.ttf", 30, &lv_font_montserrat_28);
    root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 320, 170);
    lv_obj_add_event_cb(root, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_timer_create(tick_cb, 200, NULL);
    if (a->freq_hz <= 0) { /* nothing to decode yet: open on a band overview */
        page = PG_RADIO;
        app_scan(a);
    }
    a->radio_audio = page == PG_RADIO;
}

int ui_should_quit(void)
{
    return quit;
}

/* Left/Right: one RS41 channel (10 kHz); Up/Down: 100 kHz. */
static long tune_step(int k)
{
    long d = (k == K_LEFT || k == K_RIGHT) ? TUNE_STEP_HZ : TUNE_BIG_STEP_HZ;
    return (k == K_RIGHT || k == K_UP) ? d : -d;
}

void ui_key(int k)
{
    if (k & K_REPEAT) { /* a held arrow keeps tuning on the spectrum; elsewhere it acts once */
        if (page != PG_RADIO || entry || A->burst_alert) return;
        k &= ~K_REPEAT;
    }
    if (!A->sdr_ok) { /* blocking error screen */
        if (k == 'q') quit = 1;
    } else if (A->burst_alert) { /* alert overlay takes Enter / Esc only */
        if (k == K_ENTER) { A->burst_alert = 0; page = PG_MAP; map_follow = 0; map_zoom = -1; }
        else if (k == K_ESC) A->burst_alert = 0;
    } else if (entry) {
        if (k >= '0' && k <= '9' && entry_len < 6) { entry_buf[entry_len++] = (char)k; entry_buf[entry_len] = 0; }
        else if (k == K_BKSP && entry_len > 0) entry_buf[--entry_len] = 0;
        else if (k == K_ESC) entry = 0;
        else if (k == K_ENTER) {
            long khz = strtol(entry_buf, NULL, 10);
            if (khz >= 24000 && khz <= 1766000) { app_tune(A, khz * 1000L); page = PG_RADIO; }
            entry = 0;
        }
    } else if (k >= '4' && k <= '8') page = k - '4'; /* keys 4-8 sit right under the nav bar labels */
    else if (k == 'q') quit = 1;
    else if (k == 'm') A->prox.muted = !A->prox.muted;
    else if (k == 's') { app_scan(A); page = PG_RADIO; }
    else if (k == 'f') { entry = 1; entry_len = 0; entry_buf[0] = 0; }
    else if (k == 'r' && A->freq_hz > 0) app_tune(A, A->freq_hz);
    else if (k == 'x') app_stop(A);
    else if (page == PG_RADIO && k == 'b') radio_init(&A->radio, A->radio.mode, radio_next_bw(A->radio.mode, A->radio.bw_hz));
    else if (page == PG_RADIO && k == 'd') { /* NFM -> AM -> USB -> LSB, each with its usual width */
        int m = (A->radio.mode + 1) % DEMOD_N;
        radio_init(&A->radio, m, radio_default_bw(m));
    }
    else if (page == PG_RADIO && A->dsdr && (k == K_LEFT || k == K_RIGHT || k == K_UP || k == K_DOWN)) { /* tune */
        long f = A->freq_hz + tune_step(k);
        if (f >= 24000000L && f <= 1766000000L) app_listen(A, f);
    } else if (page == PG_RADIO && A->dsdr && A->state == ST_LISTEN && k == K_ENTER) app_tune(A, A->freq_hz);
    else if (page == PG_RADIO && A->state == ST_SCAN && (k == K_LEFT || k == K_RIGHT || k == K_UP || k == K_DOWN)) {
        long f = A->cursor_hz + tune_step(k); /* the overview's cursor, kept inside the band */
        A->cursor_hz = f < A->cfg.scan_lo_hz ? A->cfg.scan_lo_hz : (f > A->cfg.scan_hi_hz ? A->cfg.scan_hi_hz : f);
    } else if (page == PG_RADIO && A->state == ST_SCAN && k == K_ENTER) { /* -> tuned view */
        if (A->cfg.demo) app_tune(A, A->cursor_hz); /* no receiver to listen with */
        else app_listen(A, A->cursor_hz);
    }
    else if (page == PG_GRAPH && (k == K_LEFT || k == K_RIGHT)) gpage ^= 1;
    else if (page == PG_MAP && (k == K_UP || k == '+' || k == '=')) {
        if (map_zoom < 0) map_zoom = m_z;
        if (map_zoom < A->cfg.zoom_max) map_zoom++;
    } else if (page == PG_MAP && (k == K_DOWN || k == '-')) {
        if (map_zoom < 0) map_zoom = m_z;
        if (map_zoom > A->cfg.zoom_min) map_zoom--;
    } else if (page == PG_MAP && k == 'c') { map_follow = (map_follow + 1) % 3; map_zoom = -1; }
    else if (page == PG_MAP && k == 'v') { /* simple <-> detailed: tiles are recoloured again as they are drawn */
        int i;
        A->cfg.map_simple = !A->cfg.map_simple;
        for (i = 0; i < TC_N; i++) tc[i].valid = 0;
    }
    A->radio_audio = page == PG_RADIO; /* the tuned frequency is heard while its spectrum is on screen */
    if (root) lv_obj_invalidate(root);
}
