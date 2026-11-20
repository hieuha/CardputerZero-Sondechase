/* sondechase core API: no LVGL dependency, testable on any Linux host. */
#ifndef SC_H
#define SC_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define RS41_TUNE_OFFSET_HZ 12000L /* the decoder stream is centred this far below the carrier */

/* ---------------- base ---------------- */
double sc_now(void); /* CLOCK_MONOTONIC, seconds */

typedef struct {
    long freq_hz;            /* start-up frequency, 0 = none */
    int ppm;                 /* RTL-SDR frequency correction */
    char gain[12];           /* "" = tuner AGC, else dB value e.g. "38.6" */
    int rtl_dev;             /* rtl-sdr device index */
    int bias_tee;            /* 1 = power an LNA through the bias-T */
    char rtlsdr_lib[256];    /* librtlsdr to dlopen; "" = librtlsdr.so.0 (tests point it at a fake) */
    char decoder_feed_cmd[400]; /* reads 48 kHz cs16 IQ on stdin, prints rs41mod JSON */
    long scan_lo_hz, scan_hi_hz, scan_step_hz;
    char gpsd_host[64];
    int gpsd_port;           /* 0 = gpsd disabled */
    double ground_alt;       /* landing altitude AMSL; < -900 = use own GPS altitude */
    double burst_alt;        /* assumed burst altitude while the sonde ascends */
    double descent_v0;       /* assumed sea-level descent rate while ascending, m/s */
    double cog_min_speed;    /* m/s above which GPS course is used as heading */
    int mag_enable;
    double mag_off_x, mag_off_y, mag_off_z, mag_rot_deg;
    int mag_flip_y;
    int mag_tilt;            /* 1 = tilt-compensated with the accelerometer (set by calibration when it can be) */
    char accel_axes[8];      /* accelerometer axes as magnetometer axes, e.g. "+y-x+z" (set by calibration) */
    double declination;      /* magnetic declination, degrees east positive */
    double map_lat, map_lon; /* Map centre while neither this device nor a sonde has a position */
    int map_simple;          /* 1: downloaded tiles show only water and roads (V on the Map toggles) */
    double stale_s;          /* telemetry older than this is marked stale */
    char data_dir[160];      /* "" = $HOME/sondechase: sondechase.csv, log/ (auto_rx format), tiles/ (map cache) */
    char kbd_dev[64];        /* "" = auto-detect evdev keyboard */
    char fb_dev[64];
    double prox_radius_m;    /* proximity beeper starts inside this ground distance */
    double prox_hyst_m;      /* ...and stops beyond radius + hysteresis */
    int audio_enable;
    char led_path[96];       /* LED brightness file blinked on each frame on the Track and Map screens; "" = none */
    char audio_cmd[200];     /* reads raw S16_LE mono 16 kHz PCM on stdin */
    char tile_dir[160];      /* preloaded map tiles: <tile_dir>/<z>/<x>/<y>.png */
    int zoom_min, zoom_max;  /* zoom levels present in tile_dir */
    char font_dir[160];      /* Orbitron-Bold.ttf, Rajdhani-SemiBold.ttf */
    int sdr_check;           /* 1 = require an RTL2832U on USB (0bda:2838 / 0bda:2832) */
    int tile_online;         /* 1 = download missing map tiles over the network while the map shows them */
    char tile_url[200];      /* {z} {x} {y} template */
    char tile_user_agent[120];
    char tile_fetch_cmd[240]; /* {ua} {url} {out}; must write the tile to {out} and exit 0 */
    char tile_cache_dir[200]; /* "" = <data_dir>/tiles */
    int tile_cache_mb;       /* downloaded tiles above this total are removed, oldest first */
    int demo;                /* 1 = simulated flight: no SDR, GPS or decoder needed */
    double demo_speed;       /* simulated seconds per real second */
} cfg_t;

void cfg_defaults(cfg_t *c);
int cfg_load(cfg_t *c, const char *path); /* 0 ok, -1 file not readable */
/* Expand {f_hz} (the tuned sonde frequency) in a command template; -1 if out is too small. */
int sc_subst(char *out, size_t n, const char *tpl, long f_hz);
/* The LCD framebuffer: fb_dev from the config, else $LV_LINUX_FBDEV_DEVICE, else lcd_link (/dev/fb_lcd, made by the
 * image's udev rule for the st7789v node), else the ST7789V entry in procfb (/proc/fb: fb_st7789v, or panel-mipi-dbi
 * on newer kernels), else /dev/fb0. The LCD is not always fb0: with HDMI attached it can be fb1. */
