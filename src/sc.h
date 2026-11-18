/* sondechase core API: no LVGL dependency, testable on any Linux host. */
#ifndef SC_H
#define SC_H
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#define RS41_TUNE_OFFSET_HZ 12000L /* rtl_fm tunes this far below the carrier */

/* ---------------- base ---------------- */
double sc_now(void); /* CLOCK_MONOTONIC, seconds */

typedef struct {
    long freq_hz;            /* start-up frequency, 0 = none */
    int ppm;                 /* RTL-SDR frequency correction */
    char gain[12];           /* "" = tuner AGC, else dB value e.g. "38.6" */
    int rtl_dev;             /* rtl-sdr device index */
    int bias_tee;            /* 1 = enable bias-T (-T) */
    char decoder_cmd[640];   /* shell pipeline that prints rs41mod JSON on stdout */
    char scan_cmd[320];      /* shell command that prints rtl_power CSV on stdout */
    long scan_lo_hz, scan_hi_hz, scan_step_hz;
    int scan_int_s;          /* rtl_power integration time */
    double scan_snr_db;      /* peak threshold above median noise floor */
    int auto_tune;           /* tune to strongest peak after a scan */
    char gpsd_host[64];
    int gpsd_port;           /* 0 = gpsd disabled */
    double ground_alt;       /* landing altitude AMSL; < -900 = use own GPS altitude */
    double burst_alt;        /* assumed burst altitude while the sonde ascends */
    double descent_v0;       /* assumed sea-level descent rate while ascending, m/s */
    double cog_min_speed;    /* m/s above which GPS course is used as heading */
    int mag_enable;
    double mag_off_x, mag_off_y, mag_rot_deg;
    int mag_flip_y;
    double declination;      /* magnetic declination, degrees east positive */
    double stale_s;          /* telemetry older than this is marked stale */
    char log_dir[160];       /* "" = $HOME */
    char kbd_dev[64];        /* "" = auto-detect evdev keyboard */
    char fb_dev[64];
    double prox_radius_m;    /* proximity beeper starts inside this ground distance */
    double prox_hyst_m;      /* ...and stops beyond radius + hysteresis */
    int audio_enable;
    char audio_cmd[200];     /* reads raw S16_LE mono 16 kHz PCM on stdin */
    char tile_dir[160];      /* preloaded map tiles: <tile_dir>/<z>/<x>/<y>.png */
    int zoom_min, zoom_max;  /* zoom levels present in tile_dir */
    char font_dir[160];      /* Orbitron-Bold.ttf, Rajdhani-SemiBold.ttf */
    int sdr_check;           /* 1 = require an RTL2832U on USB (0bda:2838 / 0bda:2832) */
    int tile_online;         /* 1 = download missing map tiles over the network while the map shows them */
    char tile_url[200];      /* {z} {x} {y} template */
    char tile_user_agent[120];
    char tile_fetch_cmd[240]; /* {ua} {url} {out}; must write the tile to {out} and exit 0 */
    char tile_cache_dir[200]; /* "" = $HOME/.cache/sondechase/tiles */
    int tile_cache_mb;       /* downloaded tiles above this total are removed, oldest first */
    int demo;                /* 1 = simulated flight: no SDR, GPS or decoder needed */
    double demo_speed;       /* simulated seconds per real second */
} cfg_t;

void cfg_defaults(cfg_t *c);
int cfg_load(cfg_t *c, const char *path); /* 0 ok, -1 file not readable */
/* Expand {ppm} {dev} {gain} {bias} {f_hz} {f_tune} {lo} {hi} {step} {int}. */
int sc_subst(char *out, size_t n, const char *tpl, const cfg_t *c, long f_hz);

/* Non-blocking line reader on a file descriptor. One rtl_power CSV row is ~7 bytes per bin:
 * ~3 kB at the default 5 kHz step, ~14 kB at 1 kHz, so callers size their line buffer with LR_MAX. */
#define LR_MAX 16384
typedef struct {
    int fd;
    size_t len;
    char buf[LR_MAX];
} lr_t;
int lr_readline(lr_t *r, char *out, size_t n); /* 1 line, 0 nothing yet, -1 EOF/error */

/* Child shell pipeline in its own process group. */
typedef struct {
    pid_t pid;
    lr_t lr;
} proc_t;
void proc_init(proc_t *p);
int proc_start(proc_t *p, const char *cmd);
void proc_stop(proc_t *p);

/* Flat-JSON field extraction (rs41mod and gpsd TPV objects are flat). */
int js_num(const char *s, const char *key, double *out);
int js_str(const char *s, const char *key, char *out, size_t n);

