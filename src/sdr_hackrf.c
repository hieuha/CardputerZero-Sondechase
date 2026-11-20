/* HackRF back end: libhackrf loaded with dlopen like librtlsdr. libhackrf runs its own USB thread and hands over
 * signed 8-bit IQ in 256 KiB transfers; they become the u8 IQ the rest of the receiver expects on the way into
 * the ring. The HackRF does not sample below 2 MS/s, so a lower rate (the decoder's 1.536 MS/s) runs at twice
 * the rate and is halved with a half-band filter. */
#define _GNU_SOURCE
#include "sdr_priv.h"
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HRF_MIN_RATE 2000000L

/* The subset of libhackrf's API we use; types and prototypes as in hackrf.h (2018.01 and later). hrf_api_t is
 * filled as an array of pointers, so its members must stay in the same order as hrf_names. */
typedef struct hackrf_device hackrf_device;
typedef struct {
    hackrf_device *device;
    uint8_t *buffer;
    int buffer_length;
    int valid_length;
    void *rx_ctx;
    void *tx_ctx;
} hackrf_transfer;
typedef int (*hackrf_cb_t)(hackrf_transfer *t);
typedef struct {
    char **serial_numbers;
    int *usb_board_ids;
    int *usb_device_index;
    int devicecount;
    void **usb_devices;
    int usb_devicecount;
} hackrf_device_list_t;

static const char *const hrf_names[] = {
    "hackrf_init",           "hackrf_exit",          "hackrf_device_list",     "hackrf_device_list_free",
    "hackrf_device_list_open", "hackrf_close",       "hackrf_set_sample_rate", "hackrf_set_baseband_filter_bandwidth",
    "hackrf_compute_baseband_filter_bw", "hackrf_set_freq", "hackrf_set_lna_gain", "hackrf_set_vga_gain",
    "hackrf_set_amp_enable", "hackrf_set_antenna_enable", "hackrf_start_rx",   "hackrf_stop_rx",
    "hackrf_is_streaming",
};
typedef struct {
    int (*init)(void);
    int (*exit)(void);
    hackrf_device_list_t *(*device_list)(void);
    void (*device_list_free)(hackrf_device_list_t *list);
    int (*device_list_open)(hackrf_device_list_t *list, int idx, hackrf_device **dev);
    int (*close)(hackrf_device *dev);
    int (*set_sample_rate)(hackrf_device *dev, double rate);
    int (*set_baseband_filter_bandwidth)(hackrf_device *dev, uint32_t bw_hz);
    uint32_t (*compute_baseband_filter_bw)(uint32_t bw_hz);
    int (*set_freq)(hackrf_device *dev, uint64_t f_hz);
    int (*set_lna_gain)(hackrf_device *dev, uint32_t db);
    int (*set_vga_gain)(hackrf_device *dev, uint32_t db);
    int (*set_amp_enable)(hackrf_device *dev, uint8_t on);
    int (*set_antenna_enable)(hackrf_device *dev, uint8_t on);
    int (*start_rx)(hackrf_device *dev, hackrf_cb_t cb, void *ctx);
    int (*stop_rx)(hackrf_device *dev);
    int (*is_streaming)(hackrf_device *dev);
} hrf_api_t;

typedef struct {
    void *lib;
    hrf_api_t api;
    int inited;
    hackrf_device *dev;
    double corr;              /* 1 + ppm / 1e6: the clock's error, for the frequency and the sample rate */
    long hw_rate;             /* the HackRF's own rate: s->rate, or twice it when halved */
    int halve;
    _Atomic int on;           /* transfers are written to the ring only while set */
    pthread_mutex_t mu;       /* t_tune, and the check-then-write of a transfer against it */
    int have_mu;
    long long t_tune;         /* CLOCK_MONOTONIC ns of the last retune */
    int8_t *ext;              /* HB_KEEP pairs from the previous transfer, then the new one (cap bytes) */
    uint8_t *out;             /* cap bytes */
    size_t cap;
    long long pairs;          /* input pairs halved so far, for the output phase */
    iqring_t *ring;
} hrf_t;

