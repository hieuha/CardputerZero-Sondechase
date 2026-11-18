#define _GNU_SOURCE
#include "../src/sc.h"
#include <math.h>
#include <signal.h>
#include <sys/prctl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails = 0, checks = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            fails++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)
#define NEAR(a, b, tol) CHECK(fabs((a) - (b)) <= (tol))

static void t_geo(void)
{
    double la, lo;
    NEAR(geo_dist(0, 0, 1, 0), 111195.1, 1.0); /* 1 deg of latitude */
    NEAR(geo_bearing(0, 0, 0, 1), 90.0, 1e-6);
    NEAR(geo_bearing(10, 20, 9, 20), 180.0, 1e-6);
    geo_dest(21.0, 105.8, 37.0, 25000.0, &la, &lo);
    NEAR(geo_dist(21.0, 105.8, la, lo), 25000.0, 0.5);
    NEAR(geo_bearing(21.0, 105.8, la, lo), 37.0, 0.01);
    NEAR(geo_elev(0.0, 0, 1000), 90.0, 1e-6);
    NEAR(geo_elev(10000.0, 0, 10000), 44.96, 0.05);
    CHECK(geo_elev(200000.0, 0, 1000) < 0); /* below the geometric horizon */
    NEAR(norm180(350.0), -10.0, 1e-9);
    NEAR(norm360(-10.0), 350.0, 1e-9);
}

static void t_isa(void)
{
    NEAR(isa_rho(0), 1.2250, 0.001);
    NEAR(isa_rho(11000), 0.3639, 0.001);
    NEAR(isa_rho(20000), 0.0880, 0.0005);
    NEAR(isa_rho(30000), 0.0180, 0.0003); /* geopotential altitude */
}

static void t_predict(void)
{
    wind_t w;
    sonde_t s;
    pred_t p;
    memset(&w, 0, sizeof w);
    memset(&s, 0, sizeof s);
    s.lat = 21.0; s.lon = 105.8; s.alt = 1000; s.vel_h = 10; s.heading = 90; s.vel_v = -6;
    p = predict_landing(&s, s.vel_v, &w, 0, 30000, 6);
    CHECK(p.valid);
    NEAR(p.t_s, 170.6, 3.0); /* 1000 m at ~5.86 m/s mean */
    NEAR(geo_dist(s.lat, s.lon, p.lat, p.lon), 10 * p.t_s, 2.0);
    NEAR(geo_bearing(s.lat, s.lon, p.lat, p.lon), 90.0, 0.1);

    /* Wind profile from ascent overrides the current velocity layer by layer. */
    wind_add(&w, 100, 5, 0);  /* 0-250 m: 5 m/s to the north */
    wind_add(&w, 900, 5, 180); /* 750-1000 m: 5 m/s to the south */
    {
        double e, n;
        wind_get(&w, 120, 9, 9, &e, &n);
        NEAR(n, 5.0, 1e-4);
        wind_get(&w, 990, 9, 9, &e, &n);
        NEAR(n, -5.0, 1e-4);
    }
    /* Ascent: climbs to burst, then descends; flight time must exceed the climb time. */
    s.alt = 5000; s.vel_v = 5;
    p = predict_landing(&s, s.vel_v, &w, 0, 30000, 6);
    CHECK(p.valid && p.t_s > 25000.0 / 5.0 && p.t_s < 25000.0 / 5.0 + 3600.0);
    /* On the ground: no prediction. */
    s.alt = 10; s.vel_v = 0;
    p = predict_landing(&s, 0, &w, 0, 30000, 6);
    CHECK(!p.valid);
}

static void t_parse(void)
{
    const char *l = "{\"type\":\"RS41\",\"frame\":5391,\"id\":\"S4430329\",\"datetime\":"
                    "\"2026-11-18T00:31:05.000Z\",\"lat\":21.02345,\"lon\":105.80123,\"alt\":15234.5,"
                    "\"vel_h\":12.3,\"heading\":95.2,\"vel_v\":5.1,\"sats\":9,\"bt\":65535,"
                    "\"batt\":2.9,\"temp\":-55.2,\"humidity\":12.0,\"subtype\":\"RS41-SGP\"}";
    sonde_t s;
    gps_t g;
    char buf[32];
    double v;
    CHECK(sonde_parse(l, &s) == 1);
    CHECK(!strcmp(s.id, "S4430329"));
    CHECK(s.frame == 5391 && s.sats == 9 && s.bt == 65535);
    NEAR(s.lat, 21.02345, 1e-9);
    NEAR(s.alt, 15234.5, 1e-9);
    NEAR(s.vel_v, 5.1, 1e-9);
    NEAR(s.temp, -55.2, 1e-9);
    CHECK(sonde_parse("{\"id\":\"X\",\"lat\":0.0,\"lon\":0.0,\"alt\":0.0}", &s) == 0);
    CHECK(sonde_parse("{\"id\":\"X\",\"frame\":1}", &s) == 0);
    /* Real RS41 frame before its GPS locks (Y0532351 on the bench): telemetry only. */
    CHECK(sonde_parse("{ \"type\": \"RS41\", \"frame\": 471, \"id\": \"Y0532351\", \"datetime\": "
                      "\"1980-01-06T00:00:00.000Z\", \"lat\": 7.87821, \"lon\": 112.81147, \"alt\": 398.46852, "
                      "\"vel_h\": 0.00000, \"heading\": -0.00000, \"vel_v\": 0.00000, \"sats\": 0, "
                      "\"bt\": 65535, \"batt\": 3.30, \"subtype\": \"RS41\" }", &s) == 2);
    CHECK(!strcmp(s.id, "Y0532351") && s.frame == 471 && s.sats == 0);
    NEAR(s.batt, 3.30, 1e-9);
    CHECK(js_num(l, "alt", &v) && v == 15234.5);          /* "alt" must not match inside other keys */
    CHECK(!js_num(l, "al", &v));
    CHECK(js_str(l, "subtype", buf, sizeof buf) && !strcmp(buf, "RS41-SGP"));

    gps_init(&g);
    CHECK(gps_parse(&g, "{\"class\":\"SKY\",\"nSat\":12}", 1.0) == 0);
    CHECK(gps_parse(&g, "{\"class\":\"TPV\",\"device\":\"/dev/ttyACM0\",\"mode\":3,\"lat\":21.0285,"
                        "\"lon\":105.8542,\"altHAE\":-10.1,\"altMSL\":18.2,\"speed\":13.9,\"track\":271.5}", 2.0) == 1);
    CHECK(g.mode == 3 && gps_ok(&g, 3.0) && !gps_ok(&g, 10.0));
    NEAR(g.alt, 18.2, 1e-9);
    NEAR(g.track, 271.5, 1e-9);
    CHECK(gps_parse(&g, "{\"class\":\"TPV\",\"mode\":1}", 3.0) == 1 && !gps_ok(&g, 3.0));
}

