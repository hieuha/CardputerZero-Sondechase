#define _GNU_SOURCE
#include "sc.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void set_status(app_t *a, const char *s)
{
    snprintf(a->status, sizeof a->status, "%s", s);
}

/* Everything the app keeps goes under one folder, data_dir (default ~/sondechase). Versions before 0.5.1 wrote
 * ~/sondechase.csv and cached tiles in ~/.cache/sondechase/tiles: those are moved in once, if the new ones are
 * not there yet, so the recorded flights and the downloaded map stay usable offline. */
static void data_paths(cfg_t *c)
{
    const char *home = getenv("HOME");
    char from[320], to[320];
    if (!c->data_dir[0]) snprintf(c->data_dir, sizeof c->data_dir, "%s/sondechase", home ? home : "/tmp");
    if (!home) return;
    snprintf(from, sizeof from, "%s/sondechase.csv", home);
    snprintf(to, sizeof to, "%s/sondechase.csv", c->data_dir);
    if (access(from, F_OK) == 0 && access(to, F_OK) != 0) {
        mkdirs(to);
        rename(from, to);
    }
    if (c->tile_cache_dir[0]) return; /* a cache placed by hand stays where it is */
    snprintf(from, sizeof from, "%s/.cache/sondechase/tiles", home);
    snprintf(to, sizeof to, "%s/tiles", c->data_dir);
    if (access(from, F_OK) == 0 && access(to, F_OK) != 0) {
        mkdirs(to);
        rename(from, to);
    }
}

/* Keep the calibration just finished: in the running config and in <data_dir>/compass.conf for next time. */
int app_compass_save(app_t *a)
{
    char p[200];
    magcal_apply(&a->cal, &a->cfg);
    a->cal.step = MAGCAL_OFF;
    mkdirs(a->cfg.data_dir);
    snprintf(p, sizeof p, "%s/compass.conf", a->cfg.data_dir);
    return magcal_save(&a->cfg, p);
}

void app_init(app_t *a, const char *cfg_path)
{
    memset(a, 0, sizeof *a);
    cfg_defaults(&a->cfg);
    if (cfg_path) cfg_load(&a->cfg, cfg_path);
    data_paths(&a->cfg);
    { /* compass calibration saved by the app (Info, K) overrides the main config */
        char p[200];
        snprintf(p, sizeof p, "%s/compass.conf", a->cfg.data_dir);
        if (access(p, R_OK) == 0) cfg_load(&a->cfg, p);
    }
    proc_init(&a->proc);
    gps_init(&a->gps);
    heading_init(&a->hdg, &a->cfg);
    spec_reset(&a->spec, a->cfg.scan_lo_hz, a->cfg.scan_hi_hz, a->cfg.scan_step_hz);
    fft_init(&a->sw.fft, SWEEP_FFT);
    hist_reset(&a->hist);
    flight_reset(&a->flight);
    audio_init(&a->audio);
    led_open(&a->led, a->cfg.led_path);
    radio_init(&a->radio, DEMOD_NFM, radio_default_bw(DEMOD_NFM));
    tf_init(&a->tf, &a->cfg);
    a->freq_hz = a->cfg.freq_hz;
    a->sdr_ok = (a->cfg.sdr_check && !a->cfg.demo) ? sdr_present("/sys/bus/usb/devices") : 1;
    set_status(a, "idle");
    if (a->freq_hz > 0) app_tune(a, a->freq_hz);
}

/* ---------------- the dongle ---------------- */
/* Opened once and kept for the session: the sweep and the decoder take turns on it, sw.sdr / dsdr pointing
 * to a->rx while their job runs. Failures (librtlsdr0 missing, dongle busy) are shown on the status line. */
static sdr_t *rx_get(app_t *a, long rate)
{
    char err[96];
    if (!a->rx && !(a->rx = sdr_open(&a->cfg, rate, err, sizeof err))) {
        set_status(a, err);
        return NULL;
    }
    if (sdr_resume(a->rx, rate) < 0) {
        set_status(a, "cannot restart the receiver stream");
        return NULL;
    }
    return a->rx;
}

/* The stream ended (dongle unplugged): drop the device; the next job reopens it once it is back. */
static void rx_lost(app_t *a)
{
    sdr_close(a->rx);
    a->rx = NULL;
    a->sw.sdr = NULL;
    a->dsdr = NULL;
}

