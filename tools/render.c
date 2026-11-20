/* Headless screenshot tool: runs the real UI code on a simulated flight and writes one
 * 320x170 PPM per screen.  Usage: sc_render <out_dir> <font_dir> <tile_dir> */
#define _GNU_SOURCE
#include "lvgl.h"
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 320
#define H 170
static uint8_t fb[W * H * 2];
static app_t app;
static lv_display_t *disp;
static const char *out_dir;

static uint32_t tick_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void shot(const char *name)
{
    char p[512];
    FILE *f;
    int i;
    app_poll(&app);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    for (i = 0; app.cfg.tile_online && i < 40; i++) { /* two downloads at a time: let all be listed */
        usleep(20000);
        app_poll(&app);
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(disp);
    }
    snprintf(p, sizeof p, "%s/%s.ppm", out_dir, name);
    f = fopen(p, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = 0; i < W * H; i++) {
        uint16_t v = (uint16_t)(fb[2 * i] | (fb[2 * i + 1] << 8));
        uint8_t rgb[3] = {(uint8_t)(((v >> 11) & 31) * 255 / 31), (uint8_t)(((v >> 5) & 63) * 255 / 63), (uint8_t)((v & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("%s\n", p);
}

/* The Radio screen's spectrum around f_hz, made up (no dongle here): noise, the sonde at f_hz and a weaker
 * carrier 215 kHz above. */
static void mock_live(long f_hz)
{
    long centre = f_hz - RS41_TUNE_OFFSET_HZ + DDC_RATE / 4;
    unsigned r = 777u;
    int i;
    spec_reset(&app.live, centre - DDC_RATE / 2, centre + DDC_RATE / 2, DDC_RATE / SWEEP_FFT);
    for (i = 0; i < app.live.n; i++) {
        double f = (double)app.live.f0 + (double)i * (double)app.live.step, db;
        r = r * 1103515245u + 12345u;
        db = -50.0 + ((r >> 16) & 0xff) / 255.0 * 4.0;
        db += 30.0 * exp(-pow((f - f_hz) / 4500.0, 2)) + 9.0 * exp(-pow((f - f_hz - 215000.0) / 3000.0, 2));
        app.live.db[i] = (float)db;
        app.live.ok[i] = 1;
    }
}

/* Place this device d metres from the sonde so that the bearing to the sonde is brg. */
static void place_me(double d, double brg)
{
    geo_dest(app.sonde.lat, app.sonde.lon, brg + 180.0, d, &app.demo.own_lat, &app.demo.own_lon);
}

int main(int argc, char **argv)
{
    double now;
    if (argc < 4) {
        fprintf(stderr, "usage: %s <out_dir> <font_dir> <tile_dir> [download_cache_dir]\n", argv[0]);
        return 2;
    }
    out_dir = argv[1];
    app_init(&app, NULL);
    app.cfg.demo = 1;
    app.cfg.demo_speed = 0; /* the tool advances the flight itself */
    app.cfg.audio_enable = 0;
    app.cfg.sdr_check = 0;
    app.sdr_ok = 1;
    snprintf(app.cfg.font_dir, sizeof app.cfg.font_dir, "%s", argv[2]);
    snprintf(app.cfg.tile_dir, sizeof app.cfg.tile_dir, "%s", argv[3]);
    app.cfg.tile_online = 0; /* the tool never touches the network */
    if (getenv("SC_RENDER_MISSING") && *getenv("SC_RENDER_MISSING")) { /* list the tiles the shots need, to fetch them separately */
        app.cfg.tile_online = 1;
        snprintf(app.cfg.tile_fetch_cmd, sizeof app.cfg.tile_fetch_cmd, "echo '{url}' >> '%s'; exit 1", getenv("SC_RENDER_MISSING"));
    }
    if (argc > 4) snprintf(app.cfg.tile_cache_dir, sizeof app.cfg.tile_cache_dir, "%s", argv[4]);
    tf_init(&app.tf, &app.cfg);
    app.cfg.zoom_min = 9;

    lv_init();
    lv_tick_set_cb(tick_ms);
    disp = lv_display_create(W, H);
    lv_display_set_buffers(disp, fb, NULL, sizeof fb, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);
    ui_init(&app);
    now = sc_now();

    app.sdr_ok = 0; /* error screen; app_poll in demo mode sets sdr_ok again, so draw it directly */
    lv_obj_invalidate(lv_screen_active());
    { char p[512]; FILE *f; int i; lv_refr_now(disp); snprintf(p, sizeof p, "%s/00_nosdr.ppm", out_dir); f = fopen(p, "wb");
      if (f) { fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (i = 0; i < W * H; i++) { uint16_t v = (uint16_t)(fb[2 * i] | (fb[2 * i + 1] << 8));
          uint8_t rgb[3] = {(uint8_t)(((v >> 11) & 31) * 255 / 31), (uint8_t)(((v >> 5) & 63) * 255 / 63), (uint8_t)((v & 31) * 255 / 31)}; fwrite(rgb, 1, 3, f); }
        fclose(f); printf("%s\n", p); } }
    app.sdr_ok = 1;

    ui_key('s'); /* scan */
    app.t_restart = 0;
    shot("04_scan");
    ui_key(K_LEFT);
    shot("04_scan_left");
    ui_key('f'); ui_key('4'); ui_key('0'); ui_key('1'); ui_key('8');
    shot("06_freq");
    ui_key(K_ESC);
    ui_key(K_RIGHT);
    ui_key(K_ENTER); /* tune the cursor (403.000) -> decode */
    ui_key('4');
    shot("01_track_waiting");

    while (!app.burst_alert && app.demo.tv < 9000) demo_advance(&app, 5, now);
    demo_advance(&app, 20, now);
    shot("07_burst");
    ui_key(K_ESC);

    while (app.demo.alt > 2976) demo_advance(&app, 1, now);
    place_me(10380.0, 90.0);
    ui_key('4'); shot("01_track");
    ui_key('5'); shot("02_map");
    ui_key('+'); ui_key('+'); ui_key('+'); shot("02_map_zoom"); /* two-digit zoom, chip layout */
    ui_key(K_RIGHT); shot("02_map_pan");
    ui_key('c');
    ui_key('6'); shot("03_graph_skewt");
    ui_key(K_RIGHT); shot("03_graph_flight");
    ui_key(K_LEFT);
    ui_key('8'); shot("05_info"); ui_key(K_RIGHT); shot("05_info_pos"); ui_key(K_RIGHT);
    { /* compass calibration: part of the turn, then the north step (fed by hand: the render has no sensor) */
        double h;
        app.hdg.mag_found = 1;
        magcal_start(&app.cal, 300);
        for (h = 0; h < 230; h += 5) magcal_add(&app.cal, 300 * cos(h * M_PI / 180), 300 * sin(h * M_PI / 180));
        lv_obj_invalidate(lv_screen_active());
        shot("08_compass_turn");
        for (; h < 380; h += 5) magcal_add(&app.cal, 300 * cos(h * M_PI / 180), 300 * sin(h * M_PI / 180));
        lv_obj_invalidate(lv_screen_active());
        shot("08_compass_north");
        app.cal.step = MAGCAL_OFF;
        app.hdg.mag_found = 0;
    }

    /* Radio screen tuned to the sonde. demo mode never reads from dsdr; the UI only needs it set. */
    mock_live(app.freq_hz);
    app.dsdr = (sdr_t *)&app;
    app.state = ST_LISTEN;
    ui_key('7'); shot("04_radio_listen");
    app.state = ST_DECODE;
    shot("04_radio_decode");
    app.dsdr = NULL;
    place_me(2400.0, 60.0); /* inside the beeper radius, sonde ahead-right */
    ui_key('4'); shot("01_track_near");

    /* no GPS: decoding continues */
    app_poll(&app);
    {
        double keep = app.gps.t_fix;
        lv_obj_invalidate(lv_screen_active());
        app.cfg.demo = 0; app.cfg.gpsd_port = 0; app.cfg.mag_enable = 0; /* stop demo_poll refreshing the fix */
        app.gps.t_fix = sc_now() - 35.0;
        app.gps.n_used = 0; app.gps.n_sat = 3; app.gps.t_sky = sc_now(); /* receiver still talking, no fix */
        shot("01_track_nogps");
        ui_key('5'); shot("02_map_nogps");
        app.cfg.demo = 1;
        (void)keep;
    }

    /* landed, signal lost 42 s ago, walking with the magnetometer heading */
    while (app.demo.phase != 2) demo_advance(&app, 1, now);
    demo_advance(&app, 200, now);
    place_me(412.0, 90.0);
    app.sonde.t_rx = sc_now() - 42.0;
    app_poll(&app);
    app.cfg.demo = 0;
    app.gps.t_fix = sc_now(); app.gps.speed = 1.1; app.gps.has_track = 0;
    geo_dest(app.sonde.lat, app.sonde.lon, 270.0, 412.0, &app.gps.lat, &app.gps.lon);
    app.hdg.deg = 211.0; app.hdg.src = HDG_MAG;
    ui_key('4'); shot("01_track_lost");
    ui_key('5'); shot("02_map_lost");
    ui_key('8'); shot("05_info_lost");
    return 0;
}
