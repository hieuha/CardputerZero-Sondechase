/* Info screens: sonde and device details (1/2), exact numbers (2/2), compass calibration. */
#define _GNU_SOURCE
#include "ui_priv.h"

/* ---------------- 8 Info ---------------- */
/* result of the last calibration, over the bottom line of Info for 4 s */
static void cal_toast(void)
{
    if (!cal_saved || sc_now() - cal_t > 4.0) return;
    box(4, 135, 312, 17, cal_saved > 0 ? C_GN : C_RD, C_G);
    txt(10, 137, 300, F_S, cal_saved > 0 ? C_GN : C_RD, AL_C, "%s",
        cal_saved < 0 ? "Calibrated, but compass.conf could not be written"
        : A->cfg.mag_tilt ? "Compass calibrated and saved, works tilted"
                          : "Compass calibrated and saved: hold it flat");
}

/* Compass calibration, opened with K on Info: turn a full circle, lean four ways (with an accelerometer), then
 * point north and save. */
void p_cal(void)
{
    const magcal_t *m = &A->cal;
    const double cx = 68, cy = 86;
    double x0, y0, x1, y1;
    int k, cov = magcal_covered(m), steps = m->use_acc ? 3 : 2;
    top_bar("Compass", C_YL, "Calibration");
    if (m->step == MAGCAL_TILT) { /* four arcs: forward, right, back, left, lit once leaned that way far enough */
        for (k = 0; k < 4; k++) {
            int d, on = m->tilt_seen[k];
            for (d = -36; d < 36; d += 8) {
                pol(cx, cy, 54, k * 90 + d + 1, &x0, &y0);
                pol(cx, cy, 54, k * 90 + d + 7, &x1, &y1);
                ln(x0, y0, x1, y1, on ? C_GN : C_L, 5);
            }
        }
    } else
        for (k = 0; k < MAGCAL_SECTORS; k++) { /* one lit sector per 10° the turn has passed through */
            int on = m->step == MAGCAL_NORTH || m->seen[k];
            pol(cx, cy, 54, k * 10 + 1, &x0, &y0);
            pol(cx, cy, 54, k * 10 + 9, &x1, &y1);
            ln(x0, y0, x1, y1, on ? C_GN : C_L, 5);
        }
    if (!A->hdg.mag_found) {
        txt(140, 22, 174, F_M, C_RD, AL_L, "No compass sensor");
        txt(140, 42, 174, F_S, C_TX, AL_L, "No magnetometer was found, or");
        txt(140, 55, 174, F_S, C_TX, AL_L, "mag_enable = 0 in the config.");
        txt(140, 73, 174, F_S, C_DIM, AL_L, "Heading comes from GPS while");
        txt(140, 86, 174, F_S, C_DIM, AL_L, "moving faster than 5 km/h.");
    } else if (m->step == MAGCAL_TURN) {
        txt(18, 72, 100, F_BIG, C_TX, AL_C, "%d", cov * 100 / MAGCAL_SECTORS);
        txt(18, 104, 100, F_S, C_DIM, AL_C, "%% of a full turn");
        txt(140, 22, 174, F_M, C_YL, AL_L, "Step 1 of %d", steps);
        txt(140, 42, 174, F_S, C_TX, AL_L, "Hold the device flat, away from");
        txt(140, 55, 174, F_S, C_TX, AL_L, "cars and metal.");
        txt(140, 73, 174, F_S, C_TX, AL_L, "Turn slowly to your right, a full");
        txt(140, 86, 174, F_S, C_TX, AL_L, "circle, until the ring is all green.");
        txt(140, 106, 174, F_S, C_DIM, AL_L, "Next step starts by itself.");
    } else if (m->step == MAGCAL_TILT) {
        txt(18, 72, 100, F_BIG, C_TX, AL_C, "%d/4", magcal_tilt_covered(m));
        txt(18, 104, 100, F_S, C_DIM, AL_C, "directions");
        txt(140, 22, 174, F_M, C_YL, AL_L, "Step 2 of 3");
        txt(140, 42, 174, F_S, C_TX, AL_L, "Lean the device slowly about 30°");
        txt(140, 55, 174, F_S, C_TX, AL_L, "forward, back, left and right,");
        txt(140, 68, 174, F_S, C_TX, AL_L, "back to flat between each.");
        txt(140, 86, 174, F_S, C_DIM, AL_L, "This lets the compass work");
        txt(140, 99, 174, F_S, C_DIM, AL_L, "when it is not held flat.");
    } else {
        ln(cx, cy - 40, cx, cy + 20, C_CY, 3);
        ln(cx, cy - 40, cx - 8, cy - 28, C_CY, 3);
        ln(cx, cy - 40, cx + 8, cy - 28, C_CY, 3);
        txt((int)cx - 20, (int)cy + 26, 40, F_M, C_CY, AL_C, "N ?");
        txt(140, 22, 174, F_M, C_YL, AL_L, "Step %d of %d", steps, steps);
        txt(140, 42, 174, F_S, C_TX, AL_L, "Point the top of the device");
        txt(140, 55, 174, F_S, C_TX, AL_L, "North, using a phone compass,");
        txt(140, 68, 174, F_S, C_TX, AL_L, "hold it flat, press Enter.");
        txt(140, 106, 174, F_S, C_DIM, AL_L, "Saved for next time.");
    }
    box(140, 132, 174, 18, C_L, C_P);
    if (A->hdg.mag_found && m->step == MAGCAL_NORTH) txt(146, 134, 80, F_S, C_TX, AL_L, "Enter  save");
    txt(230, 134, 78, F_S, C_DIM, AL_R, "Esc  %s", A->hdg.mag_found ? "cancel" : "close");
    nav_bar(PG_INFO);
}

