#define _GNU_SOURCE
#include "../src/sc.h"
#include "../src/ui.h"
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
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
    CHECK(!gps_sky(&g, 1.0));
    CHECK(gps_parse(&g, "{\"class\":\"SKY\",\"device\":\"/dev/ttyACM0\",\"nSat\":2,\"uSat\":0,"
                        "\"satellites\":[{\"PRN\":4,\"used\":false}]}", 1.0) == 0);
    CHECK(g.n_sat == 2 && g.n_used == 0 && gps_sky(&g, 5.0) && !gps_sky(&g, 12.0) && !gps_ok(&g, 1.0));
    CHECK(gps_parse(&g, "{\"class\":\"SKY\",\"nSat\":12,\"uSat\":9}", 1.5) == 0 && g.n_sat == 12 && g.n_used == 9);
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

/* the sensor as mounted: heading h reads at raw angle (h + rot), mirrored when flipped, around a hard-iron offset */
static void mag_at(double h, double rot, int flip, double *x, double *y)
{
    double a = (flip ? -(h + rot) : h + rot) * M_PI / 180.0;
    *x = 120 + 300 * cos(a);
    *y = -80 + 300 * sin(a);
}

static void t_magcal(void)
{
    int flip;
    for (flip = 0; flip <= 1; flip++) {
        magcal_t m;
        cfg_t c;
        double h, x, y;
        cfg_defaults(&c);
        c.declination = -1.0;
        magcal_start(&m, 320, 0); /* 0.2 G at the BMM150's 0.000625 G per count */
        for (h = 0; h < 200; h += 4) { mag_at(h, 37, flip, &x, &y); magcal_add(&m, (double[3]){x, y, 0}, NULL); }
        CHECK(m.step == MAGCAL_TURN); /* half a turn is not enough */
        CHECK(magcal_covered(&m) < MAGCAL_SECTORS);
        for (; h <= 370 && m.step == MAGCAL_TURN; h += 4) { mag_at(h, 37, flip, &x, &y); magcal_add(&m, (double[3]){x, y, 0}, NULL); }
        CHECK(m.step == MAGCAL_NORTH); /* a full turn to the right moves on by itself */
        for (h = 0; h < 12; h++) { mag_at(0, 37, flip, &x, &y); magcal_add(&m, (double[3]){x, y, 0}, NULL); } /* north */
        CHECK(magcal_apply(&m, &c) == 0 && c.mag_tilt == 0); /* no accelerometer: flat only */
        NEAR(c.mag_off_x, 120, 2.0);
        NEAR(c.mag_off_y, -80, 2.0);
        CHECK(c.mag_flip_y == flip);
        mag_at(0, 37, flip, &x, &y);
        NEAR(norm180(heading_from_mag(x, y, &c)), 0.0, 0.5);
        mag_at(90, 37, flip, &x, &y);
        NEAR(heading_from_mag(x, y, &c), 90.0, 0.5);
        mag_at(225, 37, flip, &x, &y);
        NEAR(heading_from_mag(x, y, &c), 225.0, 0.5);
    }
    { /* a device lying still: noise alone (seen on the BMM150: about +-50 counts) must not fill the ring */
        magcal_t m;
        int i;
        magcal_start(&m, 320, 0);
        for (i = 0; i < 600; i++) magcal_add(&m, (double[3]){-350 + 50 * cos(i * 2.4), 1930 + 50 * sin(i * 1.7), 0}, NULL);
        CHECK(m.step == MAGCAL_TURN);
        CHECK(magcal_covered(&m) == 0);
    }
    { /* saved values come back through the normal config loader */
        cfg_t c, d;
        cfg_defaults(&c);
        cfg_defaults(&d);
        c.mag_off_x = -12.5; c.mag_off_y = 40.25; c.mag_flip_y = 1; c.mag_rot_deg = -123.4;
        c.mag_off_z = 866.5; c.mag_tilt = 1; snprintf(c.accel_axes, sizeof c.accel_axes, "-x+y-z");
        CHECK(magcal_save(&c, "/tmp/sc_compass.conf") == 0);
        CHECK(cfg_load(&d, "/tmp/sc_compass.conf") == 0);
        NEAR(d.mag_off_x, -12.5, 1e-3);
        NEAR(d.mag_off_y, 40.25, 1e-3);
        CHECK(d.mag_flip_y == 1);
        NEAR(d.mag_rot_deg, -123.4, 1e-3);
        NEAR(d.mag_off_z, 866.5, 1e-3);
        CHECK(d.mag_tilt == 1 && !strcmp(d.accel_axes, "-x+y-z"));
        unlink("/tmp/sc_compass.conf");
    }
}


/* A simulated device for tilt compensation. World axes east, north, up; the field points north and down by the
 * inclination. The device's top (+y) points at yaw (clockwise from north), then it pitches (top up) and rolls
 * (right side down). Each chip sees the device axes through its own signed permutation (both chips flat on the
 * board), the magnetometer around a hard-iron offset. */
typedef struct { int acc[3][3], mag[3][3]; double bh, bv; } simdev_t;

static void rot3(double v[3], int axis, double deg)
{
    double a = deg * M_PI / 180.0, c = cos(a), s = sin(a), r[3] = {v[0], v[1], v[2]};
    int i = (axis + 1) % 3, j = (axis + 2) % 3;
    v[i] = c * r[i] - s * r[j];
    v[j] = s * r[i] + c * r[j];
}

/* world vector -> device axes: undo yaw, then pitch (about x), then roll (about y) */
static void sim_dev(const double w[3], double yaw, double pitch, double roll, double d[3])
{
    d[0] = w[0]; d[1] = w[1]; d[2] = w[2];
    rot3(d, 2, yaw);    /* device turned clockwise by yaw: the world turns counter-clockwise under it */
    rot3(d, 0, -pitch);
    rot3(d, 1, roll);
}