/* ---------------- the tuned receiver ---------------- */
/* Listening and decoding share it: the dongle at DDC_RATE, tuned DDC_RATE/4 above the stream centre, feeds
 * the DDC. Its 48 kHz output goes to the radio audio, and once a decode is set (Enter) also to the stdin of
 * decoder_feed_cmd (fsk_demod | rs41mod), whose JSON lines app_poll reads. */
static void rx_stop(app_t *a)
{
    if (a->dsdr) sdr_pause(a->dsdr);
    a->dsdr = NULL;
}

static void rx_tune(app_t *a)
{
    long centre = a->freq_hz - RS41_TUNE_OFFSET_HZ + DDC_RATE / 4;
    sdr_tune(a->dsdr, centre);
    spec_reset(&a->live, centre - DDC_RATE / 2, centre + DDC_RATE / 2, DDC_RATE / SWEEP_FFT);
    a->t_live = 0;
}

/* 0: receiving; -1: retried from app_poll at t_restart. */
static int rx_start(app_t *a)
{
    if (!a->sdr_ok) { /* the receiver is not on USB */
        a->t_restart = sc_now() + 1.0;
        return -1;
    }
    if (!(a->dsdr = rx_get(a, DDC_RATE))) {
        a->t_restart = sc_now() + 3.0;
        return -1;
    }
    ddc_init(&a->ddc);
    radio_init(&a->radio, a->radio.mode, a->radio.bw_hz); /* fresh filter state, same settings */
    rx_tune(a);
    return 0;
}

static void start_decoder(app_t *a)
{
    char cmd[1024];
    if (a->cfg.demo) { /* frames come from demo_poll */
        set_status(a, "decoding (demo)");
        return;
    }
    if (!a->dsdr && rx_start(a) < 0) return;
    if (sc_subst(cmd, sizeof cmd, a->cfg.decoder_feed_cmd, a->freq_hz) < 0 || proc_start_feed(&a->proc, cmd) < 0) {
        set_status(a, "decoder start failed");
        a->t_restart = sc_now() + 3.0;
        return;
    }
    set_status(a, "decoding");
}

/* Feed the decoder (when running) and the radio audio (while the UI asks for it) from the dongle, and refresh
 * the tuned spectrum about 20 times a second. */
static void rx_poll(app_t *a, double now)
{
    static unsigned char iq[65536];
    static int16_t out[2 * (sizeof iq / (2 * DDC_DECIM) + DDC_BLOCK / DDC_DECIM)];
    static int16_t wide[4 * sizeof out / sizeof out[0]];
    static int16_t pcm[sizeof wide / sizeof wide[0] / 24 + 2];
    static float db[SWEEP_FFT];
    size_t k, nw;
    int listen = a->radio_audio && !a->prox.muted, wfm = listen && a->radio.mode == DEMOD_WFM;
    if (!listen) audio_radio_clear(&a->audio);
    while ((k = sdr_read(a->dsdr, iq, sizeof iq)) > 0) {
        size_t n = ddc_process_wide(&a->ddc, iq, k, out, wfm ? wide : NULL, &nw);
        if (n && a->proc.pid > 0) proc_feed(&a->proc, out, n * 2 * sizeof out[0]);
        if (nw && wfm) audio_radio_push(&a->audio, pcm, radio_process_wfm(&a->radio, wide, nw, pcm));
        else if (n && listen && !wfm) audio_radio_push(&a->audio, pcm, radio_process(&a->radio, out, n, pcm));
        if (a->radio_audio && now - a->t_live >= 0.05 && k >= 2 * SWEEP_FFT * SWEEP_FRAMES) { /* only drawn there */
            int i, b = (int)(DC_ZONE_HZ / ((double)DDC_RATE / SWEEP_FFT)), l = SWEEP_FFT / 2 - b - 1, r = SWEEP_FFT / 2 + b + 1;
            fft_spectrum(&a->sw.fft, iq, SWEEP_FRAMES, db);
            for (i = l + 1; i < r; i++) db[i] = db[l] + (db[r] - db[l]) * (float)(i - l) / (float)(r - l); /* DC zone */
            spec_add_bins(&a->live, (double)a->live.f0, (double)a->live.step, db, SWEEP_FFT, SWEEP_DECAY_DB);
            a->t_live = now;
        }
    }
    if (!sdr_alive(a->dsdr)) { /* dongle unplugged: restart from scratch once it is back */
        rx_lost(a);
        proc_stop(&a->proc);
        set_status(a, "receiver stream ended");
        a->t_restart = now + 2.0;
    }
}