void fb_pick(char *out, size_t n, const char *cfg_dev, const char *env, const char *lcd_link, const char *procfb);

/* Non-blocking line reader on a file descriptor (decoder JSON, gpsd reports). The longest lines are gpsd
 * SKY reports listing every satellite, a few kB; callers size their line buffer with LR_MAX. */
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
    int wfd;                 /* child's stdin (proc_start_feed), -1 otherwise */
    unsigned long wdrop;     /* bytes proc_feed dropped because the child was not keeping up */
} proc_t;
void proc_init(proc_t *p);
int proc_start(proc_t *p, const char *cmd);      /* stdin = /dev/null */
int proc_start_feed(proc_t *p, const char *cmd); /* stdin = a pipe written with proc_feed */
void proc_feed(proc_t *p, const void *buf, size_t n); /* n a multiple of the stream's frame size */
void proc_stop(proc_t *p);
void mkdirs(const char *file_path); /* create every parent directory of file_path */
void reap_group(pid_t pg); /* collect the remaining members of a killed child process group */

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
    char datetime[32]; /* the sonde's GPS time, "YYYY-MM-DDTHH:MM:SS[.fff]Z"; "" = not reported */
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
    int n_sat, n_used; /* gpsd SKY: satellites in view / used in the fix */
    double t_sky;      /* last SKY report: the receiver is plugged in and talking even without a fix */
} gps_t;
void gps_init(gps_t *g);
void gps_poll(gps_t *g, const cfg_t *c);
int gps_parse(gps_t *g, const char *line, double now); /* 1 = TPV consumed */
int gps_ok(const gps_t *g, double now);
int gps_sky(const gps_t *g, double now); /* 1 = satellite counts are current */
void gps_close(gps_t *g);

enum { HDG_NONE, HDG_GPS, HDG_MAG, HDG_HOLD };
typedef struct {
    double deg;
    int src;
    int mag_found;
    int acc_found;    /* accelerometer x/y/z and magnetometer z found: tilt compensation possible */
    double mag_scale; /* gauss per raw count (in_magn_scale), 0 = not reported */
    double t_mag;
    double acc[3];    /* accelerometer, low-passed, raw counts */
    int acc_ok;
    char mag_x[160], mag_y[160], mag_z[160], acc_x[160], acc_y[160], acc_z[160];
} heading_t;
void heading_init(heading_t *h, const cfg_t *c);
void heading_update(heading_t *h, const gps_t *g, const cfg_t *c, double now);
double heading_from_mag(double x, double y, const cfg_t *c); /* held flat */
double heading_tilt(const double mag[3], const double acc[3], const cfg_t *c); /* any attitude; NAN near vertical */
int mag_read(const heading_t *h, double mag[3]); /* raw magnetometer; z = 0 without tilt support; 0 = no sensor */
int acc_read(const heading_t *h, double acc[3]); /* raw accelerometer; 0 = none or read failed */

/* Compass calibration. Step 1: the device, held flat, turns right through a full circle; the centre of the raw x/y
 * circle gives the hard-iron offsets and the direction the raw angle moved gives the axis flip. Step 2 (with an
 * accelerometer): the device leans forward, back, left and right; that gives the z offset and how the accelerometer's
 * axes line up with the magnetometer's. Last step: the device points north and the rotation is set so that reading
 * becomes 0. */