static void t_heading(void)
{
    cfg_t c;
    gps_t g;
    heading_t h;
    cfg_defaults(&c);
    c.mag_enable = 0;
    heading_init(&h, &c);
    gps_init(&g);
    heading_update(&h, &g, &c, 1.0);
    CHECK(h.src == HDG_NONE);
    g.mode = 3; g.t_fix = 1.0; g.speed = 10; g.track = 123; g.has_track = 1;
    heading_update(&h, &g, &c, 1.5);
    CHECK(h.src == HDG_GPS);
    NEAR(h.deg, 123.0, 1e-9);
    g.speed = 0.2; /* stationary: course is noise, keep the last value */
    heading_update(&h, &g, &c, 2.0);
    CHECK(h.src == HDG_HOLD);
    NEAR(h.deg, 123.0, 1e-9);
    c.mag_off_x = 10; c.mag_off_y = -5; c.mag_rot_deg = 0;
    NEAR(heading_from_mag(10 + 1, -5 + 0, &c), 0.0, 1e-9);
    NEAR(heading_from_mag(10 + 0, -5 + 1, &c), 90.0, 1e-9);
    c.mag_flip_y = 1; c.declination = -1.5;
    NEAR(heading_from_mag(10 + 0, -5 + 1, &c), 268.5, 1e-9);
}

static void t_subst(void)
{
    cfg_t c;
    char out[1024];
    cfg_defaults(&c);
    CHECK(sc_subst(out, sizeof out, "a {f_hz} {nope}", 403500000) == 0);
    CHECK(!strcmp(out, "a 403500000 {nope}"));
    CHECK(sc_subst(out, sizeof out, c.decoder_feed_cmd, 403500000) == 0);
    CHECK(strstr(out, "fsk_demod --cs16") && strstr(out, "--jsn_cfq 403500000"));
    CHECK(sc_subst(out, 8, "0123456789", 0) == -1);
}

static void t_cfg(void)
{
    cfg_t c;
    FILE *f = fopen("/tmp/sc_test.conf", "w");
    fprintf(f, "# comment\nfreq_hz = 403500000\n gain=38.6 \nground_alt = 12.5\nlog_dir = /tmp\nbogus = 1\n");
    fclose(f);
    cfg_defaults(&c);
    CHECK(cfg_load(&c, "/tmp/sc_test.conf") == 0);
    CHECK(c.freq_hz == 403500000 && !strcmp(c.gain, "38.6") && c.ground_alt == 12.5);
    CHECK(c.gpsd_port == 2947);
    CHECK(cfg_load(&c, "/nonexistent/x.conf") == -1);
}