/* ---------------- in-process sweep (librtlsdr) ---------------- */
#define SWEEP_SETTLE_BYTES ((size_t)(SWEEP_RATE * 2 * 15 / 1000)) /* 15 ms of IQ after each retune */

/* Centre of the current step: odd passes are shifted half a step down (and take one step more), so each
 * pass measures the DC zones the other one skips. */
static double sweep_centre(const sweep_t *w)
{
    return w->f_first + w->step * w->f_step - ((w->passes & 1) ? w->f_step / 2.0 : 0.0);
}

static void sweep_tune(app_t *a)
{
    sweep_t *w = &a->sw;
    sdr_tune(w->sdr, (long)sweep_centre(w));
    w->skip = SWEEP_SETTLE_BYTES + SDR_USB_BUF_BYTES; /* settling time + a transfer begun before the retune */
    w->have = 0;
}

static void sweep_stop(app_t *a)
{
    if (a->sw.sdr) sdr_pause(a->sw.sdr);
    a->sw.sdr = NULL;
}

/* 0: sweeping; -1: the dongle could not be started (reason on the status line). */
static int sweep_start(app_t *a)
{
    sweep_t *w = &a->sw;
    if (!(w->sdr = rx_get(a, SWEEP_RATE))) return -1;
    w->f_step = SWEEP_RATE * SWEEP_KEEP; /* central part of each FFT; the band edges roll off */
    w->nstep = (int)ceil((double)(a->cfg.scan_hi_hz - a->cfg.scan_lo_hz) / w->f_step);
    if (w->nstep < 1) w->nstep = 1;
    w->f_first = (double)a->cfg.scan_lo_hz + w->f_step / 2.0;
    w->step = 0;
    w->passes = 0;
    sweep_tune(a);
    return 0;
}

void app_stop(app_t *a)
{
    a->hunt = 0;
    sweep_stop(a);
    rx_stop(a);
    proc_stop(&a->proc);
    a->state = ST_IDLE;
    set_status(a, "idle");
}

/* Set the decode: the tuned receiver (kept, retuned, when already listening) and the decoder on f_hz. */
void app_tune(app_t *a, long f_hz)
{
    a->hunt = 0;
    sweep_stop(a);
    proc_stop(&a->proc);
    a->freq_hz = f_hz;
    a->state = ST_DECODE;
    if (a->dsdr) rx_tune(a);
    start_decoder(a);
}

/* Tuning on the spectrum like an SDR app: spectrum and radio audio on f_hz, no decoder until it is set
 * with app_tune. Only the dongle moves when it is already receiving. */
void app_listen(app_t *a, long f_hz)
{
    a->hunt = 0;
    sweep_stop(a);
    proc_stop(&a->proc);
    a->freq_hz = f_hz;
    a->state = ST_LISTEN;
    set_status(a, "listening");
    if (a->cfg.demo) return;
    if (a->dsdr) rx_tune(a);
    else rx_start(a);
}

/* Band overview: continuous sweeps of scan_lo_hz..scan_hi_hz until a frequency is tuned or the scan is stopped.
 * The cursor starts on the current frequency, or mid-band. */
