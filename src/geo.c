#define _GNU_SOURCE
#include "sc.h"
#include <math.h>
#include <string.h>

#define R_EARTH 6371008.8
#define D2R (M_PI / 180.0)
#define R2D (180.0 / M_PI)

double norm360(double d)
{
    d = fmod(d, 360.0);
    return d < 0 ? d + 360.0 : d;
}

double norm180(double d)
{
    d = norm360(d);
    return d > 180.0 ? d - 360.0 : d;
}

double geo_dist(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * D2R, p2 = lat2 * D2R;
    double sp = sin((p2 - p1) / 2), sl = sin((lon2 - lon1) * D2R / 2);
    double a = sp * sp + cos(p1) * cos(p2) * sl * sl;
    return 2.0 * R_EARTH * asin(sqrt(a > 1.0 ? 1.0 : a));
}

double geo_bearing(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * D2R, p2 = lat2 * D2R, dl = (lon2 - lon1) * D2R;
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    return norm360(atan2(y, x) * R2D);
}

void geo_dest(double lat, double lon, double brg_deg, double dist_m, double *lat2, double *lon2)
{
    double p1 = lat * D2R, l1 = lon * D2R, b = brg_deg * D2R, c = dist_m / R_EARTH;
    double p2 = asin(sin(p1) * cos(c) + cos(p1) * sin(c) * cos(b));
    double l2 = l1 + atan2(sin(b) * sin(c) * cos(p1), cos(c) - sin(p1) * sin(p2));
    *lat2 = p2 * R2D;
    *lon2 = norm180(l2 * R2D);
}

/* Elevation angle of a target at height h_tgt seen from h_obs, ground distance d,
 * on a spherical earth (no refraction). */
double geo_elev(double d, double h_obs, double h_tgt)
{
    double c = d / R_EARTH, r1 = R_EARTH + h_obs, r2 = R_EARTH + h_tgt;
    return atan2(r2 * cos(c) - r1, r2 * sin(c)) * R2D;
}

/* International Standard Atmosphere density. */
double isa_rho(double h)
{
    static const double hb[] = {0, 11000, 20000, 32000, 47000};
    static const double tb[] = {288.15, 216.65, 216.65, 228.65, 270.65};
    static const double lb[] = {-0.0065, 0.0, 0.001, 0.0028, 0.0};
    static const double pb[] = {101325.0, 22632.06, 5474.889, 868.0187, 110.9063};
    const double g0 = 9.80665, Rs = 287.053;
    int i = 0;
    if (h < 0) h = 0;
    if (h > 47000) h = 47000;
    while (i < 3 && h >= hb[i + 1]) i++;
    double dh = h - hb[i], T = tb[i] + lb[i] * dh, p;
    if (lb[i] != 0.0) p = pb[i] * pow(tb[i] / T, g0 / (Rs * lb[i]));
    else p = pb[i] * exp(-g0 * dh / (Rs * tb[i]));
    return p / (Rs * T);
}

double isa_p(double h)
{
    return isa_rho(h) * 287.053 * (h < 11000 ? 288.15 - 0.0065 * (h < 0 ? 0 : h)
                                   : h < 20000 ? 216.65
                                   : h < 32000 ? 216.65 + 0.001 * (h - 20000)
                                   : 228.65 + 0.0028 * ((h > 47000 ? 47000 : h) - 32000)) / 100.0;
}

double dewpoint(double t, double rh)
{
    double g;
    if (rh < 0.5) rh = 0.5;
    g = log(rh / 100.0) + 17.62 * t / (243.12 + t);
    return 243.12 * g / (17.62 - g);
}

/* ---------------- wind profile measured from the sonde's own ascent ---------------- */
static int wbin(double alt)
{
    int b = (int)(alt / WBIN_M);
    return b < 0 ? 0 : (b >= WBINS ? WBINS - 1 : b);
}

void wind_add(wind_t *w, double alt, double vel_h, double heading_deg)
{
    int b = wbin(alt);
    float e = (float)(vel_h * sin(heading_deg * D2R)), n = (float)(vel_h * cos(heading_deg * D2R));
    if (w->ok[b]) {
        w->e[b] = 0.7f * w->e[b] + 0.3f * e;
        w->n[b] = 0.7f * w->n[b] + 0.3f * n;
    } else {
        w->e[b] = e;
        w->n[b] = n;
        w->ok[b] = 1;
    }
}

