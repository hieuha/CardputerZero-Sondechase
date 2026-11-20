/* sondechase UI: every screen is painted from one LVGL draw callback with three primitives
 * (line, rectangle, label) plus map tile images. Coordinates match the 320x170 mockup.
 * This file holds the overlays, the LVGL glue and the keys; each screen has its own ui_<screen>.c. */
#define _GNU_SOURCE
#include "ui_priv.h"
#include <stdlib.h>
#include <unistd.h>

static lv_obj_t *root;
static int page, gpage, ipage, entry, entry_len;
static char entry_buf[8];
static int scan_from = ST_IDLE; /* what the receiver did before S: Esc on Scan goes back to it */
static long scan_from_hz;

app_t *A;
lv_layer_t *LY;
const lv_font_t *F_S, *F_M, *F_NUM, *F_BIG, *F_TTL;
int cal_saved;
double cal_t;

/* ---------------- overlays ---------------- */
static void p_freq(void)
{
    long lo, hi;
    const char *tuner = sdr_range(A->rx, &lo, &hi);
    top_bar("Tune", C_CY, NULL);
    box(44, 30, 232, 92, C_CY, C_P);
    txt(58, 40, 200, F_M, C_DIM, AL_L, "Frequency, kHz");
    txt(58, 56, 200, F_BIG, C_TX, AL_L, "%s_", entry_buf);
    txt(58, 100, 210, F_S, C_DIM, AL_L, "%s: %ld to %ld", tuner, lo / 1000, hi / 1000);
    txt(44, 130, 50, F_S, C_DIM, AL_L, "0-9  type");
    txt(100, 130, 54, F_S, C_DIM, AL_L, "Bksp  del");
    txt(156, 130, 56, F_S, C_DIM, AL_L, "Enter  tune");
    txt(222, 130, 54, F_S, C_DIM, AL_R, "Esc  cancel");
    nav_bar(PG_RADIO);
}

static void stripes_col(int y, uint32_t col)
{
    int x, i;
    fill(0, y, 320, 6, col);
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
    stripes_col(16, C_YL);
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
    stripes_col(150, C_YL);
    nav_bar(page);
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
    txt(10, 60, 300, F_M, C_TX, AL_L, "An RTL-SDR or a HackRF on USB is required.");
    txt(10, 82, 12, F_M, C_RD, AL_L, "1");
    txt(24, 82, 290, F_M, C_TX, AL_L, "Plug the receiver into a USB port.");
    txt(10, 98, 12, F_M, C_RD, AL_L, "2");
    txt(24, 98, 290, F_M, C_TX, AL_L, "Set the Host / Slave switch (left side) to Host.");
    txt(10, 114, 12, F_M, C_RD, AL_L, "3");
    txt(24, 114, 290, F_M, C_TX, AL_L, "Reception resumes when the device appears.");
    txt(10, 142, 300, F_S, C_DIM, AL_L, "Checking USB every 2 s");
    txt(10, 142, 300, F_S, C_DIM, AL_R, "Hold Esc  quit");
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
    else if (page == PG_INFO && A->cal.step != MAGCAL_OFF) p_cal();
    else if (page == PG_INFO && ipage) p_info_pos();
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
    if (a->freq_hz <= 0) { /* nothing to decode yet: open on the Radio screen, listening mid-band like an SDR app */
        page = PG_RADIO;
        if (a->cfg.demo) app_scan(a); /* no receiver to listen with: the demo's simulated band overview */
        else app_listen(a, (a->cfg.scan_lo_hz + a->cfg.scan_hi_hz) / 2 / TUNE_STEP_HZ * TUNE_STEP_HZ);
    }
    a->radio_audio = page == PG_RADIO;
    a->frame_cue = page == PG_TRACK || page == PG_MAP;
}

/* Left/Right: one RS41 channel (10 kHz); Up/Down: 100 kHz. */
static long tune_step(int k)
{
    long d = (k == K_LEFT || k == K_RIGHT) ? TUNE_STEP_HZ : TUNE_BIG_STEP_HZ;
    return (k == K_RIGHT || k == K_UP) ? d : -d;
}