/* ---------------- geodesy / prediction ---------------- */
double geo_dist(double lat1, double lon1, double lat2, double lon2);    /* m, haversine */
double geo_bearing(double lat1, double lon1, double lat2, double lon2); /* deg true, 0..360 */
void geo_dest(double lat, double lon, double brg_deg, double dist_m, double *lat2, double *lon2);
double geo_elev(double ground_dist_m, double h_obs, double h_tgt); /* deg, spherical earth */
double norm360(double d);
double norm180(double d);
double isa_rho(double h_m); /* ISA air density, kg/m^3, 0..47 km */
double isa_p(double h_m);   /* ISA pressure, hPa */
double dewpoint(double t_c, double rh_pct); /* Magnus formula, deg C */

#define WBIN_M 250.0
#define WBINS 160 /* 0..40 km */
typedef struct {
    float e[WBINS], n[WBINS];
    unsigned char ok[WBINS];
} wind_t;
void wind_add(wind_t *w, double alt, double vel_h, double heading_deg);
void wind_get(const wind_t *w, double alt, double def_e, double def_n, double *e, double *n);

typedef struct {
    char id[20];
    int frame, sats, bt;
    double lat, lon, alt, vel_h, vel_v, heading, temp, batt;
    double humidity;  /* %, < 0 = not reported */
    double pressure;  /* hPa; when the sonde has no sensor: ISA value from altitude */
    int pres_est;     /* 1 = pressure is the ISA estimate */
    char subtype[16];
    double t_rx;
} sonde_t;
/* 1 = valid position frame; 2 = telemetry decoded but the sonde has no GPS solution yet
 * (fewer than 4 satellites: lat/lon/alt are not usable); 0 = not a sonde frame. */
int sonde_parse(const char *line, sonde_t *s);

typedef struct {
    int valid;
    double lat, lon, t_s;
} pred_t;
pred_t predict_landing(const sonde_t *s, double vv, const wind_t *w, double ground_alt,
                       double burst_alt, double v0);

/* ---------------- gps / heading ---------------- */
typedef struct {
    lr_t lr;
    double t_retry;
    int mode; /* gpsd: 0/1 no fix, 2 = 2D, 3 = 3D */
    double lat, lon, alt, speed, track;
    int has_track;
    double t_fix;
} gps_t;
void gps_init(gps_t *g);
void gps_poll(gps_t *g, const cfg_t *c);
int gps_parse(gps_t *g, const char *line, double now); /* 1 = TPV consumed */
int gps_ok(const gps_t *g, double now);
void gps_close(gps_t *g);

enum { HDG_NONE, HDG_GPS, HDG_MAG, HDG_HOLD };
typedef struct {
    double deg;
    int src;
    int mag_found;
    double t_mag;
    char mag_x[160], mag_y[160];
} heading_t;
void heading_init(heading_t *h, const cfg_t *c);
void heading_update(heading_t *h, const gps_t *g, const cfg_t *c, double now);
double heading_from_mag(double x, double y, const cfg_t *c);

/* 1 when an RTL2832U receiver is listed under <usb_sysfs> (normally /sys/bus/usb/devices). */
int sdr_present(const char *usb_sysfs);

/* ---------------- spectrum scan ---------------- */
#define SPEC_MAX 2048
typedef struct {
    long f0, step;
    int n;
    float floor_db, max_db;
    float db[SPEC_MAX];
    unsigned char ok[SPEC_MAX];
} spectrum_t;
typedef struct {
    long f_hz;
    float snr_db;
} peak_t;
void spec_reset(spectrum_t *s, long lo, long hi, long step);
int spec_add_line(spectrum_t *s, const char *line); /* one rtl_power CSV row */
int spec_peaks(spectrum_t *s, double snr_db, peak_t *out, int max);

/* ---------------- flight history, burst detection, proximity beeper, audio, map tiles ---------------- */
#define HIST_MAX 2048
typedef struct {
    float t_s, alt, vv, temp, rh, pres, ws, wd; /* t_s = seconds since first frame */
    float lat, lon;                             /* float: ~1 m resolution, enough for the map trail */
} hist_pt_t;
typedef struct {
    int n;
    double t0, t_last, step_s; /* step doubles each time the buffer is decimated */
    hist_pt_t p[HIST_MAX];
} hist_t;
void hist_reset(hist_t *h);
int hist_add(hist_t *h, const sonde_t *s, double now); /* 1 = sample stored */

enum { PH_UNKNOWN, PH_ASCENT, PH_DESCENT, PH_LANDED };
typedef struct {
    int phase, neg_cnt, still_cnt, burst;
    double max_alt, burst_alt, burst_t, t_first;
} flight_t;
void flight_reset(flight_t *f);
int flight_update(flight_t *f, const sonde_t *s, double now); /* 1 = burst detected by this frame */