void wind_get(const wind_t *w, double alt, double def_e, double def_n, double *e, double *n)
{
    int b = wbin(alt), d;
    for (d = 0; d < WBINS; d++) {
        int lo = b - d, hi = b + d;
        if (lo >= 0 && w->ok[lo]) { *e = w->e[lo]; *n = w->n[lo]; return; }
        if (hi < WBINS && w->ok[hi]) { *e = w->e[hi]; *n = w->n[hi]; return; }
    }
    *e = def_e;
    *n = def_n;
}

/* Landing prediction.
 * Descent under a parachute at terminal velocity: v(h) = v_sl * sqrt(rho0 / rho(h)).
 * While descending, v_sl is calibrated from the measured vertical speed.
 * Horizontal drift per layer uses the wind profile recorded during ascent, falling back
 * to the sonde's current horizontal velocity for layers that were not observed. */
pred_t predict_landing(const sonde_t *s, double vv, const wind_t *w, double ground_alt,
                       double burst_alt, double v0)
{
    pred_t r = {0, s->lat, s->lon, 0.0};
    const double rho0 = isa_rho(0), step = 100.0;
    double h = s->alt, xe = 0, xn = 0, t = 0, e, n, v_sl;
    double ce = s->vel_h * sin(s->heading * D2R), cn = s->vel_h * cos(s->heading * D2R);

    if (h <= ground_alt) return r;
    if (vv > 0.5) { /* ascending: climb to the assumed burst altitude first */
        double va = vv < 1.0 ? 1.0 : vv;
        while (h < burst_alt) {
            double dh = burst_alt - h < step ? burst_alt - h : step, dt = dh / va;
            wind_get(w, h + dh / 2, ce, cn, &e, &n);
            xe += e * dt; xn += n * dt; t += dt; h += dh;
        }
        v_sl = v0;
    } else if (vv < -0.5) {
        v_sl = -vv / sqrt(rho0 / isa_rho(h));
    } else {
        return r; /* floating or on the ground: no prediction */
    }
    if (v_sl < 0.5) v_sl = 0.5;
    while (h > ground_alt) {
        double dh = h - ground_alt < step ? h - ground_alt : step;
        double hm = h - dh / 2, dt = dh / (v_sl * sqrt(rho0 / isa_rho(hm)));
        wind_get(w, hm, ce, cn, &e, &n);
        xe += e * dt; xn += n * dt; t += dt; h -= dh;
    }
    geo_dest(s->lat, s->lon, norm360(atan2(xe, xn) * R2D), hypot(xe, xn), &r.lat, &r.lon);
    r.t_s = t;
    r.valid = 1;
    return r;
}

int sonde_parse(const char *line, sonde_t *s)
{
    double v;
    memset(s, 0, sizeof *s);
    if (!js_num(line, "lat", &s->lat) || !js_num(line, "lon", &s->lon) || !js_num(line, "alt", &s->alt))
        return 0;
    if (!js_str(line, "id", s->id, sizeof s->id)) strcpy(s->id, "?");
    if (js_num(line, "frame", &v)) s->frame = (int)v;
    /* Before its GPS locks (e.g. still on the ground indoors) an RS41 reports sats 0, a 1980 date
     * and a position that wanders around a meaningless point: keep the telemetry, not the position. */
    if (js_num(line, "sats", &v)) {
        s->sats = (int)v;
        if (s->sats < 4) {
            js_num(line, "batt", &s->batt);
            return 2;
        }
    }
    if (fabs(s->lat) < 1e-6 && fabs(s->lon) < 1e-6) return 0; /* no GPS solution on the sonde */
    if (fabs(s->lat) > 90.0 || fabs(s->lon) > 180.0) return 0;
    s->bt = -1;
    if (js_num(line, "bt", &v)) s->bt = (int)v;
    js_num(line, "vel_h", &s->vel_h);
    js_num(line, "vel_v", &s->vel_v);
    js_num(line, "heading", &s->heading);
    s->temp = -273.0;
    js_num(line, "temp", &s->temp);
    js_num(line, "batt", &s->batt);
    s->humidity = -1.0;
    js_num(line, "humidity", &s->humidity);
    s->pressure = -1.0;
    js_num(line, "pressure", &s->pressure);
    if (s->pressure <= 0.0) { /* RS41-SG has no pressure sensor */
        s->pressure = isa_p(s->alt);
        s->pres_est = 1;
    }
    js_str(line, "subtype", s->subtype, sizeof s->subtype);
    if (!js_str(line, "datetime", s->datetime, sizeof s->datetime) || strlen(s->datetime) < 19 ||
        s->datetime[4] != '-' || s->datetime[10] != 'T')
        s->datetime[0] = 0;
    return 1;
}
