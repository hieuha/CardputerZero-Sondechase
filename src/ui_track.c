/* Track screen: the compass and the plain-language way to the sonde. */
#define _GNU_SOURCE
#include "ui_priv.h"

/* ---------------- 4 Track ---------------- */
#define ON_COURSE_DEG 15.0 /* the arrow within this of straight ahead: the ring turns green */

void compass(double hdg, int have_rel, double rel, uint32_t col, int dashed, int have_land, double land_rel,
             uint32_t ring)
{
    const double cx = 68, cy = 86;
    double x0, y0, x1, y1, tx, ty, bx, by, hx, hy, lx, ly, rx, ry;
    static const char *card[] = {"N", "E", "S", "W"};
    int k;
    for (k = 0; k < 72; k++) { /* outer ring as a 72-gon */
        pol(cx, cy, 54, k * 5, &x0, &y0);
        pol(cx, cy, 54, k * 5 + 5, &x1, &y1);
        ln(x0, y0, x1, y1, ring, 2);
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

/* Track before this device has ever had a GPS fix: the sonde is still decoded and shown in full;
 * distance and direction are replaced by absolute coordinates. */
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
    txt(232, 21, 82, F_S, C_RD, AL_R, "Waiting for GPS");
    snprintf(c, sizeof c, "%.0f", s->alt);
    txt(140, 36, 150, F_BIG, ok ? C_TX : C_RD, AL_L, "%s", c);
    txt(146 + text_w(F_BIG, c), 40, 50, F_S, C_DIM, AL_L, "altitude"); /* stacked: a 5-digit altitude leaves ~70 px */
    txt(146 + text_w(F_BIG, c), 54, 50, F_M, C_DIM, AL_L, "m");
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

const char *phase_name(void)
{
    static const char *n[] = {"Tracking", "Ascending", "Descending", "Landed"};
    return n[A->flight.phase];
}

/* 8-point compass name of a bearing: plain words instead of degrees on the Track screen */
static const char *point8(double brg)
{
    static const char *n[] = {"North", "North-east", "East", "South-east", "South", "South-west", "West", "North-west"};
    return n[(int)floor(norm360(brg + 22.5) / 45.0) % 8];
}

/* which way to turn, from the sonde's angle off straight ahead; green while on course */
static const char *turn_words(double rel, uint32_t *col)
{
    double a = fabs(rel);
    *col = a <= ON_COURSE_DEG ? C_GN : C_TX;
    if (a <= ON_COURSE_DEG) return "Straight ahead";
    if (a <= 45.0) return rel < 0 ? "Bear left" : "Bear right";
    if (a <= 135.0) return rel < 0 ? "Turn left" : "Turn right";
    return "Turn around";
}

/* Track with a sonde position and a device position: distance, which way to go, height and drift in plain
 * words (the exact numbers are on Info 2/2). last = the device position is its last GPS fix, not a current one. */
static void track_panel(double dist, double brg, int last)
{
    const sonde_t *s = &A->sonde;
    int ok = fresh(), have_h = A->hdg.src != HDG_NONE;
    double hdg = have_h ? A->hdg.deg : 0.0, rel = norm180(brg - hdg);
    char num[16];
    const char *unit = "";
    uint32_t tc;
    /* the arrow points at the index on top when heading at the sonde: the ring turns green */
    compass(hdg, 1, rel, ok ? C_PK : C_RD, !ok || last, A->have_land && ok, norm180(A->land_brg - hdg),
            have_h && fabs(rel) <= ON_COURSE_DEG ? C_GN : C_L);
    if (ok) {
        box(140, 20, 62, 14, C_PK, C_G);
        txt(140, 20, 62, F_S, C_PK, AL_C, "%s", phase_name());
    } else {
        fill(140, 20, 88, 14, C_RD);
        txt(140, 20, 88, F_S, C_G, AL_C, "Signal lost %.0f s", sc_now() - s->t_rx);
    }
    if (last) txt(232, 21, 82, F_S, C_RD, AL_R, "GPS lost %.0f s", sc_now() - A->gps.t_fix);
    else if (!have_h) txt(232, 21, 82, F_S, C_DIM, AL_R, "North up");

    fmt_dist(num, sizeof num, &unit, dist);
    txt(140, 36, 150, F_BIG, last ? C_YL : (ok ? C_TX : C_RD), AL_L, "%s", num);
    txt(146 + text_w(F_BIG, num), 40, 80, F_S, C_DIM, AL_L, last ? "from last GPS" : "to the sonde");
    txt(146 + text_w(F_BIG, num), 54, 40, F_M, C_DIM, AL_L, "%s", unit);
    if (have_h) {
        const char *t = turn_words(rel, &tc);
        kv(140, 76, 174, "Go", tc, "%s, %s", t, point8(brg));
    } else kv(140, 76, 174, "Go", C_TX, "Head %s", point8(brg));
    if (A->flight.phase != PH_LANDED) { /* where to point an antenna: from the last fix too, like the distance */
        double el = geo_elev(dist, A->gps.alt, s->alt);
        kv(140, 118, 174, "Elevation", C_TX, "%.0f° %s the horizon", fabs(el), el >= -0.5 ? "above" : "below");
    }
    if (A->flight.phase == PH_LANDED) kv(140, 90, 174, "Height", C_TX, "%.0f m, landed", s->alt);
    else {
        kv(140, 90, 174, "Height", C_TX, "%.0f m, %s %.1f m/s", s->alt, A->vv_s < -0.05 ? "falling" : "rising",
           fabs(A->vv_s));
        kv(140, 104, 174, "Drifting", C_TX, "%s, %.0f km/h", point8(s->heading), s->vel_h * 3.6);
    }

    if (ok && A->have_land && !last) {
        fmt_dist(num, sizeof num, &unit, A->land_dist);
        static const char *ab[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        char when[24], where[48];
        int ww;
        snprintf(when, sizeof when, "in %s", land_rem_txt());
        ww = text_w(F_S, when) + 2; /* the time keeps its width, the place text gets the rest: no wrapping */
        snprintf(where, sizeof where, "Lands %s %s %s", num, unit, point8(A->land_brg));
        if (text_w(F_S, where) > 308 - ww - 150) /* "36.75 km South-east" with "1h21" does not fit: SE */
            snprintf(where, sizeof where, "Lands %s %s %s", num, unit, ab[(int)floor(norm360(A->land_brg + 22.5) / 45.0) % 8]);
        box(140, 132, 174, 18, C_YL, C_P);
        txt(146, 134, 308 - ww - 150, F_S, C_YL, AL_L, "%s", where);
        txt(308 - ww, 134, ww, F_S, C_YL, AL_R, "%s", when);
    } else if (!ok) {
        box(140, 132, 174, 18, C_RD, C_P);
        txt(146, 134, 40, F_S, C_DIM, AL_L, "Last fix");
        txt(186, 134, 122, F_S, C_TX, AL_R, "%.5f %c  %.5f %c", fabs(s->lat), s->lat < 0 ? 'S' : 'N', fabs(s->lon), s->lon < 0 ? 'W' : 'E');
    }
}

void p_track(void)
{
    top_bar("Track", C_CY, NULL);
    if (A->have_sonde && A->have_geo) {
        track_panel(A->dist, A->brg, 0);
    } else if (A->have_sonde && A->gps.t_fix > 0) { /* GPS dropped out: work from where this device last was */
        track_panel(geo_dist(A->gps.lat, A->gps.lon, A->sonde.lat, A->sonde.lon),
                    geo_bearing(A->gps.lat, A->gps.lon, A->sonde.lat, A->sonde.lon), 1);
    } else if (A->have_sonde) {
        p_track_nogps();
    } else {
        compass(A->hdg.src != HDG_NONE ? A->hdg.deg : 0.0, 0, 0.0, C_PK, 0, 0, 0.0, C_L);
        if (A->nofix.t_rx > 0 && sc_now() - A->nofix.t_rx < 10.0) {
            txt(140, 44, 174, F_M, C_CY, AL_L, "%s", A->nofix.id);
            txt(140, 60, 174, F_M, C_TX, AL_L, "Frame %d   batt %.1f V", A->nofix.frame, A->nofix.batt);
            txt(140, 80, 174, F_S, C_RD, AL_L, "Sonde has no GPS fix (%d sats)", A->nofix.sats);
            txt(140, 94, 174, F_S, C_DIM, AL_L, "Position appears once it locks");
        } else {
            txt(140, 60, 174, F_M, C_DIM, AL_L, A->state == ST_DECODE ? "Waiting for the first frame" : "No frequency tuned");
            txt(140, 76, 174, F_S, C_DIM, AL_L, "7 radio     F type a frequency");
        }
    }
    nav_bar(PG_TRACK);
}