static void t_spec(void)
{
    static spectrum_t s;
    static lr_t lr;
    static char line[LR_MAX], got[LR_MAX];
    float bins[410];
    peak_t pk[MAX_PEAKS];
    int hop, i, n, fd[2];
    spec_reset(&s, 400000000, 406000000, 5000);
    CHECK(s.n == 1200);
    /* Three 2 MHz steps of 4882.8125 Hz bins (not the 5 kHz grid), noise -42 dB with ripple,
     * carriers at 403.500 MHz (+20 dB) and 401.100 MHz (+11 dB). */
    for (hop = 0; hop < 3; hop++) {
        double lo = 400e6 + hop * 2e6, st = 4882.8125;
        for (i = 0; i < 410; i++) {
            double f = lo + i * st;
            bins[i] = (float)(-42.0 + 0.8 * sin(i * 0.7));
            if (fabs(f - 403.5e6) < 6000) bins[i] = -22.0f;
            if (fabs(f - 401.1e6) < 6000) bins[i] = -31.0f;
        }
        CHECK(spec_add_bins(&s, lo, st, bins, 410, 0.0) > 300);
    }
    n = spec_peaks(&s, 8.0, pk, MAX_PEAKS);
    CHECK(n == 2);
    CHECK(pk[0].f_hz == 403500000 && pk[1].f_hz == 401100000);
    NEAR(pk[0].snr_db, 20.0, 1.5);
    NEAR(s.floor_db, -42.0, 1.0);

    /* a line of a few kB (a gpsd SKY report listing every satellite) passes the line reader intact */
    CHECK(pipe(fd) == 0);
    lr.fd = fd[0];
    lr.len = 0;
    memset(line, 'x', 6000);
    line[6000] = '\n';
    CHECK(write(fd[1], line, 6001) == 6001);
    CHECK(lr_readline(&lr, got, sizeof got) == 1 && strlen(got) == 6000);
    close(fd[0]);
    close(fd[1]);

    { /* in-process sweep steps: 2 kHz FFT bins onto the 5 kHz grid, peak hold with decay */
        static float bins[800];
        spec_reset(&s, 400000000, 406000000, 5000);
        for (hop = 0; hop < 4; hop++) {
            for (i = 0; i < 800; i++) bins[i] = -60.0f;
            if (hop == 1) bins[300] = -20.0f; /* 401.6 + 300 * 2 kHz = 402.200 MHz */
            /* bins lo .. lo + 1598 kHz round onto cells 0 .. 320 (321 cells); the last step runs past
             * 406 MHz and keeps the 240 cells up to the end of the band */
            CHECK(spec_add_bins(&s, 400.0e6 + hop * 1.6e6, 2000.0, bins, 800, 3.0) == (hop < 3 ? 321 : 240));
        }
        n = spec_peaks(&s, 8.0, pk, MAX_PEAKS);
        CHECK(n == 1 && pk[0].f_hz == 402200000);
        NEAR(s.db[(402200000 - 400000000) / 5000], -20.0, 1e-6);
        for (i = 0; i < 800; i++) bins[i] = -60.0f; /* the carrier pauses: held, decaying 3 dB per pass */
        spec_add_bins(&s, 401.6e6, 2000.0, bins, 800, 3.0);
        NEAR(s.db[(402200000 - 400000000) / 5000], -23.0, 1e-6);
        CHECK(spec_peaks(&s, 8.0, pk, MAX_PEAKS) == 1 && pk[0].f_hz == 402200000);
        for (hop = 0; hop < 20; hop++) spec_add_bins(&s, 401.6e6, 2000.0, bins, 800, 3.0); /* it stopped */
        NEAR(s.db[(402200000 - 400000000) / 5000], -60.0, 1e-6);
        CHECK(spec_peaks(&s, 8.0, pk, MAX_PEAKS) == 0);
    }
}

static void t_ring(void)
{
    iqring_t r;
    unsigned char in[64], out[64];
    int i;
    for (i = 0; i < 64; i++) in[i] = (unsigned char)i;
    CHECK(iqring_init(&r, 48) == -1); /* not a power of two */
    CHECK(iqring_init(&r, 32) == 0);
    CHECK(iqring_write(&r, in, 20) == 20 && iqring_avail(&r) == 20);
    CHECK(iqring_read(&r, out, 8) == 8 && !memcmp(out, in, 8));
    /* 12 buffered + 20 more wraps the end of the buffer and fills it exactly */
    CHECK(iqring_write(&r, in + 20, 20) == 20 && iqring_avail(&r) == 32 && r.dropped == 0);
    CHECK(iqring_read(&r, out, 64) == 32 && !memcmp(out, in + 8, 32));
    /* full ring: new data is dropped and counted, never overwriting unread bytes */
    CHECK(iqring_write(&r, in, 30) == 30);
    CHECK(iqring_write(&r, in, 7) == 2 && r.dropped == 5); /* whole IQ pairs only */
    CHECK(iqring_read(&r, out, 4) == 4 && !memcmp(out, in, 4));
    iqring_flush(&r);
    CHECK(iqring_avail(&r) == 0 && iqring_read(&r, out, 8) == 0);
    CHECK(iqring_write(&r, in, 5) == 4); /* odd byte of a split pair is not stored */
    iqring_free(&r);
}

/* u8 IQ (rtl-sdr format) of a complex tone at f_hz, sampled at rate. */
static void tone_u8(unsigned char *iq, int pairs, double f_hz, double rate, double amp)
{
    int i;
    for (i = 0; i < pairs; i++) {
        double ph = 2.0 * M_PI * f_hz * i / rate;
        iq[2 * i] = (unsigned char)lround(127.5 + amp * cos(ph));
        iq[2 * i + 1] = (unsigned char)lround(127.5 + amp * sin(ph));
    }
}

/* Power of complex int16 IQ at f_hz (single-bin DFT), skipping the filter start-up. */
static double cs16_power(const int16_t *x, int pairs, double f_hz, double rate)
{
    double re = 0, im = 0;
    int i;
    for (i = 64; i < pairs; i++) {
        double ph = -2.0 * M_PI * f_hz * i / rate;
        re += x[2 * i] * cos(ph) - x[2 * i + 1] * sin(ph);
        im += x[2 * i] * sin(ph) + x[2 * i + 1] * cos(ph);
    }
    return re * re + im * im;
}

