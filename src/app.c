#define _GNU_SOURCE
#include "sc.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void set_status(app_t *a, const char *s)
{
    snprintf(a->status, sizeof a->status, "%s", s);
}

void app_init(app_t *a, const char *cfg_path)
{
    memset(a, 0, sizeof *a);
    cfg_defaults(&a->cfg);
    if (cfg_path) cfg_load(&a->cfg, cfg_path);
    proc_init(&a->proc);
    gps_init(&a->gps);
    heading_init(&a->hdg, &a->cfg);
    spec_reset(&a->spec, a->cfg.scan_lo_hz, a->cfg.scan_hi_hz, a->cfg.scan_step_hz);
    fft_init(&a->sw.fft, SWEEP_FFT);
    hist_reset(&a->hist);
    flight_reset(&a->flight);
    audio_init(&a->audio);
    tf_init(&a->tf, &a->cfg);
    a->freq_hz = a->cfg.freq_hz;
    a->sdr_ok = (a->cfg.sdr_check && !a->cfg.demo) ? sdr_present("/sys/bus/usb/devices") : 1;
    set_status(a, "idle");
    if (a->freq_hz > 0) app_tune(a, a->freq_hz);
}

static void decode_stop(app_t *a)
{
    sdr_close(a->dsdr);
    a->dsdr = NULL;
}

/* In-process decode: the dongle feeds the DDC, whose 48 kHz output goes to decoder_feed_cmd.
 * 0 on success; -1 to fall back to decoder_cmd (rtl_fm), with the reason in err. */
static int decode_start(app_t *a, char *cmd, size_t ncmd, char *err, size_t n)
{
    long centre = a->freq_hz - RS41_TUNE_OFFSET_HZ + DDC_RATE / 4;
    if (!strcmp(a->cfg.sdr_backend, "cmd")) {
        snprintf(err, n, "sdr_backend = cmd");
        return -1;
    }
    if (!a->dsdr && !(a->dsdr = sdr_open(&a->cfg, DDC_RATE, err, n))) return -1;
    sdr_tune(a->dsdr, centre);
    ddc_init(&a->ddc);
    spec_reset(&a->live, centre - DDC_RATE / 2, centre + DDC_RATE / 2, DDC_RATE / SWEEP_FFT);
    a->t_live = 0;
    if (sc_subst(cmd, ncmd, a->cfg.decoder_feed_cmd, &a->cfg, a->freq_hz) < 0 || proc_start_feed(&a->proc, cmd) < 0) {
        snprintf(err, n, "decoder start failed");
        decode_stop(a);
        return -1;
    }
    return 0;
}

static void start_decoder(app_t *a)
{
    char cmd[1024], err[96];
    if (a->cfg.demo) { /* frames come from demo_poll */
        set_status(a, "decoding (demo)");
        return;
    }
    if (!a->sdr_ok) { /* retried from app_poll once the receiver is back */
        a->t_restart = sc_now() + 1.0;
        return;
    }
    if (decode_start(a, cmd, sizeof cmd, err, sizeof err) == 0) {
        set_status(a, "decoding");
        return;
    }
    if (sc_subst(cmd, sizeof cmd, a->cfg.decoder_cmd, &a->cfg, a->freq_hz) < 0 ||
        proc_start(&a->proc, cmd) < 0) {
        set_status(a, "decoder start failed");
        a->t_restart = sc_now() + 3.0;
        return;
    }
    snprintf(a->status, sizeof a->status, "decoding with rtl_fm (%s)", err);
}