static void sim_read(const simdev_t *s, double yaw, double pitch, double roll, double mag[3], double acc[3])
{
    const double up[3] = {0, 0, 4096}, b[3] = {0, s->bh, -s->bv}, off[3] = {235, 205, 867};
    double du[3], db[3];
    int i, j;
    sim_dev(up, yaw, pitch, roll, du);
    sim_dev(b, yaw, pitch, roll, db);
    for (i = 0; i < 3; i++) {
        acc[i] = 0;
        mag[i] = off[i];
        for (j = 0; j < 3; j++) { acc[i] += s->acc[i][j] * du[j]; mag[i] += s->mag[i][j] * db[j]; }
    }
}

/* the true heading of the device's top, from the same rotations applied forwards */
static double sim_heading(double yaw, double pitch, double roll)
{
    double f[3] = {0, 1, 0};
    rot3(f, 1, -roll);
    rot3(f, 0, pitch);
    rot3(f, 2, -yaw);
    return norm360(atan2(f[0], f[1]) * 180.0 / M_PI);
}

static void t_magcal_tilt(void)
{
    static const simdev_t devs[] = {
        /* CardputerZero as measured: BMM150 = (-x, +y, -z) of the BMI270, 0.465 G at about 20° inclination */
        {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}}, 700, 255},
        /* both chips the same way round, 65° inclination (northern Europe) */
        {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, 340, 730},
        /* accelerometer x/y swapped, a mirrored (left-handed) magnetometer, southern hemisphere (field points up) */
        {{{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}}, {{1, 0, 0}, {0, -1, 0}, {0, 0, 1}}, 420, -520},
    };
    size_t k;
    for (k = 0; k < sizeof devs / sizeof devs[0]; k++) {
        static magcal_t m;
        const simdev_t *s = &devs[k];
        cfg_t c;
        double h, mag[3], acc[3], worst = 0, worst_flat = 0;
        int d, i, p, r;
        cfg_defaults(&c);
        c.declination = 2.0;
        magcal_start(&m, 320, 1);
        for (h = 0; h <= 380 && m.step == MAGCAL_TURN; h += 4) { sim_read(s, h, 0, 0, mag, acc); magcal_add(&m, mag, acc); }
        CHECK(m.step == MAGCAL_TILT); /* with an accelerometer the turn leads to the tilt step */
        sim_read(s, 30, 0, 40, mag, acc);
        magcal_add(&m, mag, (double[3]){acc[0] * 1.3, acc[1] * 1.3, acc[2] * 1.3}); /* jolted: not a lean */
        CHECK(m.tn == 0);
        for (d = 0; d < 4; d++) /* forward, back, left, right: out to 35° and back */
            for (i = -35; i <= 35; i += 2) {
                double a = 35 - abs(i);
                sim_read(s, 30, d == 0 ? -a : (d == 1 ? a : 0), d == 2 ? -a : (d == 3 ? a : 0), mag, acc);
                magcal_add(&m, mag, acc);
            }
        CHECK(magcal_tilt_covered(&m) == 4 && m.step == MAGCAL_NORTH);
        for (i = 0; i < 12; i++) { sim_read(s, 0, 0, 0, mag, acc); magcal_add(&m, mag, acc); } /* flat, north */
        CHECK(magcal_apply(&m, &c) == 1 && c.mag_tilt == 1);
        NEAR(c.mag_off_z, 867, 5.0);
        for (h = 0; h < 360; h += 23)
            for (p = -30; p <= 30; p += 15)
                for (r = -30; r <= 30; r += 15) {
                    double e;
                    sim_read(s, h, p, r, mag, acc);
                    /* "north" is where the device pointed in the last step, declination included */
                    e = fabs(norm180(heading_tilt(mag, acc, &c) - sim_heading(h, p, r)));
                    if (e > worst) worst = e;
                    if (p == 0 && r == 0) { /* held flat the tilt compensated heading is the plain one, and so is the
                                             * flat-only fallback (used if the accelerometer goes away) */
                        e = fabs(norm180(heading_tilt(mag, acc, &c) - h));
                        if (e > worst_flat) worst_flat = e;
                        e = fabs(norm180(heading_from_mag(mag[0], mag[1], &c) - h));
                        if (e > worst_flat) worst_flat = e;
                    }
                }
        CHECK(worst < 1.0);
        CHECK(worst_flat < 0.5);
        sim_read(s, 0, 89, 0, mag, acc); /* top pointing at the sky: no heading */
        CHECK(isnan(heading_tilt(mag, acc, &c)));
    }
    { /* the flat-only heading is what the tilt step fixes: 20° of pitch at 65° inclination is far off */
        static magcal_t m;
        const simdev_t *s = &devs[1];
        cfg_t c;
        double h, mag[3], acc[3];
        cfg_defaults(&c);
        magcal_start(&m, 320, 0);
        for (h = 0; h <= 380 && m.step == MAGCAL_TURN; h += 4) { sim_read(s, h, 0, 0, mag, acc); magcal_add(&m, mag, NULL); }
        for (h = 0; h < 12; h++) { sim_read(s, 0, 0, 0, mag, acc); magcal_add(&m, mag, NULL); }
        CHECK(magcal_apply(&m, &c) == 0);
        sim_read(s, 90, 20, 0, mag, acc);
        CHECK(fabs(norm180(heading_from_mag(mag[0], mag[1], &c) - 90)) > 20);
    }
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
    CHECK(!strncmp(out, "PATH=" SC_DECODER_DIR ":", strlen("PATH=" SC_DECODER_DIR ":")));
    CHECK(sc_subst(out, 8, "0123456789", 0) == -1);
}