static void t_dsp(void)
{
    enum { N = 1024, PAIRS = 8 * DDC_BLOCK };
    static unsigned char iq[2 * PAIRS];
    static int16_t a[2 * (PAIRS / DDC_DECIM + DDC_BLOCK / DDC_DECIM)], b[sizeof a / sizeof a[0]];
    static ddc_t d1, d2;
    static float db[N];
    fft_t f;
    size_t na, nb, off;
    int i, best = 0;
    double p12;

    /* FFT: a tone exactly on bin +100 peaks there (DC sits at N/2), ~0 dBFS at full scale */
    CHECK(fft_init(&f, 1000) == -1);
    CHECK(fft_init(&f, N) == 0);
    tone_u8(iq, 4 * N, 100.0 * DDC_RATE / N, DDC_RATE, 120.0);
    fft_spectrum(&f, iq, 4, db);
    for (i = 1; i < N; i++)
        if (db[i] > db[best]) best = i;
    CHECK(best == N / 2 + 100);
    NEAR(db[best], 20.0 * log10(120.0 / 127.5), 0.5);
    CHECK(db[N / 2 + 300] < db[best] - 40.0);
    fft_free(&f);

    /* DDC: the sonde sits DDC_RATE/4 - 12 kHz below the dongle's centre (rtl_fm's offset tuning);
     * it must come out at +12 kHz in the 48 kHz stream, where fsk_demod expects it */
    tone_u8(iq, PAIRS, -(DDC_RATE / 4.0 - 12000.0), DDC_RATE, 60.0);
    ddc_init(&d1);
    na = ddc_process(&d1, iq, sizeof iq, a);
    CHECK(na == PAIRS / DDC_DECIM);
    p12 = cs16_power(a, (int)na, 12000.0, 48000.0);
    CHECK(p12 > 100.0 * cs16_power(a, (int)na, -12000.0, 48000.0));
    CHECK(p12 > 100.0 * cs16_power(a, (int)na, 20000.0, 48000.0));
    /* the same input fed in small odd-sized pieces gives a bit-identical stream */
    ddc_init(&d2);
    for (off = 0, nb = 0; off < sizeof iq; off += 38)
        nb += ddc_process(&d2, iq + off, off + 38 <= sizeof iq ? 38 : sizeof iq - off, b + 2 * nb);
    CHECK(nb == na && !memcmp(a, b, na * 2 * sizeof a[0]));
}

/* Number of non-zombie processes in process group pg (zombies await reaping by init). */
static int live_in_group(pid_t pg)
{
    char path[64], comm[64], st;
    int pid, ppid, pgrp, n = 0, i;
    for (i = 2; i < 4194304 && i < (int)pg + 64; i++) {
        FILE *f;
        if (i < (int)pg) i = (int)pg;
        snprintf(path, sizeof path, "/proc/%d/stat", i);
        f = fopen(path, "r");
        if (!f) continue;
        if (fscanf(f, "%d %63s %c %d %d", &pid, comm, &st, &ppid, &pgrp) == 5 && pgrp == (int)pg && st != 'Z') n++;
        fclose(f);
    }
    return n;
}

/* Frames from a sonde without a GPS fix are counted and kept apart; the count carries on once it locks. */
static void t_frames(void)
{
    static app_t a;
    const char *nofix = "{\"id\":\"Y0532351\",\"frame\":10,\"lat\":7.8,\"lon\":112.8,\"alt\":398,\"sats\":0,\"batt\":3.0}";
    const char *fix = "{\"id\":\"Y0532351\",\"frame\":12,\"lat\":21.03,\"lon\":105.85,\"alt\":40,\"sats\":9,\"batt\":3.0}";
    app_init(&a, NULL);
    snprintf(a.cfg.log_dir, sizeof a.cfg.log_dir, "/tmp");
    a.freq_hz = 403000000L;
    CHECK(app_known(&a, 403000000L) == NULL);
    app_on_frame(&a, nofix, 1.0);
    app_on_frame(&a, nofix, 2.0);
    CHECK(!a.have_sonde && a.frames == 2 && a.nofix.frame == 10 && a.nofix.t_rx == 2.0);
    /* any decoded frame marks its frequency as an RS41 with that serial, within +-10 kHz */
    CHECK(app_known(&a, 403010000L) && !strcmp(app_known(&a, 403010000L)->id, "Y0532351"));
    CHECK(app_known(&a, 403020000L) == NULL);
    app_on_frame(&a, fix, 3.0);
    CHECK(a.have_sonde && a.frames == 3 && a.sonde.frame == 12);
    app_on_frame(&a, "{\"id\":\"S1111111\",\"frame\":1,\"lat\":0.5,\"lon\":100,\"alt\":5,\"sats\":0}", 4.0);
    CHECK(a.frames == 1 && !strcmp(a.nofix.id, "S1111111")); /* a different sonde starts over */
    app_free(&a);
    unlink("/tmp/sondechase.csv");
}

/* Number of processes (zombies included) in process group pg. */
static int procs_in_group(pid_t pg)
{
    char path[64], comm[64], st;
    int pid, ppid, pgrp, n = 0, i;
    for (i = (int)pg; i < (int)pg + 64; i++) {
        FILE *f;
        snprintf(path, sizeof path, "/proc/%d/stat", i);
        if (!(f = fopen(path, "r"))) continue;
        if (fscanf(f, "%d %63s %c %d %d", &pid, comm, &st, &ppid, &pgrp) == 5 && pgrp == (int)pg) n++;
        fclose(f);
    }
    return n;
}

/* stdin-fed children get every byte; stopping a pipeline leaves no zombies behind (as a subreaper,
 * like the app, the members the shell leaves behind are ours to collect). */