/* Feed the decoder from the dongle, and refresh the decode-window spectrum about 10 times a second. */
static void decode_poll(app_t *a, double now)
{
    static unsigned char iq[65536];
    static int16_t out[2 * (sizeof iq / (2 * DDC_DECIM) + DDC_BLOCK / DDC_DECIM)];
    static float db[SWEEP_FFT];
    size_t k;
    while ((k = sdr_read(a->dsdr, iq, sizeof iq)) > 0) {
        size_t n = ddc_process(&a->ddc, iq, k, out);
        if (n) proc_feed(&a->proc, out, n * 2 * sizeof out[0]);
        if (now - a->t_live >= 0.1 && k >= 2 * SWEEP_FFT * SWEEP_FRAMES) {
            fft_spectrum(&a->sw.fft, iq, SWEEP_FRAMES, db);
            spec_add_bins(&a->live, (double)a->live.f0, (double)a->live.step, db, SWEEP_FFT, SWEEP_DECAY_DB);
            a->t_live = now;
        }
    }
    if (!sdr_alive(a->dsdr)) { /* dongle unplugged: restart from scratch once it is back */
        decode_stop(a);
        proc_stop(&a->proc);
        set_status(a, "receiver stream ended");
        a->t_restart = now + 2.0;
    }
}

/* ---------------- in-process sweep (librtlsdr) ---------------- */
#define SWEEP_SETTLE_BYTES ((size_t)(SWEEP_RATE * 2 * 15 / 1000)) /* 15 ms of IQ after each retune */

static void sweep_tune(app_t *a)
{
    sweep_t *w = &a->sw;
    sdr_tune(w->sdr, (long)(w->f_first + w->step * w->f_step));
    w->skip = SWEEP_SETTLE_BYTES + SDR_USB_BUF_BYTES; /* settling time + a transfer begun before the retune */
    w->have = 0;
}

static void sweep_stop(app_t *a)
{
    sdr_close(a->sw.sdr);
    a->sw.sdr = NULL;
}

/* 0: the sweep runs in the app; -1: use the rtl_power command instead (reason in err). */
static int sweep_start(app_t *a, char *err, size_t n)
{
    sweep_t *w = &a->sw;
    if (!strcmp(a->cfg.sdr_backend, "cmd")) {
        snprintf(err, n, "sdr_backend = cmd");
        return -1;
    }
    if (!w->sdr && !(w->sdr = sdr_open(&a->cfg, SWEEP_RATE, err, n))) return -1;
    w->f_step = SWEEP_RATE * SWEEP_KEEP; /* central part of each FFT, like rtl_power -c */
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
    sweep_stop(a);
    decode_stop(a);
    proc_stop(&a->proc);
    a->state = ST_IDLE;
    set_status(a, "idle");
}

void app_tune(app_t *a, long f_hz)
{
    sweep_stop(a); /* the decoder needs the dongle (an in-process decode keeps it and just retunes) */
    proc_stop(&a->proc);
    a->freq_hz = f_hz;
    a->state = ST_DECODE;
    start_decoder(a);
}

void app_scan(app_t *a)
{
    char cmd[640], err[96];
    if (!a->sdr_ok) {
        set_status(a, "no SDR device");
        return;
    }
    decode_stop(a); /* one dongle: stop the decoder (or a previous rtl_power) first */
    proc_stop(&a->proc);
    spec_reset(&a->spec, a->cfg.scan_lo_hz, a->cfg.scan_hi_hz, a->cfg.scan_step_hz);
    a->npeaks = 0;
    a->sel = 0;
    if (a->cfg.demo) { /* demo_poll fills the spectrum after a simulated sweep time */
        a->state = ST_SCAN;
        a->t_restart = sc_now() + 1.5;
        set_status(a, "scanning");
        return;
    }
    if (sweep_start(a, err, sizeof err) == 0) {
        a->state = ST_SCAN;
        set_status(a, "scanning");
        return;
    }
    if (sc_subst(cmd, sizeof cmd, a->cfg.scan_cmd, &a->cfg, 0) < 0 || proc_start(&a->proc, cmd) < 0) {
        a->state = ST_IDLE;
        set_status(a, "scan start failed");
        return;
    }
    a->state = ST_SCAN;
    snprintf(a->status, sizeof a->status, "scanning with rtl_power (%s)", err);
}