static void t_cfg(void)
{
    cfg_t c;
    FILE *f = fopen("/tmp/sc_test.conf", "w");
    fprintf(f, "# comment\nfreq_hz = 403500000\n gain=38.6 \nground_alt = 12.5\ndata_dir = /tmp/sc_data\nbogus = 1\n"
               "tile_dir = /usr/share/sondechase/tiles   # optional pack\ntile_url = https://t.example/{z}#x\n");
    fclose(f);
    cfg_defaults(&c);
    CHECK(cfg_load(&c, "/tmp/sc_test.conf") == 0);
    CHECK(c.freq_hz == 403500000 && !strcmp(c.gain, "38.6") && c.ground_alt == 12.5);
    CHECK(c.gpsd_port == 2947);
    CHECK(!strcmp(c.tile_dir, "/usr/share/sondechase/tiles"));  /* inline comment dropped */
    CHECK(!strcmp(c.tile_url, "https://t.example/{z}#x"));      /* a # inside a value stays */
    CHECK(strstr(c.tile_user_agent, "SondeChase/" SC_VERSION " ") == c.tile_user_agent);
    CHECK(cfg_load(&c, "/nonexistent/x.conf") == -1);
}

/* Framebuffer: config, then environment, then the udev /dev/fb_lcd link, then the ST7789V entry of /proc/fb, then fb0 */
static void t_fb(void)
{
    const char *pf = "/tmp/sc_test_procfb", *link = "/tmp/sc_test_fb_lcd", *none = "/nonexistent/fb_lcd";
    char fb[64];
    FILE *f = fopen(pf, "w");
    fprintf(f, "0 vc4drmfb\n1 fb_st7789v\n");
    fclose(f);
    fb_pick(fb, sizeof fb, "", NULL, none, pf);
    CHECK(!strcmp(fb, "/dev/fb1"));
    fb_pick(fb, sizeof fb, "", "/dev/fb2", none, pf);
    CHECK(!strcmp(fb, "/dev/fb2"));
    fb_pick(fb, sizeof fb, "/dev/fb3", "/dev/fb2", none, pf);
    CHECK(!strcmp(fb, "/dev/fb3"));
    f = fopen(pf, "w");
    fprintf(f, "0 vc4drmfb\n1 panel-mipi-dbid\n"); /* kernel 6.18: the LCD runs on the generic MIPI DBI panel driver */
    fclose(f);
    fb_pick(fb, sizeof fb, "", NULL, none, pf);
    CHECK(!strcmp(fb, "/dev/fb1"));
    fclose(fopen(link, "w"));
    fb_pick(fb, sizeof fb, "", NULL, link, pf);
    CHECK(!strcmp(fb, link));
    unlink(link);
    f = fopen(pf, "w");
    fprintf(f, "0 vc4drmfb\n");
    fclose(f);
    fb_pick(fb, sizeof fb, "", NULL, none, pf);
    CHECK(!strcmp(fb, "/dev/fb0"));
    fb_pick(fb, sizeof fb, "", "", none, "/nonexistent/fb");
    CHECK(!strcmp(fb, "/dev/fb0"));
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
    { /* asking for the 192 kHz stream as well leaves the 48 kHz one unchanged */
        static int16_t w[2 * 4 * (PAIRS / DDC_DECIM)];
        size_t nw;
        ddc_init(&d2);
        nb = ddc_process_wide(&d2, iq, sizeof iq, b, w, &nw);
        CHECK(nb == na && nw == 4 * na && !memcmp(a, b, na * 2 * sizeof a[0]));
        CHECK(cs16_power(w, (int)nw, 12000.0, DDC_WIDE_RATE) > 100.0 * cs16_power(w, (int)nw, -12000.0, DDC_WIDE_RATE));
    }
}

/* Held arrow keys repeat (after 0.4 s, every 0.08 s) until released; other keys act once. */
static void t_keys(void)
{
    int p[2], k, n = 0;
    double t0;
    struct input_event ev;
    CHECK(pipe(p) == 0);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    memset(&ev, 0, sizeof ev);
#define KEV(c, v) (ev.type = EV_KEY, ev.code = (c), ev.value = (v), write(p[1], &ev, sizeof ev) == (ssize_t)sizeof ev)
    CHECK(KEV(KEY_RIGHT, 1));
    CHECK(keys_read(p[0]) == K_RIGHT);
    CHECK(keys_read(p[0]) == 0); /* no repeat before the delay */
    t0 = sc_now();
    while (sc_now() - t0 < 1.0) {
        if ((k = keys_read(p[0])) != 0) {
            CHECK(k == (K_RIGHT | K_REPEAT));
            n++;
        }
        usleep(5000);
    }
    CHECK(n >= 6 && n <= 9); /* (1.0 - 0.4) / 0.08 = 7.5 */
    CHECK(KEV(KEY_RIGHT, 2)); /* the kernel's own auto-repeat is ignored */
    CHECK(KEV(KEY_RIGHT, 0));
    CHECK(keys_read(p[0]) == 0);
    usleep(200000);
    CHECK(keys_read(p[0]) == 0); /* released: no more repeats */
    CHECK(KEV(KEY_S, 1));
    CHECK(keys_read(p[0]) == 's');
    CHECK(KEV(KEY_TAB, 1));
    CHECK(keys_read(p[0]) == K_TAB);
    CHECK(KEV(KEY_Z, 1)); /* Z has no action of its own */
    CHECK(keys_read(p[0]) == 0);
    CHECK(KEV(KEY_CD, 1) && KEV(KEY_KP7, 1)); /* Sym held + R (printed -), as the driver sends it: zoom out */
    CHECK(keys_read(p[0]) == '-');
    CHECK(KEV(KEY_KP7, 0) && KEV(KEY_KP8, 1)); /* Sym held + T (printed +): zoom in */
    CHECK(keys_read(p[0]) == '+');
    CHECK(KEV(KEY_KP8, 0) && KEV(KEY_CD, 0) && KEV(KEY_R, 1)); /* plain R: restart the decoder */
    CHECK(keys_read(p[0]) == 'r');
    CHECK(KEV(KEY_K, 1)); /* K opens the compass calibration on Info */
    CHECK(keys_read(p[0]) == 'k');
    CHECK(KEV(KEY_KPMINUS, 1));
    CHECK(keys_read(p[0]) == '-');
    usleep(500000);
    CHECK(keys_read(p[0]) == 0);
#undef KEV
    close(p[0]);
    close(p[1]);
}