static void t_feed(void)
{
    static unsigned char buf[3 * 4096 + 100];
    proc_t p;
    pid_t pg;
    FILE *f;
    long size = -1;
    size_t i;
    signal(SIGPIPE, SIG_IGN);
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    for (i = 0; i < sizeof buf; i++) buf[i] = (unsigned char)i;
    proc_init(&p);
    unlink("/tmp/sc_feed_test");
    CHECK(proc_start_feed(&p, "cat > /tmp/sc_feed_test") == 0 && p.wfd >= 0);
    proc_feed(&p, buf, sizeof buf);
    CHECK(p.wdrop == 0);
    usleep(200000);
    proc_stop(&p);
    if ((f = fopen("/tmp/sc_feed_test", "rb"))) {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
    }
    CHECK(size == (long)sizeof buf);
    unlink("/tmp/sc_feed_test");

    CHECK(proc_start(&p, "sleep 30 | sleep 30 | cat") == 0);
    pg = p.pid;
    usleep(200000);
    CHECK(procs_in_group(pg) >= 3);
    proc_stop(&p);
    CHECK(procs_in_group(pg) == 0); /* not even zombies */
    prctl(PR_SET_CHILD_SUBREAPER, 0);
}

static void t_app(const char *fake, const char *fakelib)
{
    static app_t a;
    double t0;
    int i;
    app_init(&a, NULL);
    a.cfg.gpsd_port = 0; /* no gpsd in the test: inject a fix instead */
    a.cfg.mag_enable = 0;
    a.cfg.audio_enable = 0;
    a.cfg.sdr_check = 0; /* no USB receiver in the test environment */
    snprintf(a.cfg.rtlsdr_lib, sizeof a.cfg.rtlsdr_lib, "%s", fakelib); /* librtlsdr stand-in, 403.000 MHz carrier */
    a.sdr_ok = 1;
    a.cfg.ground_alt = 10;
    snprintf(a.cfg.log_dir, sizeof a.cfg.log_dir, "/tmp");
    snprintf(a.cfg.decoder_feed_cmd, sizeof a.cfg.decoder_feed_cmd, "sh %s {f_hz}", fake);
    unlink("/tmp/sondechase.csv");
    a.gps.mode = 3; a.gps.lat = 21.03; a.gps.lon = 105.85; a.gps.alt = 10;
    app_tune(&a, 403500000);
    CHECK(a.state == ST_DECODE && a.proc.pid > 0);
    for (i = 0; i < 100 && a.frames < 3; i++) {
        a.gps.t_fix = sc_now();
        app_poll(&a);
        usleep(20000);
    }
    CHECK(a.frames == 3);
    CHECK(a.have_sonde && !strcmp(a.sonde.id, "S4430329") && a.sonde.frame == 5392);
    CHECK(a.have_geo);
    NEAR(a.dist, 10378.0, 30.0); /* 0.1 deg of longitude at 21.03 N */
    NEAR(a.brg, 90.0, 0.1);
    NEAR(a.elev, 16.0, 0.5); /* ~2966 m above the observer at 10.4 km */
    CHECK(a.pred.valid && a.have_land);
    NEAR(a.pred.t_s, 266.0, 10.0);              /* 2966 m, 12 m/s at 3 km => ~11.2 m/s mean */
    NEAR(a.land_dist - a.dist, 8.0 * a.pred.t_s, 30.0); /* drifting further east at 8 m/s */
    {
        pid_t pg = a.proc.pid;
        app_stop(&a);
        CHECK(a.state == ST_IDLE && a.proc.pid == 0);
        CHECK(a.hist.n == 1 && a.flight.phase == PH_DESCENT && !a.burst_alert);
        CHECK(!a.prox.active); /* 10.38 km: outside the 10 km radius */
        int gone = 0, k;
        for (k = 0; k < 50 && !gone; k++) { /* children die asynchronously after SIGTERM */
            gone = live_in_group(pg) == 0;
            if (!gone) usleep(20000);
        }
        CHECK(gone); /* whole pipeline (incl. "sleep 30") is gone */
    }
    {
        FILE *f = fopen("/tmp/sondechase.csv", "r");
        char l[512];
        int rows = 0;
        CHECK(f != NULL);
        while (f && fgets(l, sizeof l, f)) rows++;
        if (f) fclose(f);
        CHECK(rows == 4); /* header + 3 frames */
    }
    /* Decoder that exits immediately: the app schedules a restart instead of going idle. */
    snprintf(a.cfg.decoder_feed_cmd, sizeof a.cfg.decoder_feed_cmd, "true");
    app_tune(&a, 403500000);
    for (i = 0; i < 50 && a.proc.pid > 0; i++) {
        app_poll(&a);
        usleep(10000);
    }
    CHECK(a.state == ST_DECODE && a.proc.pid == 0 && a.t_restart > sc_now());
    /* Receiver missing: tuning is accepted but nothing is spawned until it appears; scans are refused. */
    app_stop(&a);
    a.sdr_ok = 0;
    app_tune(&a, 403500000);
    CHECK(a.state == ST_DECODE && a.proc.pid == 0);
    app_scan(&a);
    CHECK(a.state == ST_DECODE && a.proc.pid == 0 && !strcmp(a.status, "no SDR device"));
    a.sdr_ok = 1;
    snprintf(a.cfg.decoder_feed_cmd, sizeof a.cfg.decoder_feed_cmd, "sleep 5");
    for (i = 0; i < 150 && a.proc.pid == 0; i++) {
        app_poll(&a);
        usleep(10000);
    }
    CHECK(a.proc.pid > 0); /* started by itself once the receiver is back */

    /* Manual scan through the fake dongle: ~1 s of passes finds its 403.000 MHz carrier, then holds. */
    app_stop(&a);
    app_scan(&a);
    CHECK(a.state == ST_SCAN && a.sw.sdr == a.rx && a.rx != NULL);
    for (i = 0; i < 500 && a.state == ST_SCAN; i++) {
        app_poll(&a);
        usleep(10000);
    }
    CHECK(a.state == ST_IDLE && !strcmp(a.status, "scan done") && a.sw.passes >= SCAN_PASSES);
    /* the fake's IQ has a DC offset (like a real tuner): no false peaks at the step centres */
    CHECK(a.npeaks == 1 && a.peaks[0].f_hz == 403000000 && a.sel == 0);
    /* Enter on that peak: the open dongle switches job and rate, it is not reopened */
    t0 = sc_now();
    app_tune(&a, a.peaks[0].f_hz);
    CHECK(a.dsdr == a.rx && a.proc.pid > 0 && sc_now() - t0 < 0.3);
    { /* tuning on the spectrum moves the dongle and the view, the decoder keeps running */
        pid_t pid = a.proc.pid;
        app_retune(&a, 403010000L);
        CHECK(a.proc.pid == pid && a.dsdr == a.rx && a.freq_hz == 403010000L);
        CHECK(a.live.f0 == 403010000L - RS41_TUNE_OFFSET_HZ + DDC_RATE / 4 - DDC_RATE / 2);
    }
    app_free(&a);

    /* librtlsdr0 missing: the reason is shown, nothing is spawned, scanning is refused the same way */
    app_init(&a, NULL);
    a.cfg.gpsd_port = 0;
    a.cfg.mag_enable = 0;
    a.cfg.audio_enable = 0;
    a.cfg.sdr_check = 0;
    a.sdr_ok = 1;
    snprintf(a.cfg.rtlsdr_lib, sizeof a.cfg.rtlsdr_lib, "/nonexistent/librtlsdr.so.0");
    app_tune(&a, 403000000);
    CHECK(a.proc.pid == 0 && a.rx == NULL && !strcmp(a.status, "librtlsdr0 not installed"));
    app_scan(&a);
    CHECK(a.state == ST_IDLE && !strcmp(a.status, "librtlsdr0 not installed"));
    app_free(&a);
}