#define MAGCAL_SECTORS 36
#define MAGCAL_MAX 1200 /* 60 s of samples at 20 Hz */
#define MAGCAL_TILT_MAX 400
#define MAGCAL_TILT_DEG 25.0 /* lean that counts for one of the four tilt directions */
enum { MAGCAL_OFF, MAGCAL_TURN, MAGCAL_TILT, MAGCAL_NORTH };
typedef struct {
    int step, n;
    int use_acc;     /* 1 = tilt step too */
    double min_span; /* raw counts the x and y ranges must reach: a real turn, not sensor noise */
    double xmin, xmax, ymin, ymax;
    float xs[MAGCAL_MAX], ys[MAGCAL_MAX], zs[MAGCAL_MAX], as[MAGCAL_MAX][3];
    unsigned char seen[MAGCAL_SECTORS]; /* 10° sectors of the raw angle the turn has passed through */
    double acc_level[3];                /* accelerometer summed over the level turn: "up" while flat */
    int acc_level_n;
    float tm[MAGCAL_TILT_MAX][3], ta[MAGCAL_TILT_MAX][3]; /* magnetometer and accelerometer while leaning */
    int tn, tn_all;
    unsigned char tilt_seen[4];
    double t_poll;
} magcal_t;
void magcal_start(magcal_t *m, double min_span, int use_acc);
double mag_min_span(const heading_t *h); /* min_span for this sensor */
void magcal_add(magcal_t *m, const double mag[3], const double *acc); /* acc NULL = none; steps on by itself */
int magcal_covered(const magcal_t *m);      /* turn sectors seen, 0..MAGCAL_SECTORS */
int magcal_tilt_covered(const magcal_t *m); /* tilt directions seen, 0..4 */
int magcal_apply(const magcal_t *m, cfg_t *c); /* offsets, axes, rotation that makes "now" read north; 1 = tilt */
int magcal_save(const cfg_t *c, const char *path); /* 0 ok */

/* 1 when an RTL2832U receiver is listed under <usb_sysfs> (normally /sys/bus/usb/devices). */
int sdr_present(const char *usb_sysfs);

/* ---------------- DSP on u8 IQ from the RTL-SDR ---------------- */
typedef struct {
    int n;                      /* FFT size, power of two */
    float *re, *im, *win, *cs, *sn;
    int *rev;
} fft_t;
int fft_init(fft_t *f, int n);
void fft_free(fft_t *f);
/* Average power of `frames` consecutive n-pair FFTs of u8 IQ (2*n*frames bytes), Hann window,
 * written to db[n] in dB relative to a full-scale tone, lowest frequency first (DC at n/2). */
void fft_spectrum(fft_t *f, const unsigned char *iq, int frames, float *db);

/* Decoder front end, a port of rtl_fm's "-M raw -F9 -s 48000" chain: the dongle samples at
 * DDC_RATE and is tuned DDC_RATE/4 above the wanted centre; a 90-degree rotation brings that
 * centre to 0 Hz, five half-band (binomial) decimate-by-2 stages and a 9-tap droop-compensation
 * FIR give 48 kHz complex int16, the stream fsk_demod --cs16 reads. */
#define DDC_RATE 1536000L
#define DDC_DECIM 32
#define DDC_BLOCK 8192 /* input IQ pairs per processing block, a multiple of DDC_DECIM */
typedef struct {
    unsigned phase;            /* rotation step (0..3) of the next input pair */
    int npend;                 /* IQ pairs waiting in pend */
    int16_t pend[2 * DDC_BLOCK];
    int16_t hist_i[5][6], hist_q[5][6], droop_i[9], droop_q[9];
} ddc_t;
void ddc_init(ddc_t *d);
/* Consume nbytes (even) of u8 IQ; write 48 kHz cs16 pairs to out, which must hold at least
 * nbytes / (2 * DDC_DECIM) + DDC_BLOCK / DDC_DECIM pairs. Returns pairs written. */
size_t ddc_process(ddc_t *d, const unsigned char *iq, size_t nbytes, int16_t *out);
/* The same, and also the stream after the third halving (DDC_WIDE_RATE, the tuned frequency again at
 * +RS41_TUNE_OFFSET_HZ) into wide, which must hold 4x the pairs of out; *nwide gets its pairs. */
#define DDC_WIDE_PASS 2
#define DDC_WIDE_RATE (DDC_RATE >> (DDC_WIDE_PASS + 1)) /* 192 kHz: wide enough for broadcast FM */
size_t ddc_process_wide(ddc_t *d, const unsigned char *iq, size_t nbytes, int16_t *out, int16_t *wide, size_t *nwide);

/* Listening on the tuned frequency: demodulator over the DDC's 48 kHz output, where the tuned frequency sits at
 * +RS41_TUNE_OFFSET_HZ. Shift it to 0 Hz, filter the channel (bw_hz wide; one sideband for USB/LSB),
 * demodulate, and average down to AUDIO_RATE. NFM has a noise squelch that drops an empty channel to a murmur;
 * AM follows its carrier level and SSB has an AGC. WFM (broadcast FM, +-75 kHz deviation) does not fit in
 * 48 kHz: it takes the DDC's 192 kHz stream instead (radio_process_wfm). */