/* Zero crossings of pcm[from..n): twice the dominant frequency times the duration. */
static int zero_crossings(const int16_t *pcm, int from, int n)
{
    int i, zc = 0;
    for (i = from + 1; i < n; i++)
        if ((pcm[i - 1] < 0) != (pcm[i] < 0)) zc++;
    return zc;
}

/* cs16 at 48 kHz: a carrier at +12 kHz (the tuned frequency in the DDC stream) plus offset_hz, amplitude amp,
 * phase-modulated by pm(t) and amplitude-modulated by (1 + am * sin(1 kHz)). */
static void carrier_cs16(int16_t *iq, int np, double offset_hz, double amp, double fm_dev_hz, double am)
{
    int i;
    for (i = 0; i < np; i++) {
        double t = i / 48000.0, ph = 2.0 * M_PI * (12000.0 + offset_hz) * t, a = amp * (1.0 + am * sin(2.0 * M_PI * 1000.0 * t));
        ph += fm_dev_hz / 1000.0 * sin(2.0 * M_PI * 1000.0 * t);
        iq[2 * i] = (int16_t)lround(a * cos(ph));
        iq[2 * i + 1] = (int16_t)lround(a * sin(ph));
    }
}

/* Radio audio: each mode recovers its 1 kHz test tone (~1875 zero crossings in the 0.94 s after settling);
 * NFM squelches an empty channel; one sideband is kept for USB / LSB. */