static void t_track(void)
{
    static hist_t h;
    flight_t f;
    prox_t p;
    audio_t a;
    sonde_t s;
    short pcm[4000];
    double x, y, t = 100.0;
    int i, ev = 0, nz = 0;

    /* PTU extras */
    CHECK(sonde_parse("{\"id\":\"A\",\"lat\":21.0,\"lon\":105.8,\"alt\":2976.0,\"temp\":12.5,"
                      "\"humidity\":64.0,\"pressure\":706.2,\"subtype\":\"RS41-SGP\"}", &s));
    NEAR(s.humidity, 64.0, 1e-9);
    NEAR(s.pressure, 706.2, 1e-9);
    CHECK(!s.pres_est && !strcmp(s.subtype, "RS41-SGP"));
    CHECK(sonde_parse("{\"id\":\"A\",\"lat\":21.0,\"lon\":105.8,\"alt\":2976.0}", &s));
    CHECK(s.pres_est && s.humidity < 0);
    NEAR(s.pressure, 703.2, 0.5);
    NEAR(isa_p(0), 1013.25, 0.1);
    NEAR(isa_p(11000), 226.3, 0.3);
    NEAR(isa_p(16000), 102.9, 0.3);
    NEAR(dewpoint(12.5, 64), 5.87, 0.02);
    NEAR(dewpoint(20, 100), 20.0, 1e-6);

    /* burst: ascent at +5 m/s to 31 km, then three frames falling */
    flight_reset(&f);
    memset(&s, 0, sizeof s);
    for (i = 0; i < 20; i++) { s.alt = 30900 + i * 5; s.vel_v = 5; ev += flight_update(&f, &s, t++); }
    CHECK(ev == 0 && f.phase == PH_ASCENT);
    s.vel_v = -4; s.alt = 30990; ev += flight_update(&f, &s, t++);   /* one noisy frame: no alert */
    s.vel_v = 5;  s.alt = 31000; ev += flight_update(&f, &s, t++);
    CHECK(ev == 0);
    for (i = 0; i < 3; i++) { s.vel_v = -40; s.alt = 30960 - i * 40; ev += flight_update(&f, &s, t++); }
    CHECK(ev == 1 && f.burst && f.phase == PH_DESCENT);
    NEAR(f.burst_alt, 31000.0, 1e-9);
    for (i = 0; i < 10; i++) { s.vel_v = -30; ev += flight_update(&f, &s, t++); }
    CHECK(ev == 1); /* reported once */
    for (i = 0; i < 5; i++) { s.vel_v = 0.1; s.alt = 20; flight_update(&f, &s, t++); }
    CHECK(f.phase == PH_LANDED);
    /* receiver started during descent: phase set, no alert; low-level bounce is not a burst */
    flight_reset(&f); ev = 0;
    for (i = 0; i < 5; i++) { s.vel_v = -12; s.alt = 5000; ev += flight_update(&f, &s, t++); }
    CHECK(ev == 0 && f.phase == PH_DESCENT && !f.burst);
    flight_reset(&f); ev = 0;
    for (i = 0; i < 5; i++) { s.vel_v = 4; s.alt = 300; ev += flight_update(&f, &s, t++); }
    for (i = 0; i < 5; i++) { s.vel_v = -5; s.alt = 280; ev += flight_update(&f, &s, t++); }
    CHECK(ev == 0);

    /* proximity mapping */
    NEAR(prox_interval(10000), 2.0, 1e-9);
    NEAR(prox_interval(1000), 0.59, 0.01);
    NEAR(prox_interval(100), 0.24, 0.01);
    NEAR(prox_interval(0), 0.12, 1e-9);
    CHECK(prox_interval(500) < prox_interval(2000));
    NEAR(prox_volume(10000), 0.30, 1e-9);
    NEAR(prox_volume(1000), 0.65, 1e-9);
    NEAR(prox_volume(100), 1.0, 1e-9);
    NEAR(prox_volume(5), 1.0, 1e-9);
    memset(&p, 0, sizeof p);
    CHECK(prox_update(&p, 1, 12000, 0.0, 10000, 500) == 0 && !p.active);
    CHECK(prox_update(&p, 1, 9900, 1.0, 10000, 500) == 1 && p.active);   /* enters: beeps at once */
    CHECK(prox_update(&p, 1, 9900, 1.5, 10000, 500) == 0);               /* interval ~2 s not elapsed */
    CHECK(prox_update(&p, 1, 9900, 3.1, 10000, 500) == 1);
    CHECK(prox_update(&p, 1, 10300, 9.0, 10000, 500) == 1 && p.active);  /* inside hysteresis band */
    CHECK(prox_update(&p, 1, 10600, 20.0, 10000, 500) == 0 && !p.active);
    CHECK(prox_update(&p, 1, 800, 30.0, 10000, 500) == 1);
    p.muted = 1;
    CHECK(prox_update(&p, 1, 800, 40.0, 10000, 500) == 0 && p.active);
    CHECK(prox_update(&p, 0, 0, 50.0, 10000, 500) == 0 && !p.active);     /* no GPS fix: silent */

    /* audio: 60 ms tone then silence, bounded amplitude scaled by volume */
    audio_init(&a);
    CHECK(audio_render(&a, pcm, 100) == 0 && pcm[50] == 0);
    audio_tone(&a, 2200, 0.06, 0.5);
    CHECK(audio_render(&a, pcm, 4000) == 1);
    for (i = 0; i < 4000; i++) {
        if (pcm[i] != 0) nz = i;
        CHECK(pcm[i] <= 15000 && pcm[i] >= -15000);
    }
    CHECK(nz > 900 && nz < 960); /* 0.06 s * 16 kHz = 960 samples */
    CHECK(a.nseg == 0);

    /* history: 5 s spacing, decimation when full */
    hist_reset(&h);
    memset(&s, 0, sizeof s);
    CHECK(hist_add(&h, &s, 1000.0) == 1 && hist_add(&h, &s, 1002.0) == 0 && hist_add(&h, &s, 1005.0) == 1);
    for (i = 2; i < HIST_MAX + 10; i++) { s.alt = i; hist_add(&h, &s, 1000.0 + 5.0 * i); }
    CHECK(h.n > HIST_MAX / 2 && h.n < HIST_MAX && h.step_s == 10.0);
    CHECK(h.p[1].t_s == 10.0f && h.p[h.n - 1].alt >= HIST_MAX);

    /* SDR presence from a USB sysfs tree */
    if (system("rm -rf /tmp/sc_usb && mkdir -p /tmp/sc_usb/usb1 /tmp/sc_usb/1-1 /tmp/sc_usb/1-1:1.0 && "
               "echo 1d6b > /tmp/sc_usb/usb1/idVendor && echo 0002 > /tmp/sc_usb/usb1/idProduct && "
               "echo 1a40 > /tmp/sc_usb/1-1/idVendor && echo 0101 > /tmp/sc_usb/1-1/idProduct") == 0) {
        CHECK(sdr_present("/tmp/sc_usb") == 0);                 /* root hub and a USB hub only */
        CHECK(system("mkdir -p /tmp/sc_usb/1-1.2 && echo 0bda > /tmp/sc_usb/1-1.2/idVendor && "
                     "echo 2838 > /tmp/sc_usb/1-1.2/idProduct") == 0);
        CHECK(sdr_present("/tmp/sc_usb") == 1);                 /* RTL-SDR Blog v4 */
    }
    CHECK(sdr_present("/nonexistent/usb") == 0);

    /* tiles: z/x/y of a known point, scale, zoom fit */
    tile_frac(21.0285, 105.8542, 12, &x, &y);
    CHECK((int)x == 3252 && (int)y == 1803);
    tile_frac(0, 0, 1, &x, &y);
    NEAR(x, 1.0, 1e-9);
    NEAR(y, 1.0, 1e-9);
    NEAR(tile_mpp(21.03, 12), 35.67, 0.05);
    i = tile_fit_zoom(21.03, 105.85, 21.03, 105.95, 260, 100, 8, 14);
    CHECK(i == 11); /* 10.38 km: 146 px at z11 (71 m/px), 291 px at z12 */
    CHECK(tile_fit_zoom(21.0, 105.0, 23.0, 108.0, 260, 100, 8, 14) == 8);
}