static void log_frame(app_t *a)
{
    const sonde_t *s = &a->sonde;
    if (a->cfg.demo) return;
    if (!a->log) {
        const char *dir = a->cfg.log_dir[0] ? a->cfg.log_dir : getenv("HOME");
        snprintf(a->log_path, sizeof a->log_path, "%s/sondechase.csv", dir ? dir : "/tmp");
        a->log = fopen(a->log_path, "a");
        if (!a->log) return;
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

/* rtl_power sweeps continuously, so peaks follow the live spectrum. The selected peak stays
 * selected while it is still present (within 20 kHz), otherwise the strongest one is. */
static void scan_refresh(app_t *a)
{
    long keep = a->npeaks ? a->peaks[a->sel].f_hz : 0;
    int i;
    a->npeaks = spec_peaks(&a->spec, a->cfg.scan_snr_db, a->peaks, MAX_PEAKS);
    a->sel = 0;
    for (i = 0; keep && i < a->npeaks; i++)
        if (labs(a->peaks[i].f_hz - keep) <= 20000) {
            a->sel = i;
            break;
        }
}

static void sweep_poll(app_t *a)
{
    static unsigned char junk[SDR_USB_BUF_BYTES];
    sweep_t *w = &a->sw;
    const int nbin = (int)(SWEEP_FFT * SWEEP_KEEP), lo_bin = (SWEEP_FFT - nbin) / 2;
    const double bin_hz = (double)SWEEP_RATE / SWEEP_FFT;
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
        spec_add_bins(&a->spec, w->f_first + w->step * w->f_step - SWEEP_RATE / 2.0 + lo_bin * bin_hz, bin_hz,
                      w->db + lo_bin, nbin, SWEEP_DECAY_DB);
        scan_refresh(a);
        if (++w->step == w->nstep) {
            w->step = 0;
            if (++w->passes == 1 && a->cfg.auto_tune && a->npeaks) {
                app_tune(a, a->peaks[0].f_hz); /* first complete pass */
                return;
            }
        }
        sweep_tune(a);
    }
    if (!sdr_alive(w->sdr)) {
        sweep_stop(a);
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
    if (a->cfg.sdr_check && now - a->t_sdr >= 2.0) {
        a->t_sdr = now;
        a->sdr_ok = sdr_present("/sys/bus/usb/devices");
        if (!a->sdr_ok && a->state == ST_SCAN) { /* receiver unplugged during a sweep */
            sweep_stop(a);
            proc_stop(&a->proc);
            a->state = ST_IDLE;
            set_status(a, "no SDR device");
        }
    }

    if (a->state == ST_DECODE) {
        if (a->dsdr && a->proc.pid > 0) decode_poll(a, now);
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
    } else if (a->state == ST_SCAN) {
        int rows = 0;
        while ((r = lr_readline(&a->proc.lr, line, sizeof line)) == 1) rows += spec_add_line(&a->spec, line) > 0;
        if (rows) scan_refresh(a);
        if (r < 0) {
            proc_stop(&a->proc);
            a->state = ST_IDLE;
            set_status(a, a->npeaks ? "scan stopped" : "scan stopped: no signal");
        } else if (rows && a->cfg.auto_tune && a->npeaks && a->spec.n > 0 && a->spec.ok[a->spec.n - 1]) {
            app_tune(a, a->peaks[0].f_hz); /* first complete sweep reached the top of the band */
        }
    }

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
    tf_poll(&a->tf, &a->cfg, now);
}

void app_free(app_t *a)
{
    sweep_stop(a);
    decode_stop(a);
    fft_free(&a->sw.fft);
    proc_stop(&a->proc);
    gps_close(&a->gps);
    audio_close(&a->audio);
    tf_close(&a->tf);
    if (a->log) fclose(a->log);
    a->log = NULL;
}
