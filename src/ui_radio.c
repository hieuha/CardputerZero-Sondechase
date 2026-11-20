/* Radio screen: band overview, tuned spectrum, listening and decoding. */
#define _GNU_SOURCE
#include "ui_priv.h"

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
    if (A->hunt) /* Scan trying this peak: seconds left before it moves on */
        snprintf(mid, sizeof mid, "%.3f MHz   %.0f s", A->freq_hz / 1e6, HUNT_TRY_S - (sc_now() - A->t_try));
    else if (A->state == ST_DECODE && kn && sc_now() - kn->t <= A->cfg.stale_s)
        snprintf(mid, sizeof mid, "%.3f MHz   RS41 %s", A->freq_hz / 1e6, kn->id); /* RX n s on the right */
    else
        snprintf(mid, sizeof mid, "%.3f MHz", A->freq_hz / 1e6);
    top_bar("Radio", C_CY, mid);
    fill(X0, YT, W, YB - YT, C_P);
    for (f = (lo_hz + 99999) / 100000 * 100000; f <= lo_hz + span; f += 100000) { /* 100 kHz grid */
        int x = X0 + (int)((double)(f - lo_hz) / span * W);
        ln(x, YT, x, YB, C_L, 1);
        if (f % 200000 == 0 && x > X0 + 18 && x < X0 + W - 18)
            txt(x - 20, 124, 40, F_S, C_DIM, AL_C, "%.1f", f / 1e6);
    }
    { /* the channel being heard, shaded under the trace: as wide as the filter, one-sided for USB / LSB */
        const radio_t *r = &A->radio;
        int bw = (int)ceil((double)r->bw_hz / span * W), x0;
        if (bw < 3) bw = 3;
        x0 = r->mode == DEMOD_USB ? XC : (r->mode == DEMOD_LSB ? XC - bw : XC - bw / 2);
        fill_opa(x0, YT, bw, YB - YT, C_PK, LV_OPA_30);
        ln(x0, YT, x0, YB, C_PK, 1);
        ln(x0 + bw - 1, YT, x0 + bw - 1, YB, C_PK, 1);
    }
    ln(XC, YT, XC, YB, C_PK, 1);
    spec_trace(sp, (int)((lo_hz - sp->f0) / sp->step), (int)(span / sp->step), X0, W, YT, YB, lo, hi);
    fill(XC - 24, YT + 2, 48, 13, C_PK);
    txt(XC - 24, YT + 2, 48, F_S, C_G, AL_C, "%.3f", A->freq_hz / 1e6);
    if (kn) {
        fill(XC - 30, YT + 16, 60, 12, C_GN);
        txt(XC - 30, YT + 15, 60, F_S, C_G, AL_C, "%s", kn->id);
    }
    {
        const radio_t *r = &A->radio;
        txt(X0 + 4, YT + 2, 90, F_S, C_YL, AL_L, "%s %.1f kHz", radio_mode_name(r->mode), r->bw_hz / 1000.0);
        txt(X0 + W - 104, YT + 2, 100, F_S, C_DIM, AL_R, "B  width   D  mode");
    }
    if (A->hunt) scan_hints("Esc  stop", "F  freq");
    else scan_hints(A->state == ST_LISTEN ? "Enter  decode" : "S  scan", A->state == ST_LISTEN ? "S  scan" : "F  freq");
    nav_bar(PG_RADIO);
}

/* Band overview: the whole scan band swept continuously, a free cursor to pick where to listen. */
void p_scan(void)
{
    spectrum_t *sp = &A->spec;
    const int X0 = 6, W = 308, YT = 22, YB = 122;
    double fspan = (double)(A->cfg.scan_hi_hz - A->cfg.scan_lo_hz), lo, hi, xc;
    const known_t *kn;
    peak_t pk[MAX_PEAKS];
    char mid[96];
    int i, cx, np;

    if ((A->state == ST_LISTEN || A->state == ST_DECODE) && A->dsdr) {
        p_scan_live();
        return;
    }
    np = spec_peaks(sp, 8.0, pk, MAX_PEAKS); /* floor and max for the scale, and the peaks */
    lo = sp->floor_db - 4;
    hi = sp->max_db > sp->floor_db + 26 ? sp->max_db + 4 : sp->floor_db + 30;
    kn = app_known(A, A->cursor_hz);
    if (A->state == ST_SCAN)
        snprintf(mid, sizeof mid, "%.3f MHz   %s", A->cursor_hz / 1e6, kn ? kn->id : (A->hunt ? "looking for RS41" : ""));
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
    for (i = 0; i < MAX_PEAKS && A->state == ST_SCAN; i++) { /* peaks Scan would try; dim: tried lately, no frame */
        double xp;
        if (i >= np || pk[i].snr_db < HUNT_SNR_DB) continue;
        xp = X0 + (pk[i].f_hz - A->cfg.scan_lo_hz) / fspan * W;
        fill((int)xp - 2, YB - 6, 5, 5, app_skipped(A, pk[i].f_hz, sc_now()) ? C_DIM : C_YL);
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
    if (A->state == ST_SCAN) scan_hints("Enter  decode", "Esc  back");
    else txt(6, 140, 308, F_S, C_DIM, AL_L, "S  scan the band      F  enter a frequency");
    nav_bar(PG_RADIO);
}