enum { DEMOD_NFM, DEMOD_AM, DEMOD_USB, DEMOD_LSB, DEMOD_WFM, DEMOD_N };
#define RADIO_TAPS 63
typedef struct {
    int mode;
    long bw_hz;
    float tr[RADIO_TAPS], ti[RADIO_TAPS];            /* channel filter, complex for one sideband */
    float hi[2 * RADIO_TAPS], hq[2 * RADIO_TAPS];    /* its history, doubled to read it without wrapping */
    int pos;
    unsigned phase;            /* frequency shift step (0..3; WFM 0..15) */
    float pi, pq;              /* previous filtered sample, for the FM discriminator */
    float acc;                 /* decimation by 3 */
    int nacc;
    float sq_sum, sq_thr, gain; /* NFM squelch: discriminator mean square over a window, threshold, gain */
    int sq_n, open;
    float level;               /* AM carrier / SSB envelope */
    float deemph, lp;          /* WFM: 50 us de-emphasis and a further low-pass, both one-pole */
} radio_t;
void radio_init(radio_t *r, int mode, long bw_hz);
/* npairs 48 kHz cs16 pairs in; at most npairs / 3 + 1 audio samples out (16 kHz). Returns samples written. */
int radio_process(radio_t *r, const int16_t *iq, size_t npairs, int16_t *out);
/* WFM: npairs DDC_WIDE_RATE cs16 pairs in; at most npairs / 12 + 1 audio samples out. */
int radio_process_wfm(radio_t *r, const int16_t *iq, size_t npairs, int16_t *out);
const char *radio_mode_name(int mode);
long radio_default_bw(int mode);
long radio_next_bw(int mode, long bw_hz); /* the mode's next channel width, wrapping */

/* ---------------- in-process RTL-SDR (librtlsdr loaded at run time) ---------------- */
/* Lock-free single-producer (USB thread) / single-consumer (main loop) ring of u8 IQ bytes.
 * When full, new data is dropped and counted: the producer never moves the consumer's tail. */
typedef struct {
    unsigned char *buf;
    size_t size;                    /* power of two */
    _Atomic size_t head, tail;      /* free-running byte counters */
    _Atomic unsigned long dropped;  /* bytes discarded because the ring was full */
} iqring_t;
int iqring_init(iqring_t *r, size_t size); /* size: power of two */
void iqring_free(iqring_t *r);
size_t iqring_write(iqring_t *r, const unsigned char *p, size_t n); /* producer; stores whole IQ pairs */
size_t iqring_read(iqring_t *r, unsigned char *p, size_t n);        /* consumer */
size_t iqring_avail(iqring_t *r);
void iqring_flush(iqring_t *r); /* consumer: discard everything buffered (after a retune) */

typedef struct sdr sdr_t; /* opaque */
#define SDR_USB_BUF_BYTES 16384u /* one USB transfer; callers skip one of these after a retune */
/* Open cfg->rtl_dev at `rate` S/s with cfg ppm/gain/bias_tee and start streaming.
 * NULL on failure, with the reason in err (librtlsdr0 missing, no device, busy...). */
sdr_t *sdr_open(const cfg_t *c, long rate, char *err, size_t n);
void sdr_pause(sdr_t *s);                                /* stop streaming, keep the device open */
int sdr_resume(sdr_t *s, long rate);                     /* (re)start streaming at rate S/s */
int sdr_tune(sdr_t *s, long f_hz);                       /* retune and flush buffered IQ */
size_t sdr_read(sdr_t *s, unsigned char *iq, size_t n); /* non-blocking, u8 IQ pairs */
void sdr_flush(sdr_t *s);
int sdr_alive(const sdr_t *s);                 /* 0 once the stream ended (dongle unplugged) */
unsigned long sdr_dropped(const sdr_t *s);     /* IQ bytes lost to a full ring */
void sdr_close(sdr_t *s);
/* Tuning range of the dongle's tuner; its name is returned ("RTL-SDR" with the R820T's range when s is NULL or
 * the tuner unknown). */