void app_scan(app_t *a)
{
    if (!a->sdr_ok) {
        set_status(a, "no SDR device");
        return;
    }
    rx_stop(a); /* one dongle: the tuned receiver hands it over */
    proc_stop(&a->proc);
    spec_reset(&a->spec, a->cfg.scan_lo_hz, a->cfg.scan_hi_hz, a->cfg.scan_step_hz);
    if (a->freq_hz >= a->cfg.scan_lo_hz && a->freq_hz <= a->cfg.scan_hi_hz) a->cursor_hz = a->freq_hz;
    else if (a->cursor_hz < a->cfg.scan_lo_hz || a->cursor_hz > a->cfg.scan_hi_hz)
        a->cursor_hz = (a->cfg.scan_lo_hz + a->cfg.scan_hi_hz) / 2 / TUNE_STEP_HZ * TUNE_STEP_HZ;
    if (a->cfg.demo) { /* demo_poll fills the spectrum after a simulated sweep time */
        a->state = ST_SCAN;
        a->t_restart = sc_now() + 1.5;
        set_status(a, "scanning");
        return;
    }
    if (sweep_start(a) < 0) {
        a->state = ST_IDLE;
        return;
    }
    a->state = ST_SCAN;
    set_status(a, "scanning");
}

/* Scan: the overview's sweep, then each peak in turn on the decoder. The demo has no sweep to look at. */
void app_hunt(app_t *a)
{
    app_scan(a);
    if (a->state != ST_SCAN || a->cfg.demo) return;
    a->hunt = 1;
    a->hunt_pass = 0;
    set_status(a, "scanning for sondes");
}

int app_skipped(const app_t *a, long f_hz, double now)
{
    int i;
    for (i = 0; i < HUNT_SKIP_N; i++)
        if (a->skip[i].f_hz && labs(a->skip[i].f_hz - f_hz) <= KNOWN_MATCH_HZ && now - a->skip[i].t < HUNT_SKIP_S)
            return 1;
    return 0;
}

static void skip_add(app_t *a, long f_hz, double now)
{
    int i, slot = 0;
    for (i = 1; i < HUNT_SKIP_N; i++)
        if (a->skip[i].t < a->skip[slot].t) slot = i;
    a->skip[slot].f_hz = f_hz;
    a->skip[slot].t = now;
}

/* After each full sweep pass (two, so both half-step offsets are measured): the strongest peak not tried
 * lately goes to the decoder. A frame from it ends the scan there; none within HUNT_TRY_S sends it back. */
static void hunt_poll(app_t *a, double now)
{
    char msg[80];
    if (a->state == ST_SCAN && a->sw.passes >= 2 && a->sw.passes != a->hunt_pass) {
        peak_t pk[MAX_PEAKS];
        int n = spec_peaks(&a->spec, HUNT_SNR_DB, pk, MAX_PEAKS), i, best = -1;
        a->hunt_pass = a->sw.passes;
        for (i = 0; i < n; i++)
            if (!app_skipped(a, pk[i].f_hz, now) && (best < 0 || pk[i].snr_db > pk[best].snr_db)) best = i;
        if (best < 0) return; /* nothing new: keep sweeping */
        app_tune(a, pk[best].f_hz);
        a->hunt = 1;
        a->t_try = now;
        snprintf(msg, sizeof msg, "trying %.3f MHz", pk[best].f_hz / 1e6);
        set_status(a, msg);
    } else if (a->state == ST_DECODE) {
        const known_t *k = app_known(a, a->freq_hz);
        if (k && k->t >= a->t_try) { /* an RS41 on this peak: stay */
            a->hunt = 0;
            snprintf(msg, sizeof msg, "found RS41 %s", k->id);
            set_status(a, msg);
            audio_tone(&a->audio, 1320, 0.10, 1.0);
            audio_tone(&a->audio, 1760, 0.16, 1.0);
        } else if (now - a->t_try > HUNT_TRY_S) {
            skip_add(a, a->freq_hz, now);
            app_scan(a); /* hunt stays set */
            a->hunt_pass = 0;
            if (a->state == ST_SCAN) set_status(a, "scanning for sondes");
            else a->hunt = 0;
        }
    }
}

/* ---------------- known frequencies ---------------- */
const known_t *app_known(const app_t *a, long f_hz)
{
    int i;
    for (i = 0; i < MAX_KNOWN; i++)
        if (a->known[i].f_hz && labs(a->known[i].f_hz - f_hz) <= KNOWN_MATCH_HZ) return &a->known[i];
    return NULL;
}