#define HB_KEEP 6 /* the half-band filter spans 7 pairs */

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static uint8_t u8(int v) /* signed sample to rtl-sdr u8 (127.5 is zero) */
{
    v += 128;
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* npairs int8 IQ pairs at e[HB_KEEP*2] onwards, after HB_KEEP kept pairs; one output pair for every second input
 * pair through h = (-1 0 9 16 9 0 -1) / 32 (unity gain at 0 Hz, a null at half the input rate). */
static size_t halve_pairs(hrf_t *h, size_t npairs)
{
    const int8_t *e = h->ext;
    size_t c, k = 0, end = HB_KEEP + npairs - 3;
    /* centre c of the extended buffer is input pair (pairs - HB_KEEP + c); outputs fall on even input pairs */
    for (c = 3 + (size_t)((h->pairs - HB_KEEP + 3) & 1); c < end; c += 2) {
        int j;
        for (j = 0; j < 2; j++) {
            const int8_t *x = e + 2 * c + j;
            int v = 16 * x[0] + 9 * (x[-2] + x[2]) - (x[-6] + x[6]);
            h->out[k++] = u8((v + 16) >> 5);
        }
    }
    memmove(h->ext, h->ext + 2 * npairs, 2 * HB_KEEP);
    h->pairs += (long long)npairs;
    return k;
}

/* libhackrf's thread. A transfer that began before the last retune would put the old frequency's signal at the
 * new one (each holds ~40 ms), so it is dropped. Its start is estimated as now minus its length, with an eighth
 * of that as margin for a callback that runs late. The check and the write hold the lock hrf_tune takes, so a
 * transfer cannot pass the check, then land after the retune's flush. Returning non-zero would end the stream. */
static int on_transfer(hackrf_transfer *t)
{
    hrf_t *h = t->rx_ctx;
    size_t n = (size_t)t->valid_length & ~(size_t)1, i, k;
    long long t_end = now_ns(), dur = (long long)((double)(n / 2) * 1e9 / (double)h->hw_rate);
    if (!atomic_load(&h->on)) return 0;
    if (n > h->cap) { /* libhackrf's transfers are 256 KiB; a larger one only grows the buffers */
        int8_t *e = realloc(h->ext, n + 2 * HB_KEEP);
        uint8_t *o = e ? realloc(h->out, n) : NULL;
        if (e) h->ext = e;
        if (!o) return 0;
        h->out = o;
        h->cap = n;
    }
    if (h->halve) {
        memcpy(h->ext + 2 * HB_KEEP, t->buffer, n);
        k = halve_pairs(h, n / 2);
    } else {
        for (i = 0; i < n; i++) h->out[i] = u8((int8_t)t->buffer[i]);
        k = n;
    }
    pthread_mutex_lock(&h->mu);
    if (t_end - dur >= h->t_tune + dur / 8) iqring_write(h->ring, h->out, k);
    pthread_mutex_unlock(&h->mu);
    return 0;
}

static int hrf_set_rate(sdr_t *s, long rate)
{
    hrf_t *h = s->be;
    long hw = rate < HRF_MIN_RATE ? 2 * rate : rate;
    /* the analog filter at 3/4 of the rate (the next narrower step) keeps aliases out of the band */
    if (h->api.set_sample_rate(h->dev, (double)hw / h->corr) != 0) return -1;
    h->api.set_baseband_filter_bandwidth(h->dev, h->api.compute_baseband_filter_bw((uint32_t)(hw * 3 / 4)));
    h->hw_rate = hw;
    h->halve = hw != rate;
    return 0;
}

/* cfg gain: "" = LNA 32 dB, VGA 24 dB (the HackRF has no AGC); else the total in dB, filled into the LNA
 * (0-40, 8 dB steps) first, then the VGA (0-62, 2 dB steps), then the 14 dB front-end amplifier. */
static void hrf_gain(hrf_t *h, const char *gain)
{
    int g = (int)lround(atof(gain)), lna = 32, vga = 24, amp = 0;
    if (g < 0) g = 0;
    if (gain[0]) {
        lna = g >= 40 ? 40 : g / 8 * 8;
        vga = g - lna >= 62 ? 62 : (g - lna) / 2 * 2;
        amp = g - lna - vga >= 7;
    }
    h->api.set_lna_gain(h->dev, (uint32_t)lna);
    h->api.set_vga_gain(h->dev, (uint32_t)vga);
    h->api.set_amp_enable(h->dev, (uint8_t)amp);
}

static int hrf_open(sdr_t *s, const cfg_t *c, long rate, char *err, size_t n)
{
    hrf_t *h = calloc(1, sizeof *h);
    hackrf_device_list_t *list;
    void **fn;
    size_t i;
    int r;
    if (!(s->be = h)) {
        snprintf(err, n, "out of memory");
        return -1;
    }
    h->ring = &s->ring;
    if (pthread_mutex_init(&h->mu, NULL) != 0) {
        snprintf(err, n, "out of memory");
        return -1;
    }
    h->have_mu = 1;
    h->cap = 1u << 18;
    h->ext = calloc(1, h->cap + 2 * HB_KEEP);
    h->out = malloc(h->cap);
    if (!h->ext || !h->out) {
        snprintf(err, n, "out of memory");
        return -1;
    }
    h->corr = 1.0 + c->ppm * 1e-6;
    if (!(h->lib = dlopen(c->hackrf_lib[0] ? c->hackrf_lib : "libhackrf.so.0", RTLD_NOW | RTLD_LOCAL))) {
        snprintf(err, n, "libhackrf0 not installed");
        return -1;
    }
    for (fn = (void **)&h->api, i = 0; i < sizeof hrf_names / sizeof hrf_names[0]; i++) {
        if (!(fn[i] = dlsym(h->lib, hrf_names[i]))) {
            snprintf(err, n, "libhackrf: missing %s", hrf_names[i]);
            return -1;
        }
    }
    if (h->api.init() != 0) {
        snprintf(err, n, "libhackrf: cannot start libusb");
        return -1;
    }
    h->inited = 1;
    if (!(list = h->api.device_list()) || list->devicecount <= c->rtl_dev) {
        if (list) h->api.device_list_free(list);
        snprintf(err, n, "no HackRF device");
        return -1;
    }
    r = h->api.device_list_open(list, c->rtl_dev, &h->dev);
    h->api.device_list_free(list);
    if (r != 0) {
        h->dev = NULL;
        snprintf(err, n, "cannot open HackRF %d (in use?)", c->rtl_dev);
        return -1;
    }
    if (hrf_set_rate(s, rate) < 0) {
        snprintf(err, n, "sample rate %ld rejected", rate);
        return -1;
    }
    hrf_gain(h, c->gain);
    h->api.set_antenna_enable(h->dev, c->bias_tee ? 1 : 0);
    return 0;
}

static int hrf_tune(sdr_t *s, long f_hz)
{
    hrf_t *h = s->be;
    int r = h->api.set_freq(h->dev, (uint64_t)llround((double)f_hz / h->corr));
    pthread_mutex_lock(&h->mu);
    h->t_tune = now_ns();
    iqring_flush(h->ring); /* this thread is the ring's consumer */
    pthread_mutex_unlock(&h->mu);
    return r != 0 ? -1 : 0;
}

static int hrf_start(sdr_t *s)
{
    hrf_t *h = s->be;
    memset(h->ext, 0, 2 * HB_KEEP);
    h->pairs = 0;
    atomic_store(&h->on, 1);
    if (h->api.start_rx(h->dev, on_transfer, h) != 0) {
        atomic_store(&h->on, 0);
        return -1;
    }
    return 0;
}

static void hrf_stop(sdr_t *s)
{
    hrf_t *h = s->be;
    atomic_store(&h->on, 0); /* older libhackrf can still deliver a transfer after stop_rx returns */
    h->api.stop_rx(h->dev);
}

static int hrf_alive(const sdr_t *s)
{
    const hrf_t *h = s->be;
    return h->api.is_streaming(h->dev) == 1; /* HACKRF_TRUE; an error code once the device is gone */
}

static void hrf_close(sdr_t *s)
{
    hrf_t *h = s->be;
    if (h->dev) h->api.close(h->dev);
    if (h->inited) h->api.exit();
    if (h->lib) dlclose(h->lib);
    if (h->have_mu) pthread_mutex_destroy(&h->mu);
    free(h->ext);
    free(h->out);
    free(h);
    s->be = NULL;
}

static const char *hrf_range(const sdr_t *s, long *lo_hz, long *hi_hz)
{
    (void)s;
    *lo_hz = 1000000L;
    *hi_hz = 6000000000L;
    return "HackRF";
}

const sdr_ops_t sdr_hackrf_ops = {hrf_open, hrf_set_rate, hrf_tune, hrf_start, hrf_stop, hrf_alive, hrf_close, hrf_range};