const char *sdr_range(const sdr_t *s, long *lo_hz, long *hi_hz);

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
int spec_add_bins(spectrum_t *s, double f_lo, double bin_hz, const float *db, int n, double decay_db);
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
#define AUDIO_RADIO_MAX 8192   /* radio audio queue, ~0.5 s */
#define AUDIO_RADIO_PRIME 1600 /* 100 ms queued before playing: the dongle's samples arrive in bursts */
typedef struct {
    pid_t pid;
    int fd;
    double t_last, t_retry, phase;
    int nseg, seg_pos;
    struct { float freq, vol; int n; } seg[AUDIO_SEGS];
    int16_t radio[AUDIO_RADIO_MAX]; /* demodulated radio, mixed under the beeps */
    unsigned rhead, rtail;          /* free-running sample counters */
    int rprimed;
} audio_t;
void audio_init(audio_t *a);
void audio_tone(audio_t *a, double freq_hz, double dur_s, double vol); /* queue a tone (freq 0 = silence) */
int audio_render(audio_t *a, short *out, int n);                       /* fill n samples from the queue */
void audio_radio_push(audio_t *a, const int16_t *pcm, int n); /* queue radio audio (oldest dropped when full) */
void audio_radio_clear(audio_t *a);
void audio_poll(audio_t *a, const cfg_t *c, double now);               /* keep the PCM stream fed */
void audio_close(audio_t *a);

/* A sysfs LED blinked once per received frame: driven to the opposite of its resting state, then back. */
typedef struct {
    int fd;            /* -1: no LED (missing or not writable) */
    char rest;         /* '0' or '1': its state when opened, put back after each blink and on close */
    double t_off;      /* end of the current blink (0: none) */
} led_t;
void led_open(led_t *l, const char *path);
void led_blink(led_t *l, double now, double dur_s);
void led_poll(led_t *l, double now);
void led_close(led_t *l);

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
    int why;                 /* TF_WHY_*: reason of the last failed download */
    char host[96];           /* the tile server, for the message */
    char cache_dir[240];
} tilefetch_t;
/* Why downloads fail. DNS_BLOCKED: the server's name resolves only to 127.0.0.1 / ::1 / 0.0.0.0, the answer a
 * DNS filter (Pi-hole, AdGuard, NextDNS, router) gives for a blocked domain. */
enum { TF_WHY_NET, TF_WHY_DNS_FAIL, TF_WHY_DNS_BLOCKED, TF_WHY_HTTP };
int tf_host_blocked(const char *host); /* 1: resolves, but only to loopback / unspecified addresses */
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
void tile_recolor(unsigned char *rgb, int npix, int simple); /* simple: only water and roads */

/* ---------------- application state ---------------- */
enum { ST_IDLE, ST_DECODE, ST_SCAN, ST_LISTEN }; /* LISTEN: tuned, spectrum and audio, no decoder */

/* Frequencies where an RS41 has been decoded this session, with its serial (labels on the spectrum). */
#define MAX_KNOWN 16
#define KNOWN_MATCH_HZ 10000L /* two sondes can sit 10 kHz apart */
#define TUNE_STEP_HZ 10000L   /* Left/Right on the spectrum: the RS41 channel raster */
#define TUNE_BIG_STEP_HZ 100000L /* Up/Down */
#define LIVE_HALF_SPAN_HZ 384000L /* the tuned spectrum shows the frequency +- this */
typedef struct {
    long f_hz;               /* 0 = empty slot */
    char id[20];             /* same size as sonde_t.id */
    double t;                /* last frame */
} known_t;

/* In-process sweep of scan_lo_hz..scan_hi_hz: retune in steps of the central SWEEP_KEEP of the
 * SWEEP_RATE band, drop the samples of the settling time, FFT SWEEP_FRAMES frames per step.
 * Measured on the CardputerZero: 2.048 MS/s keeping 80 % gives a flat floor (step edges -0.2 dB);
 * 2.4 MS/s keeping 85 % saves a step but the edges droop 4 dB, hiding weak sondes there. */
#define SWEEP_RATE 2048000L
#define SWEEP_KEEP 0.80
#define SWEEP_FFT 1024
#define SWEEP_FRAMES 5
#define SWEEP_DECAY_DB 3.0 /* peak-hold decay per pass, ~12 dB/s at 4 passes/s */
/* The tuner's DC offset and LO leakage show up as false peaks within ~50 kHz of every tuning centre (seen on
 * the RTL-SDR Blog V4 at 40 kHz). The sweep ignores that zone and alternates passes shifted by half a step
 * so the neighbouring step measures it; the tuned view interpolates across it. */