/* Remember the sonde decoded on f_hz; the oldest slot is reused. */
static void known_set(app_t *a, long f_hz, const char *id)
{
    int i, slot = 0;
    for (i = 0; i < MAX_KNOWN; i++) {
        if (a->known[i].f_hz && labs(a->known[i].f_hz - f_hz) <= KNOWN_MATCH_HZ) {
            slot = i;
            break;
        }
        if (a->known[i].t < a->known[slot].t) slot = i;
    }
    a->known[slot].f_hz = f_hz;
    snprintf(a->known[slot].id, sizeof a->known[slot].id, "%s", id);
    a->known[slot].t = sc_now();
}

/* One file per sonde in radiosonde_auto_rx's sonde log format, for the tools that read those logs:
 * <data_dir>/log/YYYYMMDD-HHMMSS_<serial>_RS41_<MHz>_sonde.log, times in UTC. Like auto_rx, times are
 * the sonde's own GPS time: the CardputerZero has no RTC, so its clock is wrong in the field without
 * network; the system clock is used only when the sonde sent none. SNR and frequency error are not
 * measured here and a missing sensor or burst timer is written as -1, like auto_rx does. */
static void log_autorx(app_t *a, const char *dir)
{
    const sonde_t *s = &a->sonde;
    char t[32];
    if (s->datetime[0]) {
        const char *ms = s->datetime[19] == '.' ? s->datetime + 20 : "000";
        snprintf(t, sizeof t, "%.19s.%.3sZ", s->datetime, ms);
    } else {
        struct timespec ts;
        struct tm tm;
        clock_gettime(CLOCK_REALTIME, &ts);
        gmtime_r(&ts.tv_sec, &tm);
        strftime(t, sizeof t, "%Y-%m-%dT%H:%M:%S", &tm);
        snprintf(t + 19, sizeof t - 19, ".%03ldZ", ts.tv_nsec / 1000000);
    }
    if (a->rlog && strcmp(a->rlog_id, s->id)) { /* another sonde: its own file */
        fclose(a->rlog);
        a->rlog = NULL;
    }
    if (!a->rlog) {
        char p[320];
        snprintf(p, sizeof p, "%s/log/%.4s%.2s%.2s-%.2s%.2s%.2s_%s_RS41_%.3f_sonde.log", dir, t, t + 5,
                 t + 8, t + 11, t + 14, t + 17, s->id, a->freq_hz / 1e6);
        mkdirs(p);
        if (!(a->rlog = fopen(p, "a"))) return;
        snprintf(a->rlog_id, sizeof a->rlog_id, "%s", s->id);
        fseek(a->rlog, 0, SEEK_END);
        if (ftell(a->rlog) == 0) /* reopened in the same second: keep the one header */
            fprintf(a->rlog, "timestamp,serial,frame,lat,lon,alt,vel_v,vel_h,heading,temp,humidity,pressure,type,"
                             "freq_mhz,snr,f_error_hz,sats,batt_v,burst_timer,aux_data\n");
    }
    fprintf(a->rlog, "%s,%s,%d,%.5f,%.5f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%s,%.3f,-1.0,-1,%d,%.1f,%d,-1\n",
            t, s->id, s->frame, s->lat, s->lon, s->alt, s->vel_v, s->vel_h, s->heading,
            s->temp, s->humidity, s->pres_est ? -1.0 : s->pressure, s->subtype[0] ? s->subtype : "RS41",
            a->freq_hz / 1e6, s->sats, s->batt, s->bt >= 0 && s->bt < 65535 ? s->bt : -1);
    fflush(a->rlog);
}

static void log_frame(app_t *a)
{
    const sonde_t *s = &a->sonde;
    const char *dir = a->cfg.data_dir;
    if (a->cfg.demo) return;
    log_autorx(a, dir);
    if (!a->log) {
        snprintf(a->log_path, sizeof a->log_path, "%s/sondechase.csv", dir);
        mkdirs(a->log_path);
        a->log = fopen(a->log_path, "a");
        if (!a->log) return;
        fseek(a->log, 0, SEEK_END);
        if (ftell(a->log) == 0) /* header once: the file collects every session */
            fprintf(a->log, "unix,id,frame,lat,lon,alt,vel_v,vel_h,heading,freq_hz,"
                            "own_lat,own_lon,pred_lat,pred_lon\n");
    }
    fprintf(a->log, "%ld,%s,%d,%.6f,%.6f,%.1f,%.2f,%.2f,%.1f,%ld,", (long)time(NULL), s->id, s->frame,
            s->lat, s->lon, s->alt, s->vel_v, s->vel_h, s->heading, a->freq_hz);
    if (gps_ok(&a->gps, sc_now())) fprintf(a->log, "%.6f,%.6f,", a->gps.lat, a->gps.lon);
    else fprintf(a->log, ",,");
    if (a->pred.valid) fprintf(a->log, "%.6f,%.6f\n", a->pred.lat, a->pred.lon);
    else fprintf(a->log, ",\n");
    fflush(a->log);
}

