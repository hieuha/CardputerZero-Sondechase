#define _GNU_SOURCE
#include "sc.h"
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
    hist_reset(&a->hist);
    flight_reset(&a->flight);
    audio_init(&a->audio);
    tf_init(&a->tf, &a->cfg);
    a->freq_hz = a->cfg.freq_hz;
    a->sdr_ok = (a->cfg.sdr_check && !a->cfg.demo) ? sdr_present("/sys/bus/usb/devices") : 1;
    set_status(a, "idle");
    if (a->freq_hz > 0) app_tune(a, a->freq_hz);
}

static void start_decoder(app_t *a)
{
    char cmd[1024];
    if (a->cfg.demo) { /* frames come from demo_poll */
        set_status(a, "decoding (demo)");
        return;
    }
    if (!a->sdr_ok) { /* retried from app_poll once the receiver is back */
        a->t_restart = sc_now() + 1.0;
        return;
    }
    if (sc_subst(cmd, sizeof cmd, a->cfg.decoder_cmd, &a->cfg, a->freq_hz) < 0 ||
        proc_start(&a->proc, cmd) < 0) {
        set_status(a, "decoder start failed");
        a->t_restart = sc_now() + 3.0;
        return;
    }
    set_status(a, "decoding");
}

void app_stop(app_t *a)
{
    proc_stop(&a->proc);
    a->state = ST_IDLE;
    set_status(a, "idle");
}

void app_tune(app_t *a, long f_hz)
{
    proc_stop(&a->proc);
    a->freq_hz = f_hz;
    a->state = ST_DECODE;
    start_decoder(a);
}

void app_scan(app_t *a)
{
    char cmd[640];
    if (!a->sdr_ok) {
        set_status(a, "no SDR device");
        return;
    }
    proc_stop(&a->proc); /* rtl_fm and rtl_power cannot share one dongle */
    spec_reset(&a->spec, a->cfg.scan_lo_hz, a->cfg.scan_hi_hz, a->cfg.scan_step_hz);
    a->npeaks = 0;
    a->sel = 0;
    if (a->cfg.demo) { /* demo_poll fills the spectrum after a simulated sweep time */
        a->state = ST_SCAN;
        a->t_restart = sc_now() + 1.5;
        set_status(a, "scanning");
        return;
    }
    if (sc_subst(cmd, sizeof cmd, a->cfg.scan_cmd, &a->cfg, 0) < 0 || proc_start(&a->proc, cmd) < 0) {
        a->state = ST_IDLE;
        set_status(a, "scan start failed");
        return;
    }
    a->state = ST_SCAN;
    set_status(a, "scanning");
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
    if (!sonde_parse(line, &s)) return;
    s.t_rx = now;
    if (!a->have_sonde || strcmp(s.id, a->sonde.id)) { /* new sonde: discard old profile */
        memset(&a->wind, 0, sizeof a->wind);
        hist_reset(&a->hist);
        flight_reset(&a->flight);
        a->burst_alert = 0;
        a->frames = 0;
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
            proc_stop(&a->proc);
            a->state = ST_IDLE;
            set_status(a, "no SDR device");
        }
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
    } else if (a->state == ST_SCAN) {
        while ((r = lr_readline(&a->proc.lr, line, sizeof line)) == 1) spec_add_line(&a->spec, line);
        if (r < 0) {
            proc_stop(&a->proc);
            a->npeaks = spec_peaks(&a->spec, a->cfg.scan_snr_db, a->peaks, MAX_PEAKS);
            a->state = ST_IDLE;
            set_status(a, a->npeaks ? "scan done" : "scan done: no signal");
            if (a->npeaks && a->cfg.auto_tune) app_tune(a, a->peaks[0].f_hz);
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
    proc_stop(&a->proc);
    gps_close(&a->gps);
    audio_close(&a->audio);
    tf_close(&a->tf);
    if (a->log) fclose(a->log);
    a->log = NULL;
}
