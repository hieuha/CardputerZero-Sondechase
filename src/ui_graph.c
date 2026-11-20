/* Graph screens: Skew-T (1/2) and the flight profile (2/2). */
#define _GNU_SOURCE
#include "ui_priv.h"

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

void p_skewt(void)
{
    static const int isob[] = {1000, 850, 700, 500, 300, 200, 100};
    static const double wl[] = {900, 700, 500, 400, 300, 200, 150};
    const hist_t *h = &A->hist;
    const sonde_t *s = &A->sonde;
    double zero_alt = -1, trop_alt = -1, tmin = 1e9;
    int i, th, t, p;

    top_bar("Graph 1/2", C_CY, NULL);
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

void p_flight(void)
{
    const hist_t *h = &A->hist;
    const int X0 = 26, X1 = 312, YT = 36, YB = 104, VT = 116, VB = 146;
    double tmax = 30, amax = 10, t_last = h->n ? h->p[h->n - 1].t_s / 60.0 : 0;
    int i, m, a;

    top_bar("Graph 2/2", C_CY, NULL);
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
    nav_bar(PG_GRAPH);
}