void ui_key(int k)
{
    if (k & K_REPEAT) { /* a held arrow keeps tuning the spectrum or panning the map; elsewhere it acts once */
        if ((page != PG_RADIO && page != PG_MAP) || entry || A->burst_alert) return;
        k &= ~K_REPEAT;
    }
    if (!A->sdr_ok) { /* blocking error screen: only holding Esc (quit) does anything */
    } else if (A->burst_alert) { /* alert overlay takes Enter / Esc only */
        if (k == K_ENTER) { A->burst_alert = 0; page = PG_MAP; map_follow = 0; map_zoom = -1; }
        else if (k == K_ESC) A->burst_alert = 0;
    } else if (A->cal.step != MAGCAL_OFF) { /* compass calibration takes Enter / Esc only */
        if (k == K_ESC || !A->hdg.mag_found) A->cal.step = MAGCAL_OFF;
        else if (k == K_ENTER && A->cal.step == MAGCAL_NORTH) {
            cal_saved = app_compass_save(A) == 0 ? 1 : -1;
            cal_t = sc_now();
        }
    } else if (entry) {
        if (k >= '0' && k <= '9' && entry_len < 7) { entry_buf[entry_len++] = (char)k; entry_buf[entry_len] = 0; }
        else if (k == K_BKSP && entry_len > 0) entry_buf[--entry_len] = 0;
        else if (k == K_ESC) entry = 0;
        else if (k == K_ENTER) {
            long khz = strtol(entry_buf, NULL, 10), lo, hi;
            sdr_range(A->rx, &lo, &hi);
            if (khz * 1000L >= lo && khz * 1000L <= hi) { app_tune(A, khz * 1000L); page = PG_RADIO; }
            entry = 0;
        }
    } else if (k >= '4' && k <= '8') page = k - '4'; /* keys 4-8 sit right under the nav bar labels */
    else if (k == K_TAB) page = (page + 1) % (PG_INFO + 1); /* next screen, Info wraps to Track */
    else if (k == 'm') A->prox.muted = !A->prox.muted;
    else if (k == 's') {
        if (A->state != ST_SCAN && !A->hunt) {
            scan_from = A->state;
            scan_from_hz = A->freq_hz;
        }
        app_hunt(A);
        page = PG_RADIO;
    } else if (page == PG_RADIO && (A->state == ST_SCAN || A->hunt) && k == K_ESC) { /* back to the frequency it left */
        long f = scan_from_hz > 0 ? scan_from_hz : (A->cfg.scan_lo_hz + A->cfg.scan_hi_hz) / 2 / TUNE_STEP_HZ * TUNE_STEP_HZ;
        if (scan_from == ST_DECODE || A->cfg.demo) app_tune(A, f); /* the demo has no receiver to listen with */
        else app_listen(A, f);
    }
    else if (k == 'f') { entry = 1; entry_len = 0; entry_buf[0] = 0; }
    else if (k == 'r' && A->freq_hz > 0) app_tune(A, A->freq_hz);
    else if (k == 'x') app_stop(A);
    else if (page == PG_RADIO && k == 'b') radio_init(&A->radio, A->radio.mode, radio_next_bw(A->radio.mode, A->radio.bw_hz));
    else if (page == PG_RADIO && k == 'd') { /* NFM -> AM -> USB -> LSB -> WFM, each with its usual width */
        int m = (A->radio.mode + 1) % DEMOD_N;
        radio_init(&A->radio, m, radio_default_bw(m));
    }
    else if (page == PG_RADIO && A->dsdr && (k == K_LEFT || k == K_RIGHT || k == K_UP || k == K_DOWN)) { /* tune */
        long f = A->freq_hz + tune_step(k), lo, hi;
        sdr_range(A->rx, &lo, &hi);
        if (f >= lo && f <= hi) app_listen(A, f);
    } else if (page == PG_RADIO && A->dsdr && A->state == ST_LISTEN && k == K_ENTER) app_tune(A, A->freq_hz);
    else if (page == PG_RADIO && A->state == ST_SCAN && (k == K_LEFT || k == K_RIGHT || k == K_UP || k == K_DOWN)) {
        long f = A->cursor_hz + tune_step(k); /* the overview's cursor, kept inside the band; picking by hand ends Scan's */
        A->hunt = 0;
        A->cursor_hz = f < A->cfg.scan_lo_hz ? A->cfg.scan_lo_hz : (f > A->cfg.scan_hi_hz ? A->cfg.scan_hi_hz : f);
    } else if (page == PG_RADIO && A->state == ST_SCAN && k == K_ENTER) app_tune(A, A->cursor_hz); /* -> tuned view, heard and decoded */
    else if (page == PG_GRAPH && (k == K_LEFT || k == K_RIGHT)) gpage ^= 1;
    else if (page == PG_INFO && (k == K_LEFT || k == K_RIGHT)) ipage ^= 1;
    else if (page == PG_INFO && k == 'k') magcal_start(&A->cal, mag_min_span(&A->hdg), A->hdg.acc_found);
    else if (page == PG_MAP && (k == K_LEFT || k == K_RIGHT || k == K_UP || k == K_DOWN)) map_pan(k);
    else if (page == PG_MAP && (k == '+' || k == '=')) map_zoom_step(1);
    else if (page == PG_MAP && k == '-') map_zoom_step(-1);
    else if (page == PG_MAP && k == 'c') map_follow_next();
    else if (page == PG_MAP && k == 'v') map_style_toggle();
    A->radio_audio = page == PG_RADIO; /* the tuned frequency is heard while its spectrum is on screen */
    A->frame_cue = page == PG_TRACK || page == PG_MAP; /* the screens used while chasing */
    if (root) lv_obj_invalidate(root);
}
