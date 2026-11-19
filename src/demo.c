/* Simulated flight for cfg.demo: produces the same JSON lines rs41mod would, so every code
 * path after the decoder (history, burst detection, prediction, beeper, UI) runs unchanged. */
#include "sc.h"
#include <math.h>
#include <string.h>

#define DEMO_T0 1000.0   /* virtual clock origin passed to app_on_frame */
#define DEMO_BURST 31200.0
#define DEMO_GROUND 10.0

static const double TAB_T[][2] = {{0, 27}, {2, 16}, {2.98, 12.5}, {5, 0}, {8, -20}, {11, -42}, {14, -62}, {16.5, -76}, {18, -72}, {22, -60}, {26, -52}, {31.2, -44}};
static const double TAB_RH[][2] = {{0, 82}, {1, 88}, {2, 70}, {2.98, 64}, {4, 35}, {5.5, 48}, {7, 22}, {9, 30}, {11, 14}, {14, 8}, {17, 4}, {22, 2}, {31.2, 1}};
static const double TAB_WS[][2] = {{0, 3}, {1, 6}, {2.98, 8}, {5, 12}, {8, 20}, {11, 31}, {13, 36}, {15, 28}, {18, 12}, {21, 6}, {24, 10}, {28, 16}, {31.2, 21}};
static const double TAB_WD[][2] = {{0, 265}, {2.98, 270}, {8, 255}, {13, 262}, {16, 250}, {18, 110}, {24, 95}, {31.2, 88}}; /* from */
#define N(t) ((int)(sizeof t / sizeof t[0]))

static double lut(const double t[][2], int n, double km)
{
    int i;
    if (km <= t[0][0]) return t[0][1];
    for (i = 1; i < n; i++)
        if (km <= t[i][0]) return t[i - 1][1] + (t[i][1] - t[i - 1][1]) * (km - t[i - 1][0]) / (t[i][0] - t[i - 1][0]);
    return t[n - 1][1];
}

/* Advance the simulated sonde by 1 s. Returns vertical speed; *ws and *hd get wind speed and
 * the direction the sonde moves towards. */
static double sim_step(app_t *a, double *ws, double *hd)
{
    double km = a->demo.alt / 1000.0, vv = 0;
    *ws = lut(TAB_WS, N(TAB_WS), km);
    *hd = norm360(lut(TAB_WD, N(TAB_WD), km) + 180.0);
    if (a->demo.phase == 0) {
        vv = 5.0 + 0.4 * sin(a->demo.tv / 37.0);
        a->demo.alt += vv;
        if (a->demo.alt >= DEMO_BURST) a->demo.phase = 1;
    } else if (a->demo.phase == 1) {
        vv = -6.0 * sqrt(isa_rho(0) / isa_rho(a->demo.alt));
        a->demo.alt += vv;
        if (a->demo.alt <= DEMO_GROUND) {
            a->demo.alt = DEMO_GROUND;
            a->demo.phase = 2;
            a->demo.t_land = a->demo.tv;
        }
    } else {
        *ws = 0;
    }
    if (*ws > 0) geo_dest(a->demo.lat, a->demo.lon, *hd, *ws, &a->demo.lat, &a->demo.lon);
    a->demo.tv += 1.0;
    return vv;
}

static void demo_reset(app_t *a)
{
    memset(&a->demo, 0, sizeof a->demo);
    a->demo.lat = 21.0300;
    a->demo.lon = 105.5200;
    a->demo.alt = DEMO_GROUND;
}

static void demo_init(app_t *a, double now)
{
    double ws, hd, lat, lon;
    demo_reset(a);
    while (a->demo.phase != 2) sim_step(a, &ws, &hd); /* dry run: find the landing point */
    geo_dest(a->demo.lat, a->demo.lon, 262.0, 9000.0, &lat, &lon);
    demo_reset(a);
    a->demo.own_lat = lat; /* this device waits 9 km short of the landing point */
    a->demo.own_lon = lon;
    a->demo.init = 1;
    a->demo.t_last = now;
}

static void demo_frame(app_t *a)
{
    char line[512];
    double ws, hd, vv = sim_step(a, &ws, &hd), km = a->demo.alt / 1000.0;
    if (a->demo.phase == 2 && a->demo.tv - a->demo.t_land > 90.0) return; /* below the horizon: signal lost */
    snprintf(line, sizeof line,
             "{\"type\":\"RS41\",\"frame\":%d,\"id\":\"S4430329\",\"lat\":%.5f,\"lon\":%.5f,\"alt\":%.1f,"
             "\"vel_h\":%.2f,\"heading\":%.1f,\"vel_v\":%.2f,\"sats\":9,\"batt\":%.2f,\"temp\":%.1f,"
             "\"humidity\":%.1f,\"pressure\":%.2f,\"subtype\":\"RS41-SGP\"}",
             ++a->demo.frame, a->demo.lat, a->demo.lon, a->demo.alt, ws, hd, vv, 2.9 - a->demo.tv * 3e-5,
             lut(TAB_T, N(TAB_T), km), lut(TAB_RH, N(TAB_RH), km), isa_p(a->demo.alt));
    app_on_frame(a, line, DEMO_T0 + a->demo.tv);
}

static void demo_scan(app_t *a)
{
    spectrum_t *s = &a->spec;
    unsigned r = 12345u;
    int i;
    for (i = 0; i < s->n; i++) {
        double f = (double)s->f0 + (double)i * (double)s->step, db;
        r = r * 1103515245u + 12345u;
        db = -42.0 + ((r >> 16) & 0xff) / 255.0 * 3.0;
        db += 20.0 * exp(-pow((f - 403.5e6) / 9000.0, 2)) + 11.0 * exp(-pow((f - 401.1e6) / 9000.0, 2));
        s->db[i] = (float)db;
        s->ok[i] = 1;
    }
    a->state = ST_IDLE;
    snprintf(a->status, sizeof a->status, "scan done");
}

void demo_advance(app_t *a, double virtual_seconds, double now)
{
    int i, n = (int)virtual_seconds;
    if (!a->demo.init) demo_init(a, now);
    for (i = 0; i < n; i++) demo_frame(a);
    if (n > 0 && a->have_sonde && !(a->demo.phase == 2 && a->demo.tv - a->demo.t_land > 90.0)) a->sonde.t_rx = now;
}

void demo_poll(app_t *a, double now)
{
    if (!a->demo.init) demo_init(a, now);
    a->sdr_ok = 1;
    a->gps.mode = 3;
    a->gps.lat = a->demo.own_lat;
    a->gps.lon = a->demo.own_lon;
    a->gps.alt = DEMO_GROUND;
    a->gps.speed = 13.3;
    a->gps.track = 52.0;
    a->gps.has_track = 1;
    a->gps.t_fix = now;
    a->hdg.deg = 52.0;
    a->hdg.src = HDG_GPS;
    if (a->state == ST_SCAN && now >= a->t_restart) demo_scan(a);
    if (a->state == ST_DECODE) {
        double due = (now - a->demo.t_last) * a->cfg.demo_speed;
        if (due >= 1.0) {
            demo_advance(a, due > 600.0 ? 600.0 : due, now);
            a->demo.t_last = now;
        }
    } else {
        a->demo.t_last = now;
    }
}