typedef struct {
    int active, muted;
    double t_next;
} prox_t;
double prox_interval(double d_m); /* seconds between beeps */
double prox_volume(double d_m);   /* 0.3 .. 1.0 */
int prox_update(prox_t *p, int have_dist, double d_m, double now, double radius, double hyst); /* 1 = beep now */

#define AUDIO_RATE 16000
#define AUDIO_SEGS 8
typedef struct {
    pid_t pid;
    int fd;
    double t_last, t_retry, phase;
    int nseg, seg_pos;
    struct { float freq, vol; int n; } seg[AUDIO_SEGS];
} audio_t;
void audio_init(audio_t *a);
void audio_tone(audio_t *a, double freq_hz, double dur_s, double vol); /* queue a tone (freq 0 = silence) */
int audio_render(audio_t *a, short *out, int n);                       /* fill n samples from the queue */
void audio_poll(audio_t *a, const cfg_t *c, double now);               /* keep the PCM stream fed */
void audio_close(audio_t *a);

#define TILE_PX 256
void tile_frac(double lat, double lon, int z, double *tx, double *ty); /* fractional slippy-map tile coords */
double tile_mpp(double lat, int z);                                    /* metres per pixel */
int tile_fit_zoom(double lat1, double lon1, double lat2, double lon2, int w_px, int h_px, int zmin, int zmax);

/* On-demand tile download with a disk cache. Only tiles the map screen asks for are fetched,
 * at most TF_JOBS at a time; failures are not retried for TF_RETRY_S. */
#define TF_JOBS 2
#define TF_FAILS 32
#define TF_RETRY_S 30.0
typedef struct {
    struct { int z, x, y; pid_t pid; char part[300], dst[300]; } job[TF_JOBS];
    struct { int z, x, y; double t_retry; } fail[TF_FAILS];
    int nfail_slot;
    unsigned done, failed, since_sweep;
    double t_last_ok, t_last_fail;
    char cache_dir[240];
} tilefetch_t;
void tf_init(tilefetch_t *t, const cfg_t *c);
/* Find a tile on disk: 1 = preloaded (tile_dir, already in UI colours), 2 = downloaded cache
 * (original colours), 0 = not present. */
int tf_find(const tilefetch_t *t, const cfg_t *c, int z, int x, int y, char *path, size_t n);
int tf_request(tilefetch_t *t, const cfg_t *c, int z, int x, int y, double now); /* 1 = download started */
int tf_busy(const tilefetch_t *t, int z, int x, int y);                          /* 1 = download in progress */
int tf_poll(tilefetch_t *t, const cfg_t *c, double now);                         /* number of tiles completed */
long tf_sweep(const tilefetch_t *t, long max_bytes);                             /* bytes left in the cache */
void tf_close(tilefetch_t *t);
/* Light street-map colours -> UI palette, in place on an RGB888 buffer. */
void tile_recolor(unsigned char *rgb, int npix);

/* ---------------- application state ---------------- */
enum { ST_IDLE, ST_DECODE, ST_SCAN };
#define MAX_PEAKS 8
typedef struct {
    cfg_t cfg;
    int state;
    long freq_hz;
    proc_t proc;
    double t_restart;
    sonde_t sonde;
    int have_sonde;
    sonde_t nofix;           /* last frame from a sonde without a GPS solution (t_rx = 0: none) */
    unsigned frames;
    double vv_s; /* smoothed vertical velocity */
    gps_t gps;
    heading_t hdg;
    wind_t wind;
    pred_t pred;
    spectrum_t spec;
    peak_t peaks[MAX_PEAKS];
    int npeaks, sel;
    int have_geo; /* dist/brg/elev valid */
    double dist, brg, elev, rel;
    int have_land;
    double land_dist, land_brg;
    FILE *log;
    char log_path[256];
    char status[80];
    hist_t hist;
    flight_t flight;
    prox_t prox;
    audio_t audio;
    int burst_alert; /* set when a burst is detected; the UI clears it on dismiss */
    int sdr_ok;      /* receiver present on USB; decoding and scanning wait while 0 */
    double t_sdr;
    tilefetch_t tf;
    struct {
        int init, phase, frame; /* phase: 0 ascent, 1 descent, 2 landed */
        double tv, t_last, t_land, lat, lon, alt, own_lat, own_lon;
    } demo;
} app_t;
void app_init(app_t *a, const char *cfg_path);
void app_poll(app_t *a);
void app_tune(app_t *a, long f_hz);
void app_scan(app_t *a);
void app_stop(app_t *a);
void app_free(app_t *a);
void app_on_frame(app_t *a, const char *json_line, double now); /* one decoder output line */

/* Simulated flight (cfg.demo): ascent at 5 m/s to 31.2 km, burst, parachute descent, landing. */
void demo_poll(app_t *a, double now);
void demo_advance(app_t *a, double virtual_seconds, double now);

#endif