#define DC_ZONE_HZ 50000.0
typedef struct {
    sdr_t *sdr;              /* open while sweeping, NULL otherwise */
    fft_t fft;
    int step, nstep;         /* nstep: steps of an even pass; odd passes, shifted half a step, take one more */
    double f_first, f_step;  /* centre of step 0, spacing between step centres */
    size_t skip, have;       /* bytes still to discard after a retune / bytes collected */
    unsigned passes;         /* completed full passes since the sweep started */
    unsigned char buf[2 * SWEEP_FFT * SWEEP_FRAMES];
    float db[SWEEP_FFT];
} sweep_t;
#define MAX_PEAKS 8
/* Scan (S): sweep, try the strongest peak with the decoder, stay on the first one that gives an RS41 frame. */
#define HUNT_SNR_DB 10.0  /* a peak this far above the floor is tried */
#define HUNT_TRY_S 10.0   /* per peak: an RS41's first frame comes ~4 s after tuning */
#define HUNT_SKIP_S 120.0 /* a peak that gave no frame is left alone this long */
#define HUNT_SKIP_N 16
typedef struct {
    cfg_t cfg;
    int state;
    long freq_hz;
    proc_t proc;
    sweep_t sw;
    known_t known[MAX_KNOWN];
    sdr_t *rx;               /* the dongle, open for the session once first used (NULL: not open) */
    sdr_t *dsdr;             /* tuned receiver (listen / decode): dongle at freq_hz - 12 kHz + DDC_RATE/4, else NULL */
    ddc_t ddc;
    radio_t radio;           /* audio of the tuned frequency: mode and channel width */
    int radio_audio;         /* set by the UI while the spectrum is on screen: radio audio and its live spectrum */
    spectrum_t live;         /* spectrum of the decode window (+-DDC_RATE/2 around the dongle) */
    double t_live;
    double t_restart;
    sonde_t sonde;
    int have_sonde;
    sonde_t nofix;           /* last frame from a sonde without a GPS solution (t_rx = 0: none) */
    unsigned frames;
    double vv_s; /* smoothed vertical velocity */
    gps_t gps;
    heading_t hdg;
    magcal_t cal; /* compass calibration in progress (Info, K) */
    wind_t wind;
    pred_t pred;
    spectrum_t spec;
    long cursor_hz;          /* band overview cursor */
    int hunt;                /* Scan is choosing and trying peaks; any tuning by hand ends it */
    unsigned hunt_pass;      /* sweep pass whose peaks were last looked at */
    double t_try;            /* when the decode of the peak being tried began */
    struct { long f_hz; double t; } skip[HUNT_SKIP_N]; /* peaks that gave no frame */
    int have_geo; /* dist/brg/elev valid */
    double dist, brg, elev, rel;
    int have_land;
    double land_dist, land_brg;
    FILE *log;
    char log_path[256];
    FILE *rlog;              /* this sonde's auto_rx-format log */
    char rlog_id[20];
    char status[96];         /* fits a receiver error (sdr_open writes up to 96 bytes) */
    hist_t hist;
    flight_t flight;
    prox_t prox;
    audio_t audio;
    led_t led;
    int frame_cue;   /* set by the UI on the Track and Map screens: each frame ticks (unless muted) and blinks the LED */
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
void app_scan(app_t *a);  /* band overview only */
void app_hunt(app_t *a);  /* Scan: band overview that tries each peak until an RS41 decodes */
int app_skipped(const app_t *a, long f_hz, double now); /* tried lately without a frame */
void app_stop(app_t *a);
int app_compass_save(app_t *a); /* 0 = saved to <data_dir>/compass.conf */
void app_listen(app_t *a, long f_hz); /* tune without decoding: spectrum and radio audio; app_tune sets the decode */
const known_t *app_known(const app_t *a, long f_hz); /* NULL: nothing known about this frequency */
void app_free(app_t *a);
void app_on_frame(app_t *a, const char *json_line, double now); /* one decoder output line */

/* Simulated flight (cfg.demo): ascent at 5 m/s to 31.2 km, burst, parachute descent, landing. */
void demo_poll(app_t *a, double now);
void demo_advance(app_t *a, double virtual_seconds, double now);

#endif