/* Info page 2/2: the exact numbers behind the plain-language Track screen */
void p_info_pos(void)
{
    const sonde_t *s = &A->sonde;
    const gps_t *g = &A->gps;
    char c[48];
    top_bar("Info 2/2", C_CY, NULL);
    box(4, 19, 154, 114, C_PK, C_P);
    txt(11, 22, 100, F_S, C_PK, AL_L, "Sonde");
    if (A->have_sonde) {
        kv(11, 36, 140, "Latitude", C_TX, "%.5f %c", fabs(s->lat), s->lat < 0 ? 'S' : 'N');
        kv(11, 48, 140, "Longitude", C_TX, "%.5f %c", fabs(s->lon), s->lon < 0 ? 'W' : 'E');
        kv(11, 60, 140, "Altitude", C_TX, "%.0f m", s->alt);
        kv(11, 72, 140, "Vertical speed", C_TX, "%+.1f m/s", A->vv_s);
        kv(11, 84, 140, "Ground speed", C_TX, "%.1f m/s  %03.0f°", s->vel_h, s->heading);
        if (s->temp > -270) kv(11, 96, 140, "Temperature", C_TX, "%.1f °C", s->temp);
        if (s->humidity >= 0) kv(11, 108, 140, "Humidity", C_TX, "%.0f %%", s->humidity);
        kv(11, 120, 140, "Pressure", C_TX, "%.0f hPa%s", s->pressure, s->pres_est ? " est" : "");
    } else txt(11, 36, 140, F_S, C_DIM, AL_L, "No position yet");
    box(162, 19, 154, 114, C_CY, C_P);
    txt(169, 22, 140, F_S, C_CY, AL_L, "From this device");
    if (A->have_geo) {
        char num[16]; const char *unit = "";
        fmt_dist(num, sizeof num, &unit, A->dist);
        kv(169, 36, 140, "Distance", C_TX, "%s %s", num, unit);
        kv(169, 48, 140, "Bearing", C_TX, "%03.0f°", A->brg);
        kv(169, 60, 140, "Turn", C_TX, "%+.0f°", A->rel);
        kv(169, 72, 140, "Elevation", C_TX, "%+.1f°", A->elev);
    } else txt(169, 36, 140, F_S, C_DIM, AL_L, "Needs a GPS fix");
    if (g->t_fix > 0) {
        kv(169, 84, 140, "Device lat", C_TX, "%.5f %c", fabs(g->lat), g->lat < 0 ? 'S' : 'N');
        kv(169, 96, 140, "Device lon", C_TX, "%.5f %c", fabs(g->lon), g->lon < 0 ? 'W' : 'E');
        kv(169, 108, 140, "Device alt", C_TX, "%.0f m", g->alt);
    }
    txt(169, 120, 140, F_S, C_DIM, AL_L, "K  calibrate the compass");
    if (A->pred.valid) {
        snprintf(c, sizeof c, "%.5f %c  %.5f %c", fabs(A->pred.lat), A->pred.lat < 0 ? 'S' : 'N', fabs(A->pred.lon), A->pred.lon < 0 ? 'W' : 'E');
        txt(11, 138, 110, F_S, C_YL, AL_L, "Landing");
        if (A->have_land) txt(60, 138, 80, F_S, C_YL, AL_L, "%.1f km %03.0f°", A->land_dist / 1000, A->land_brg);
        txt(140, 138, 170, F_S, C_TX, AL_R, "%s", c);
    }
    cal_toast();
    nav_bar(PG_INFO);
}

void p_info(void)
{
    static const char *src[] = {"none", "from GPS", "from compass", "held"};
    const sonde_t *s = &A->sonde;
    /* until the sonde's GPS locks, its telemetry comes from the frames without a position */
    const sonde_t *t = A->have_sonde ? s : (A->nofix.t_rx > 0 ? &A->nofix : NULL);
    top_bar("Info 1/2", C_CY, NULL);
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
    {
        const char *fix = gps_ok(&A->gps, sc_now()) ? (A->gps.mode == 3 ? "3D fix" : "2D fix") : "no fix";
        if (gps_sky(&A->gps, sc_now())) kv(169, 105, 140, "GPS", C_TX, "%s, %d/%d sats", fix, A->gps.n_used, A->gps.n_sat);
        else kv(169, 105, 140, "GPS", C_TX, "%s", fix);
    }
    if (A->hdg.src != HDG_NONE) kv(169, 117, 140, "Heading", C_TX, "%03.0f° %s", A->hdg.deg, src[A->hdg.src]);
    else kv(169, 117, 140, "Heading", C_DIM, "unknown");
    if (A->have_sonde) {
        int pred = A->pred.valid && fresh();
        double la = pred ? A->pred.lat : s->lat, lo = pred ? A->pred.lon : s->lon;
        box(4, 136, 312, 17, pred ? C_YL : C_RD, C_P);
        txt(11, 138, 110, F_S, pred ? C_YL : C_RD, AL_L, pred ? "Predicted landing" : "Last fix");
        txt(120, 138, 190, F_S, C_TX, AL_R, "%.5f %c     %.5f %c", fabs(la), la < 0 ? 'S' : 'N', fabs(lo), lo < 0 ? 'W' : 'E');
    }
    cal_toast();
    nav_bar(PG_INFO);
}