void app_on_frame(app_t *a, const char *line, double now)
{
    sonde_t s;
    double ground;
    int r = sonde_parse(line, &s);
    if (!r) return;
    s.t_rx = now;
    known_set(a, a->freq_hz, s.id);
    if (a->frame_cue) { /* on Track or Map: a short tick and a blink of the LED; M silences only the tick */
        if (!a->prox.muted) audio_tone(&a->audio, 1500, 0.03, 0.5);
        led_blink(&a->led, now, 0.08);
    }
    if (r == 2) { /* telemetry without a position: shown, but kept out of track, history and prediction */
        if (strcmp(s.id, a->have_sonde ? a->sonde.id : a->nofix.id)) a->frames = 0;
        a->nofix = s;
        a->frames++;
        return;
    }
    if (!a->have_sonde || strcmp(s.id, a->sonde.id)) { /* new sonde: discard old profile */
        memset(&a->wind, 0, sizeof a->wind);
        hist_reset(&a->hist);
        flight_reset(&a->flight);
        a->burst_alert = 0;
        if (strcmp(s.id, a->nofix.id)) a->frames = 0; /* keep counting a sonde whose GPS just locked */
        a->vv_s = s.vel_v;
    }
    a->sonde = s;
    a->have_sonde = 1;
    a->frames++;
    a->vv_s = 0.7 * a->vv_s + 0.3 * s.vel_v;
    if (s.vel_v > 1.0) wind_add(&a->wind, s.alt, s.vel_h, s.heading);
    hist_add(&a->hist, &s, now);
    if (flight_update(&a->flight, &s, now)) {
        a->burst_alert = 1;
        audio_tone(&a->audio, 880, 0.12, 1.0); /* three rising tones, played once */
        audio_tone(&a->audio, 1320, 0.12, 1.0);
        audio_tone(&a->audio, 1760, 0.20, 1.0);
    }
    ground = a->cfg.ground_alt > -900.0 ? a->cfg.ground_alt : (gps_ok(&a->gps, now) ? a->gps.alt : 0.0);
    a->pred = predict_landing(&s, a->vv_s, &a->wind, ground, a->cfg.burst_alt, a->cfg.descent_v0);
    log_frame(a);
}

static void sweep_poll(app_t *a)
{
    static unsigned char junk[SDR_USB_BUF_BYTES];
    sweep_t *w = &a->sw;
    const int nbin = (int)(SWEEP_FFT * SWEEP_KEEP), lo_bin = (SWEEP_FFT - nbin) / 2;
    const double bin_hz = (double)SWEEP_RATE / SWEEP_FFT;
    const int dc_bins = (int)(DC_ZONE_HZ / bin_hz);
    int i;
    for (;;) {
        size_t k;
        if (w->skip) {
            if (!(k = sdr_read(w->sdr, junk, w->skip < sizeof junk ? w->skip : sizeof junk))) break;
            w->skip -= k;
            continue;
        }
        if (!(k = sdr_read(w->sdr, w->buf + w->have, sizeof w->buf - w->have))) break;
        if ((w->have += k) < sizeof w->buf) continue;
        fft_spectrum(&w->fft, w->buf, SWEEP_FRAMES, w->db);
        for (i = SWEEP_FFT / 2 - dc_bins; i <= SWEEP_FFT / 2 + dc_bins; i++) w->db[i] = NAN; /* left alone */
        spec_add_bins(&a->spec, sweep_centre(w) - SWEEP_RATE / 2.0 + lo_bin * bin_hz, bin_hz, w->db + lo_bin, nbin,
                      SWEEP_DECAY_DB);
        if (++w->step == w->nstep + (int)(w->passes & 1)) { /* the sweep runs on like an SDR app's spectrum */
            w->step = 0;
            w->passes++;
        }
        sweep_tune(a);
    }
    if (!sdr_alive(w->sdr)) { /* dongle unplugged */
        rx_lost(a);
        a->state = ST_IDLE;
        set_status(a, "receiver stream ended");
    }
}