static void t_radio(void)
{
    enum { NP = 48000 };
    static int16_t iq[2 * NP], pcm[NP / 3 + 1];
    static radio_t r;
    double rs = 0, rn = 0;
    int i, n, pk = 0;
    CHECK(radio_default_bw(DEMOD_NFM) == 16000 && radio_next_bw(DEMOD_NFM, 16000) == 25000);
    CHECK(radio_next_bw(DEMOD_USB, 3000) == 2400 && !strcmp(radio_mode_name(DEMOD_LSB), "LSB"));

    carrier_cs16(iq, NP, 0, 1000, 2400, 0); /* RS41-like deviation */
    radio_init(&r, DEMOD_NFM, 16000);
    n = radio_process(&r, iq, NP, pcm);
    CHECK(n == NP / 3);
    for (i = 1000; i < n; i++) {
        if (abs(pcm[i]) > pk) pk = abs(pcm[i]);
        rs += (double)pcm[i] * pcm[i];
    }
    i = zero_crossings(pcm, 1000, n);
    CHECK(i > 1800 && i < 1950);
    CHECK(pk > 7000 && pk < 11000);
    srand(7);
    for (i = 0; i < 2 * NP; i++) iq[i] = (int16_t)(rand() % 2001 - 1000);
    radio_init(&r, DEMOD_NFM, 16000);
    n = radio_process(&r, iq, NP, pcm);
    for (i = 1000; i < n; i++) rn += (double)pcm[i] * pcm[i];
    CHECK(sqrt(rn / (n - 1000)) < 2500 && sqrt(rs / (n - 1000)) > 5000);

    carrier_cs16(iq, NP, 0, 1000, 0, 0.5); /* AM, 50 % */
    radio_init(&r, DEMOD_AM, 6000);
    n = radio_process(&r, iq, NP, pcm);
    i = zero_crossings(pcm, 4000, n); /* after the carrier level has settled */
    CHECK(i > 1450 && i < 1550);      /* 0.75 s: ~1500 */
    for (pk = 0, i = 4000; i < n; i++) if (abs(pcm[i]) > pk) pk = abs(pcm[i]);
    CHECK(pk > 6000 && pk < 14000);   /* 50 % depth -> ~10000 */

    { /* a 1 kHz tone above the tuned frequency and a 2 kHz tone below: USB hears 1 kHz, LSB 2 kHz */
        static int16_t a[2 * NP];
        carrier_cs16(iq, NP, 1000, 1000, 0, 0);
        carrier_cs16(a, NP, -2000, 1000, 0, 0);
        for (i = 0; i < 2 * NP; i++) iq[i] = (int16_t)(iq[i] + a[i]);
        radio_init(&r, DEMOD_USB, 3000);
        n = radio_process(&r, iq, NP, pcm);
        i = zero_crossings(pcm, 1000, n);
        CHECK(i > 1800 && i < 1950);
        radio_init(&r, DEMOD_LSB, 3000);
        n = radio_process(&r, iq, NP, pcm);
        i = zero_crossings(pcm, 1000, n);
        CHECK(i > 3650 && i < 3850);
    }

    { /* broadcast FM, 75 kHz deviation, in the 192 kHz stream: WFM recovers the tone, NFM in 48 kHz cannot carry it */
        static int16_t w[2 * 192000];
        for (i = 0; i < 192000; i++) {
            double t = i / 192000.0, ph = 2.0 * M_PI * 12000.0 * t + 75.0 * sin(2.0 * M_PI * 1000.0 * t);
            w[2 * i] = (int16_t)lround(1000.0 * cos(ph));
            w[2 * i + 1] = (int16_t)lround(1000.0 * sin(ph));
        }
        CHECK(radio_default_bw(DEMOD_WFM) == 180000 && !strcmp(radio_mode_name(DEMOD_WFM), "WFM"));
        radio_init(&r, DEMOD_WFM, 180000);
        n = radio_process_wfm(&r, w, 192000, pcm);
        CHECK(n == 16000);
        i = zero_crossings(pcm, 1000, n);
        CHECK(i > 1850 && i < 1950); /* 0.9375 s of 1 kHz: ~1875 */
        for (pk = 0, i = 1000; i < n; i++) if (abs(pcm[i]) > pk) pk = abs(pcm[i]);
        CHECK(pk > 3000 && pk < 32767); /* the de-emphasis takes 1 kHz down ~1 dB */
    }
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
/* auto_rx-format logs under <data_dir>/log: count them (first one's path to *first), remove them if asked */
static int rx_logs(const char *dir, char *first, size_t n, int remove)
{
    char d[256], p[512];
    struct dirent *e;
    DIR *dp;
    int k = 0;
    snprintf(d, sizeof d, "%s/log", dir);
    if (!(dp = opendir(d))) return 0;
    while ((e = readdir(dp)))
        if (e->d_name[0] != '.') {
            snprintf(p, sizeof p, "%s/%s", d, e->d_name);
            if (k++ == 0 && first) snprintf(first, n, "%s", p);
            if (remove) unlink(p);
        }
    closedir(dp);
    return k;
}

/* Data folder: defaults under $HOME/sondechase; files from before 0.5.1 are moved in once */
static void t_data_paths(void)
{
    static app_t a;
    const char *home = getenv("HOME");
    char keep[256];
    FILE *f;
    snprintf(keep, sizeof keep, "%s", home ? home : "/root");
    CHECK(system("rm -rf /tmp/sc_home && mkdir -p /tmp/sc_home/.cache/sondechase/tiles/8/1") == 0);
    CHECK((f = fopen("/tmp/sc_home/sondechase.csv", "w")) && fputs("unix,id\n1,A\n", f) >= 0 && fclose(f) == 0);
    CHECK((f = fopen("/tmp/sc_home/.cache/sondechase/tiles/8/1/2.png", "w")) && fclose(f) == 0);
    setenv("HOME", "/tmp/sc_home", 1);
    app_init(&a, NULL);
    CHECK(!strcmp(a.cfg.data_dir, "/tmp/sc_home/sondechase") && !strcmp(a.tf.cache_dir, "/tmp/sc_home/sondechase/tiles"));
    CHECK(access("/tmp/sc_home/sondechase/sondechase.csv", R_OK) == 0 && access("/tmp/sc_home/sondechase.csv", F_OK) != 0);
    CHECK(access("/tmp/sc_home/sondechase/tiles/8/1/2.png", R_OK) == 0 && access("/tmp/sc_home/.cache/sondechase/tiles", F_OK) != 0);
    app_free(&a);
    setenv("HOME", keep, 1);
    CHECK(system("rm -rf /tmp/sc_home") == 0);
}

static void t_frames(void)
{
    static app_t a;
    const char *nofix = "{\"id\":\"Y0532351\",\"frame\":10,\"lat\":7.8,\"lon\":112.8,\"alt\":398,\"sats\":0,\"batt\":3.0}";
    const char *fix = "{\"id\":\"Y0532351\",\"frame\":12,\"lat\":21.03,\"lon\":105.85,\"alt\":40,\"sats\":9,\"batt\":3.0}";
    rx_logs("/tmp/sc_data", NULL, 0, 1);
    unlink("/tmp/sc_data/sondechase.csv"); /* left by an earlier run */
    app_init(&a, NULL);
    snprintf(a.cfg.data_dir, sizeof a.cfg.data_dir, "/tmp/sc_data");
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
    { /* on the Track screen each frame ticks and blinks the LED away from its resting state; M silences the tick */
        const char *led = "/tmp/sondechase_test_led";
        char v = 0;
        FILE *f = fopen(led, "w");
        fputs("255\n", f);
        fclose(f);
        led_close(&a.led);
        led_open(&a.led, led);
        CHECK(a.led.fd >= 0 && a.led.rest == '1');
        a.audio.nseg = 0;
        app_on_frame(&a, fix, 5.0); /* not on the Track screen: neither */
        CHECK(a.audio.nseg == 0 && a.led.t_off == 0);
        a.frame_cue = 1;
        app_on_frame(&a, fix, 6.0);
        CHECK(a.audio.nseg == 1 && a.audio.seg[0].freq == 1500 && a.led.t_off > 6.0);
        CHECK(pread(a.led.fd, &v, 1, 0) == 1 && v == '0');
        led_poll(&a.led, 6.2);
        CHECK(a.led.t_off == 0 && pread(a.led.fd, &v, 1, 0) == 1 && v == '1');
        a.prox.muted = 1;
        a.audio.nseg = 0;
        app_on_frame(&a, fix, 7.0);
        CHECK(a.audio.nseg == 0 && a.led.t_off > 7.0);
        led_close(&a.led); /* put back to rest when closed mid-blink */
        CHECK((f = fopen(led, "r")) && fgetc(f) == '1');
        fclose(f);
        unlink(led);
        led_open(&a.led, ""); /* no LED configured */
        CHECK(a.led.fd < 0);
        led_blink(&a.led, 8.0, 0.08);
        CHECK(a.led.t_off == 0);
    }
    app_free(&a);
    { /* a second session appends to sondechase.csv without a second header; no datetime: system clock */
        char l[512], path[512];
        int heads = 0, rows = 0;
        FILE *f;
        app_init(&a, NULL);
        snprintf(a.cfg.data_dir, sizeof a.cfg.data_dir, "/tmp/sc_data");
        app_on_frame(&a, fix, 9.0);
        app_free(&a);
        CHECK((f = fopen("/tmp/sc_data/sondechase.csv", "r")) != NULL);
        while (f && fgets(l, sizeof l, f)) {
            if (!strncmp(l, "unix,", 5)) heads++;
            else rows++;
        }
        if (f) fclose(f);
        CHECK(heads == 1 && rows == 5);
        CHECK(rx_logs("/tmp/sc_data", path, sizeof path, 0) >= 1 && (f = fopen(path, "r")) != NULL);
        if (f && fgets(l, sizeof l, f) && fgets(l, sizeof l, f))
            CHECK(strlen(l) > 24 && l[4] == '-' && l[10] == 'T' && l[19] == '.' && l[23] == 'Z' && l[24] == ',');
        if (f) fclose(f);
    }
    unlink("/tmp/sc_data/sondechase.csv");
    CHECK(rx_logs("/tmp/sc_data", NULL, 0, 1) >= 1); /* auto_rx format: only the sonde with a position has a file */
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
    CHECK(strlen(fakelib) < sizeof a.cfg.rtlsdr_lib); /* a cut path would fail every receiver test below */
    snprintf(a.cfg.rtlsdr_lib, sizeof a.cfg.rtlsdr_lib, "%s", fakelib); /* librtlsdr stand-in, 403.000 MHz carrier */
    a.sdr_ok = 1;
    a.cfg.ground_alt = 10;
    snprintf(a.cfg.data_dir, sizeof a.cfg.data_dir, "/tmp/sc_data");
    snprintf(a.cfg.decoder_feed_cmd, sizeof a.cfg.decoder_feed_cmd, "sh %s {f_hz}", fake);
    unlink("/tmp/sc_data/sondechase.csv");
    rx_logs("/tmp/sc_data", NULL, 0, 1);
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
        FILE *f = fopen("/tmp/sc_data/sondechase.csv", "r");
        char l[512];
        int rows = 0;
        CHECK(f != NULL);
        while (f && fgets(l, sizeof l, f)) rows++;
        if (f) fclose(f);
        CHECK(rows == 4); /* header + 3 frames */
    }
    { /* and the same frames in auto_rx's format, in a file named after the sonde */
        char path[512], l[512];
        int rows = 0;
        FILE *f;
        CHECK(rx_logs("/tmp/sc_data", path, sizeof path, 0) == 1);
        CHECK(strstr(path, "/sc_data/log/20261118-003100_S4430329_RS41_403.500_sonde.log") != NULL); /* sonde's time */
        CHECK((f = fopen(path, "r")) != NULL);
        while (f && fgets(l, sizeof l, f)) {
            if (rows == 0) CHECK(!strncmp(l, "timestamp,serial,frame,lat,lon,alt,vel_v,", 41));
            if (rows == 3) /* the third frame: its GPS time, no pressure sensor, burst timer 1200 s */
                CHECK(!strcmp(l, "2026-11-18T00:31:02.000Z,S4430329,5392,21.03000,105.95000,2976.0,-12.0,8.0,90.0,12.5,-1.0,-1.0,"
                                      "RS41-SGP,403.500,-1.0,-1,9,2.7,1200,-1\n"));
            rows++;
        }
        if (f) fclose(f);
        CHECK(rows == 4);
        rx_logs("/tmp/sc_data", NULL, 0, 1);
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

    /* Scan through the fake dongle: its 403.000 MHz carrier shows up and the sweep keeps running. */
    app_stop(&a);
    app_scan(&a);
    CHECK(a.state == ST_SCAN && a.sw.sdr == a.rx && a.rx != NULL);
    {
        long lo, hi;
        CHECK(!strcmp(sdr_range(a.rx, &lo, &hi), "R828D") && lo == 24000000L && hi == 1766000000L);
        CHECK(!strcmp(sdr_range(NULL, &lo, &hi), "RTL-SDR") && hi == 1766000000L);
    }
    for (i = 0; i < 500 && a.sw.passes < 6; i++) {
        app_poll(&a);
        usleep(10000);
    }
    CHECK(a.state == ST_SCAN && a.sw.sdr == a.rx && a.sw.passes >= 6);
    CHECK(a.cursor_hz == 403500000); /* on the last frequency, inside the band */
    { /* the fake's IQ has a DC offset (like a real tuner): no false peaks at the step centres */
        peak_t pk[MAX_PEAKS];
        CHECK(spec_peaks(&a.spec, 8.0, pk, MAX_PEAKS) == 1 && pk[0].f_hz == 403000000);
    }
    /* Enter on the overview: listen there; the open dongle switches job and rate, it is not reopened */
    t0 = sc_now();
    app_listen(&a, 403000000L);
    CHECK(a.state == ST_LISTEN && a.dsdr == a.rx && a.proc.pid == 0 && sc_now() - t0 < 0.3);
    a.radio_audio = 1;
    for (i = 0; i < 50; i++) {
        app_poll(&a);
        usleep(10000);
    }
    CHECK(a.audio.rhead - a.audio.rtail > 4000); /* ~0.5 s of the carrier's audio queued (no player here) */
    a.radio_audio = 0;
    app_poll(&a);
    CHECK(a.audio.rhead == a.audio.rtail);
    /* Enter on the tuned view: the decode is set on the same receiver */
    t0 = sc_now();
    app_tune(&a, a.freq_hz);
    CHECK(a.state == ST_DECODE && a.dsdr == a.rx && a.proc.pid > 0 && sc_now() - t0 < 0.3);
    /* tuning away stops the decoder until Enter sets it again; the receiver moves and stays open */
    app_listen(&a, 403010000L);
    CHECK(a.state == ST_LISTEN && a.proc.pid == 0 && a.dsdr == a.rx && a.freq_hz == 403010000L);
    CHECK(a.live.f0 == 403010000L - RS41_TUNE_OFFSET_HZ + DDC_RATE / 4 - DDC_RATE / 2);

    /* Scan: the fake's carrier is tried on the decoder; with no frame it is skipped and the sweep goes on,
     * with frames Scan stays there */
    snprintf(a.cfg.decoder_feed_cmd, sizeof a.cfg.decoder_feed_cmd, "sleep 5");
    app_hunt(&a);
    CHECK(a.state == ST_SCAN && a.hunt);
    for (i = 0; i < 500 && a.state == ST_SCAN; i++) {
        app_poll(&a);
        usleep(10000);
    }
    CHECK(a.state == ST_DECODE && a.hunt && a.freq_hz == 403000000L && a.proc.pid > 0);
    a.t_try = sc_now() - HUNT_TRY_S - 1.0;
    app_poll(&a);
    CHECK(a.state == ST_SCAN && a.hunt && a.proc.pid == 0 && app_skipped(&a, 403000000L, sc_now()));
    for (i = 0; i < 300; i++) {
        app_poll(&a);
        usleep(10000);
    }
    CHECK(a.state == ST_SCAN && a.hunt && a.sw.passes >= 3); /* its only peak was tried: it keeps sweeping */
    memset(a.skip, 0, sizeof a.skip);
    snprintf(a.cfg.decoder_feed_cmd, sizeof a.cfg.decoder_feed_cmd, "sh %s {f_hz}", fake);
    for (i = 0; i < 800 && a.hunt; i++) {
        app_poll(&a);
        usleep(10000);
    }
    CHECK(!a.hunt && a.state == ST_DECODE && a.freq_hz == 403000000L && !strcmp(a.status, "found RS41 S4430329"));
    app_listen(&a, 403100000L); /* tuning by hand while Scan runs ends it */
    CHECK(!a.hunt && a.state == ST_LISTEN);
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
        CHECK(system("mkdir -p /tmp/sc_usb/1-1.3 && echo 1d50 > /tmp/sc_usb/1-1.3/idVendor && "
                     "echo 6089 > /tmp/sc_usb/1-1.3/idProduct") == 0);
        CHECK(sdr_present("/tmp/sc_usb") == SDR_HACKRF);        /* HackRF One */
        CHECK(system("mkdir -p /tmp/sc_usb/1-1.2 && echo 0bda > /tmp/sc_usb/1-1.2/idVendor && "
                     "echo 2838 > /tmp/sc_usb/1-1.2/idProduct") == 0);
        CHECK(sdr_present("/tmp/sc_usb") == SDR_RTLSDR);        /* RTL-SDR Blog v4 too: it is used first */
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
    CHECK(t.why == TF_WHY_HTTP && !strcmp(t.host, "tiles.invalid")); /* the stand-in exits like curl -f on 404 */
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

    /* why downloads fail, from curl's exit status; a name answered with loopback only is a DNS block */
    CHECK(tf_host_blocked("localhost") == 1 && tf_host_blocked("127.0.0.1") == 1 && tf_host_blocked("192.0.2.1") == 0);
    {
        static const struct { int code; const char *url; int why; } cases[] = {
            {6, "https://tiles.invalid/{z}/{x}/{y}.png", TF_WHY_DNS_FAIL},
            {7, "https://localhost/{z}/{x}/{y}.png", TF_WHY_DNS_BLOCKED},
            {7, "https://192.0.2.1:8443/{z}/{x}/{y}.png", TF_WHY_NET},
        };
        int k;
        c.tile_online = 1;
        for (k = 0; k < 3; k++) {
            snprintf(c.tile_url, sizeof c.tile_url, "%s", cases[k].url);
            snprintf(c.tile_fetch_cmd, sizeof c.tile_fetch_cmd, "exit %d", cases[k].code);
            tf_init(&t, &c);
            CHECK(tf_request(&t, &c, 11, 1630 + k, 901, now) == 1);
            for (i = 0; i < 200 && t.failed == 0; i++) { tf_poll(&t, &c, now); usleep(10000); }
            CHECK(t.failed == 1 && t.why == cases[k].why);
            tf_close(&t);
        }
        CHECK(!strcmp(t.host, "192.0.2.1"));
    }

    /* recolour: land -> ground, white road -> light, water -> blue tint, dark label -> lightest */
    {
        static unsigned char img[256 * 3];
        for (i = 0; i < 256; i++) memcpy(&img[3 * i], &px[0], 3);    /* mostly land */
        memcpy(&img[0], &px[3], 3);
        memcpy(&img[3], &px[6], 3);
        memcpy(&img[6], &px[9], 3);
        tile_recolor(img, 256, 0);
        CHECK(img[9] < 20 && img[10] < 16 && img[11] < 30);          /* land is near the UI ground colour */
        CHECK(img[0] > 110 && img[2] > 150);                         /* road */
        CHECK(img[3] == 31 && img[4] == 47 && img[5] == 122);        /* water */
        CHECK(img[6] == 167 && img[8] == 209);                       /* label */
    }
    { /* simple: main roads and water kept, labels, buildings and land gone */
        static const unsigned char in[] = {242, 239, 233, 255, 255, 255, 170, 211, 223, 40, 40, 40, 232, 146, 162,
                                           252, 214, 164, 179, 0, 0, 217, 208, 201, 0, 90, 160};
        unsigned char img[sizeof in];
        memcpy(img, in, sizeof in);
        tile_recolor(img, (int)sizeof in / 3, 1);
        CHECK(img[0] == 10 && img[1] == 6 && img[2] == 20);     /* land -> ground */
        CHECK(img[3] == 40 && img[5] == 72);                    /* minor road -> dim */
        CHECK(img[6] == 31 && img[8] == 122);                   /* water */
        CHECK(img[9] == 10);                                    /* dark label -> ground */
        CHECK(img[12] == 167 && img[15] == 167);                /* motorway, primary -> bright */
        CHECK(img[18] == 10 && img[21] == 10 && img[24] == 10); /* red label, building, blue label -> ground */
    }
}

/* The HackRF back end through the libhackrf stand-in: the decoder's 1.536 MS/s runs at 3.072 MS/s and is halved,
 * the sweep at its own 2.048 MS/s; the carrier is heard, found by Scan at the right frequency, and decoded. */
static void t_hackrf(const char *fake, const char *fakelib)
{
    static app_t a;
    void *lib = dlopen(fakelib, RTLD_NOW | RTLD_LOCAL); /* the same copy the receiver loads: its getters */
    double (*rate)(void) = lib ? (double (*)(void))dlsym(lib, "fake_hackrf_rate") : NULL;
    uint32_t (*bw)(void) = lib ? (uint32_t (*)(void))dlsym(lib, "fake_hackrf_bw") : NULL;
    uint32_t (*lna)(void) = lib ? (uint32_t (*)(void))dlsym(lib, "fake_hackrf_lna") : NULL;
    uint32_t (*vga)(void) = lib ? (uint32_t (*)(void))dlsym(lib, "fake_hackrf_vga") : NULL;
    int (*amp)(void) = lib ? (int (*)(void))dlsym(lib, "fake_hackrf_amp") : NULL;
    int (*ant)(void) = lib ? (int (*)(void))dlsym(lib, "fake_hackrf_antenna") : NULL;
    long lo, hi;
    int i;
    CHECK(rate && bw && lna && vga && amp && ant);
    if (!rate || !bw || !lna || !vga || !amp || !ant) return;
    app_init(&a, NULL);
    a.cfg.gpsd_port = 0;
    a.cfg.mag_enable = 0;
    a.cfg.audio_enable = 0;
    a.cfg.sdr_check = 0;
    a.sdr_ok = 1;
    snprintf(a.cfg.sdr, sizeof a.cfg.sdr, "hackrf");
    snprintf(a.cfg.hackrf_lib, sizeof a.cfg.hackrf_lib, "%s", fakelib);
    snprintf(a.cfg.rtlsdr_lib, sizeof a.cfg.rtlsdr_lib, "/nonexistent/librtlsdr.so.0"); /* never the RTL-SDR */
    snprintf(a.cfg.data_dir, sizeof a.cfg.data_dir, "/tmp/sc_data");
    a.cfg.bias_tee = 1;

    app_listen(&a, 403000000L);
    CHECK(a.state == ST_LISTEN && a.rx != NULL && a.dsdr == a.rx);
    if (!a.rx) {
        app_free(&a);
        return;
    }
    CHECK(!strcmp(sdr_range(a.rx, &lo, &hi), "HackRF") && lo == 1000000L && hi == 6000000000L);
    NEAR(rate(), 2.0 * DDC_RATE, 1e-6);
    CHECK(bw() == 1750000);
    CHECK(lna() == 32 && vga() == 24 && amp() == 0 && ant() == 1); /* no gain set: LNA 32 + VGA 24 */
    a.radio_audio = 1;
    for (i = 0; i < 100 && a.audio.rhead - a.audio.rtail < 4000; i++) {
        app_poll(&a);
        usleep(10000);
    }
    CHECK(a.audio.rhead - a.audio.rtail > 4000); /* the halved stream carries the carrier's audio */
    CHECK(sdr_alive(a.rx) && sdr_dropped(a.rx) == 0);
    a.radio_audio = 0;
    app_poll(&a);

    app_scan(&a); /* the open HackRF switches job and rate */
    CHECK(a.state == ST_SCAN && a.sw.sdr == a.rx);
    for (i = 0; i < 800 && a.sw.passes < 4; i++) {
        app_poll(&a);
        usleep(10000);
    }
    NEAR(rate(), SWEEP_RATE, 1e-6); /* at or above 2 MS/s: as asked, not halved */
    CHECK(a.state == ST_SCAN && a.sw.passes >= 4);
    { /* a transfer from before a retune would smear the carrier over other steps: one peak, in its place */
        peak_t pk[MAX_PEAKS];
        CHECK(spec_peaks(&a.spec, 8.0, pk, MAX_PEAKS) == 1 && pk[0].f_hz == 403000000);
    }
    snprintf(a.cfg.decoder_feed_cmd, sizeof a.cfg.decoder_feed_cmd, "sh %s {f_hz}", fake);
    app_tune(&a, 403000000L);
    for (i = 0; i < 300 && a.frames < 2; i++) {
        app_poll(&a);
        usleep(10000);
    }
    CHECK(a.state == ST_DECODE && a.frames >= 2 && a.dsdr == a.rx);
    app_free(&a);

    /* gain as a total: LNA first, then VGA, then the amplifier */
    {
        static const struct { const char *g; uint32_t lna, vga; int amp; } t[] = {
            {"0", 0, 0, 0}, {"20", 16, 4, 0}, {"50", 40, 10, 0}, {"102", 40, 62, 0}, {"116", 40, 62, 1},
        };
        char err[96];
        cfg_t c;
        size_t k;
        cfg_defaults(&c);
        snprintf(c.sdr, sizeof c.sdr, "hackrf");
        snprintf(c.hackrf_lib, sizeof c.hackrf_lib, "%s", fakelib);
        for (k = 0; k < sizeof t / sizeof t[0]; k++) {
            sdr_t *s;
            snprintf(c.gain, sizeof c.gain, "%s", t[k].g);
            s = sdr_open(&c, SWEEP_RATE, err, sizeof err);
            CHECK(s != NULL && lna() == t[k].lna && vga() == t[k].vga && amp() == t[k].amp && ant() == 0);
            sdr_close(s);
        }
        c.rtl_dev = 1; /* the stand-in has one HackRF */
        CHECK(sdr_open(&c, SWEEP_RATE, err, sizeof err) == NULL && !strcmp(err, "no HackRF device"));
        snprintf(c.hackrf_lib, sizeof c.hackrf_lib, "/nonexistent/libhackrf.so.0");
        CHECK(sdr_open(&c, SWEEP_RATE, err, sizeof err) == NULL && !strcmp(err, "libhackrf0 not installed"));
    }
    dlclose(lib);
}

int main(int argc, char **argv)
{
    t_geo();
    t_isa();
    t_predict();
    t_parse();
    t_heading();
    t_magcal();
    t_magcal_tilt();
    t_subst();
    t_cfg();
    t_fb();
    t_spec();
    t_ring();
    t_dsp();
    t_radio();
    t_keys();
    t_data_paths();
    t_frames();
    t_feed();
    t_track();
    t_tiles();
    t_app(argc > 1 ? argv[1] : "tests/fake_decoder.sh", argc > 2 ? argv[2] : "./libfake_rtlsdr.so");
    t_hackrf(argc > 1 ? argv[1] : "tests/fake_decoder.sh", argc > 3 ? argv[3] : "./libfake_hackrf.so");
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