static void t_tiles(void)
{
    static tilefetch_t t;
    cfg_t c;
    char p[320];
    unsigned char px[4 * 3] = {242, 239, 233, 255, 255, 255, 170, 211, 223, 40, 40, 40};
    int i, n = 0;
    double now = 100.0;

    cfg_defaults(&c);
    CHECK(system("rm -rf /tmp/sc_tc /tmp/sc_pre && mkdir -p /tmp/sc_pre/12/3252 && "
                 "head -c 300 /dev/zero > /tmp/sc_src.png && cp /tmp/sc_src.png /tmp/sc_pre/12/3252/1803.png") == 0);
    snprintf(c.tile_dir, sizeof c.tile_dir, "/tmp/sc_pre");
    snprintf(c.tile_cache_dir, sizeof c.tile_cache_dir, "/tmp/sc_tc");
    snprintf(c.tile_url, sizeof c.tile_url, "http://tiles.invalid/{z}/{x}/{y}.png");
    /* stand-in for curl: checks that url, user agent and output path arrive, then writes the file */
    {
        FILE *f = fopen("/tmp/sc_fetch.sh", "w");
        fprintf(f, "case \"$1\" in http://tiles.invalid/11/1626/901.png|http://tiles.invalid/11/1627/901.png) ;; *) exit 22;; esac\n"
                   "test -n \"$2\" && cp /tmp/sc_src.png \"$3\"\n");
        fclose(f);
    }
    snprintf(c.tile_fetch_cmd, sizeof c.tile_fetch_cmd, "sh /tmp/sc_fetch.sh '{url}' '{ua}' '{out}'");
    tf_init(&t, &c);
    CHECK(tf_find(&t, &c, 12, 3252, 1803, p, sizeof p) == 1);        /* preloaded pack wins */
    CHECK(tf_find(&t, &c, 11, 1626, 901, p, sizeof p) == 0);
    CHECK(tf_request(&t, &c, 11, 1626, 901, now) == 1);
    CHECK(tf_busy(&t, 11, 1626, 901) && tf_request(&t, &c, 11, 1626, 901, now) == 0); /* no duplicate */
    CHECK(tf_request(&t, &c, 11, 1627, 901, now) == 1);
    CHECK(tf_request(&t, &c, 11, 1628, 901, now) == 0);              /* both job slots in use */
    for (i = 0; i < 200 && n < 2; i++) { n += tf_poll(&t, &c, now); usleep(10000); }
    CHECK(n == 2 && t.done == 2 && t.failed == 0);
    CHECK(tf_find(&t, &c, 11, 1626, 901, p, sizeof p) == 2 && !strcmp(p, "/tmp/sc_tc/11/1626/901.png"));
    CHECK(access("/tmp/sc_tc/11/1626/901.part", F_OK) != 0);
    /* failure: wrong URL for the stand-in -> no file, no retry for TF_RETRY_S, then allowed again */
    CHECK(tf_request(&t, &c, 11, 1628, 901, now) == 1);
    for (i = 0; i < 200 && t.failed == 0; i++) { tf_poll(&t, &c, now); usleep(10000); }
    CHECK(t.failed == 1 && tf_find(&t, &c, 11, 1628, 901, p, sizeof p) == 0);
    CHECK(tf_request(&t, &c, 11, 1628, 901, now + 5) == 0);
    CHECK(tf_request(&t, &c, 11, 1628, 901, now + TF_RETRY_S + 1) == 1);
    for (i = 0; i < 200 && t.failed < 2; i++) { tf_poll(&t, &c, now + 40); usleep(10000); }
    c.tile_online = 0;
    CHECK(tf_request(&t, &c, 11, 1629, 901, now + 100) == 0);        /* offline mode never spawns */
    /* cache limit: two 300-byte tiles, cap 400 bytes -> the older one is removed */
    CHECK(system("touch -d '2020-01-01' /tmp/sc_tc/11/1626/901.png") == 0);
    CHECK(tf_sweep(&t, 400) == 300);
    CHECK(access("/tmp/sc_tc/11/1626/901.png", F_OK) != 0 && access("/tmp/sc_tc/11/1627/901.png", F_OK) == 0);
    tf_close(&t);

    /* recolour: land -> ground, white road -> light, water -> blue tint, dark label -> lightest */
    {
        static unsigned char img[256 * 3];
        for (i = 0; i < 256; i++) memcpy(&img[3 * i], &px[0], 3);    /* mostly land */
        memcpy(&img[0], &px[3], 3);
        memcpy(&img[3], &px[6], 3);
        memcpy(&img[6], &px[9], 3);
        tile_recolor(img, 256);
        CHECK(img[9] < 20 && img[10] < 16 && img[11] < 30);          /* land is near the UI ground colour */
        CHECK(img[0] > 110 && img[2] > 150);                         /* road */
        CHECK(img[3] == 31 && img[4] == 47 && img[5] == 122);        /* water */
        CHECK(img[6] == 167 && img[8] == 209);                       /* label */
    }
}

int main(int argc, char **argv)
{
    t_geo();
    t_isa();
    t_predict();
    t_parse();
    t_heading();
    t_subst();
    t_cfg();
    t_spec();
    t_ring();
    t_dsp();
    t_frames();
    t_feed();
    t_track();
    t_tiles();
    t_app(argc > 1 ? argv[1] : "tests/fake_decoder.sh", argc > 2 ? argv[2] : "./libfake_rtlsdr.so");
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