void app_poll(app_t *a)
{
    char line[LR_MAX];
    double now = sc_now();
    int r;

    if (a->cfg.demo) {
        demo_poll(a, now);
        goto derived;
    }
    gps_poll(&a->gps, &a->cfg);
    heading_update(&a->hdg, &a->gps, &a->cfg, now);
    if (a->cal.step != MAGCAL_OFF && now - a->cal.t_poll >= 0.05) { /* 20 Hz while calibrating */
        double x, y;
        a->cal.t_poll = now;
        if (mag_read(&a->hdg, &x, &y)) magcal_add(&a->cal, x, y);
    }
    if (a->cfg.sdr_check && now - a->t_sdr >= 2.0) {
        a->t_sdr = now;
        a->sdr_ok = sdr_present("/sys/bus/usb/devices");
        if (!a->sdr_ok && a->state == ST_SCAN) { /* receiver unplugged during a sweep */
            rx_lost(a);
            a->state = ST_IDLE;
            set_status(a, "no SDR device");
        }
    }

    if (a->state == ST_LISTEN || a->state == ST_DECODE) {
        if (a->dsdr) rx_poll(a, now);
        else if (a->state == ST_LISTEN && now >= a->t_restart) rx_start(a);
    }
    if (a->state == ST_DECODE) {
        if (a->proc.pid > 0) {
            while ((r = lr_readline(&a->proc.lr, line, sizeof line)) == 1)
                if (line[0] == '{') app_on_frame(a, line, now);
            if (r < 0) {
                proc_stop(&a->proc);
                set_status(a, "decoder exited, restarting");
                a->t_restart = now + 3.0;
            }
        } else if (now >= a->t_restart) {
            start_decoder(a);
        }
    } else if (a->state == ST_SCAN && a->sw.sdr) {
        sweep_poll(a);
    }
    if (a->hunt) hunt_poll(a, now);

derived:
    a->have_geo = a->have_land = 0;
    if (a->have_sonde && gps_ok(&a->gps, now)) {
        const gps_t *g = &a->gps;
        a->dist = geo_dist(g->lat, g->lon, a->sonde.lat, a->sonde.lon);
        a->brg = geo_bearing(g->lat, g->lon, a->sonde.lat, a->sonde.lon);
        a->elev = geo_elev(a->dist, g->alt, a->sonde.alt);
        a->rel = norm180(a->brg - (a->hdg.src != HDG_NONE ? a->hdg.deg : 0.0));
        a->have_geo = 1;
        if (a->pred.valid) {
            a->land_dist = geo_dist(g->lat, g->lon, a->pred.lat, a->pred.lon);
            a->land_brg = geo_bearing(g->lat, g->lon, a->pred.lat, a->pred.lon);
            a->have_land = 1;
        }
    }

    /* Proximity beeper: ground distance to the sonde (or to its last fix when the signal is lost).
     * Stale telemetry lowers the pitch so the two cases are told apart by ear. */
    if (prox_update(&a->prox, a->have_geo, a->dist, now, a->cfg.prox_radius_m, a->cfg.prox_hyst_m) &&
        a->audio.nseg == 0) {
        int stale = now - a->sonde.t_rx > a->cfg.stale_s;
        audio_tone(&a->audio, stale ? 1100 : 2200, 0.06, prox_volume(a->dist));
    }
    audio_poll(&a->audio, &a->cfg, now);
    led_poll(&a->led, now);
    tf_poll(&a->tf, &a->cfg, now);
}

void app_free(app_t *a)
{
    sweep_stop(a);
    rx_stop(a);
    rx_lost(a); /* closes the dongle */
    fft_free(&a->sw.fft);
    proc_stop(&a->proc);
    gps_close(&a->gps);
    audio_close(&a->audio);
    led_close(&a->led);
    tf_close(&a->tf);
    if (a->log) fclose(a->log);
    a->log = NULL;
    if (a->rlog) fclose(a->rlog);
    a->rlog = NULL;
}
