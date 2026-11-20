/* In-process receiver: the IQ ring, the generic sdr_* calls that dispatch to a back end (sdr_priv.h), and the
 * RTL-SDR back end: librtlsdr loaded with dlopen (no build-time dependency, so the cross build needs no
 * librtlsdr in the sysroot) and one USB reader thread. */
#define _GNU_SOURCE
#include "sdr_priv.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------- IQ ring ---------------- */
int iqring_init(iqring_t *r, size_t size)
{
    memset(r, 0, sizeof *r);
    if (size < 2 || (size & (size - 1))) return -1;
    r->buf = malloc(size);
    if (!r->buf) return -1;
    r->size = size;
    return 0;
}

void iqring_free(iqring_t *r)
{
    free(r->buf);
    r->buf = NULL;
    r->size = 0;
}

size_t iqring_avail(iqring_t *r)
{
    return atomic_load_explicit(&r->head, memory_order_acquire) - atomic_load_explicit(&r->tail, memory_order_relaxed);
}

size_t iqring_write(iqring_t *r, const unsigned char *p, size_t n)
{
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    size_t room = r->size - (head - tail), k = (n < room ? n : room) & ~(size_t)1, at = head & (r->size - 1);
    size_t first = k < r->size - at ? k : r->size - at;
    memcpy(r->buf + at, p, first);
    memcpy(r->buf, p + first, k - first);
    atomic_store_explicit(&r->head, head + k, memory_order_release);
    if (k < n) atomic_fetch_add_explicit(&r->dropped, n - k, memory_order_relaxed);
    return k;
}

size_t iqring_read(iqring_t *r, unsigned char *p, size_t n)
{
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t k = head - tail < n ? head - tail : n, at = tail & (r->size - 1);
    size_t first = k < r->size - at ? k : r->size - at;
    memcpy(p, r->buf + at, first);
    memcpy(p + first, r->buf, k - first);
    atomic_store_explicit(&r->tail, tail + k, memory_order_release);
    return k;
}

void iqring_flush(iqring_t *r)
{
    atomic_store_explicit(&r->tail, atomic_load_explicit(&r->head, memory_order_acquire), memory_order_release);
}

/* ---------------- librtlsdr ---------------- */
typedef struct rtlsdr_dev rtlsdr_dev_t;
typedef void (*rtl_cb_t)(unsigned char *buf, uint32_t len, void *ctx);

/* The subset of librtlsdr's API we use; prototypes as in rtl-sdr.h (2.0.x). rtl_api_t is filled
 * as an array of pointers, so its members must stay in the same order as rtl_names. */
static const char *const rtl_names[] = {
    "rtlsdr_get_device_count", "rtlsdr_open",          "rtlsdr_close",
    "rtlsdr_set_sample_rate",  "rtlsdr_set_center_freq", "rtlsdr_set_freq_correction",
    "rtlsdr_set_tuner_gain_mode", "rtlsdr_set_tuner_gain", "rtlsdr_set_bias_tee",
    "rtlsdr_reset_buffer",     "rtlsdr_read_async",    "rtlsdr_cancel_async",  "rtlsdr_get_tuner_type",
};
typedef struct {
    uint32_t (*get_device_count)(void);
    int (*open)(rtlsdr_dev_t **dev, uint32_t index);
    int (*close)(rtlsdr_dev_t *dev);
    int (*set_sample_rate)(rtlsdr_dev_t *dev, uint32_t rate);
    int (*set_center_freq)(rtlsdr_dev_t *dev, uint32_t freq);
    int (*set_freq_correction)(rtlsdr_dev_t *dev, int ppm);
    int (*set_tuner_gain_mode)(rtlsdr_dev_t *dev, int manual);
    int (*set_tuner_gain)(rtlsdr_dev_t *dev, int tenth_db);
    int (*set_bias_tee)(rtlsdr_dev_t *dev, int on);
    int (*reset_buffer)(rtlsdr_dev_t *dev);
    int (*read_async)(rtlsdr_dev_t *dev, rtl_cb_t cb, void *ctx, uint32_t buf_num, uint32_t buf_len);
    int (*cancel_async)(rtlsdr_dev_t *dev);
    int (*get_tuner_type)(rtlsdr_dev_t *dev);
} rtl_api_t;

typedef struct {
    void *lib;
    rtl_api_t api;
    rtlsdr_dev_t *dev;
    int tuner;                  /* rtlsdr_tuner: 0 unknown, 1 E4000, 2 FC0012, 3 FC0013, 4 FC2580, 5 R820T, 6 R828D */
    pthread_t thread;
    int have_thread;
    _Atomic int alive;
    iqring_t *ring;
} rtl_t;

#define SDR_USB_BUFS 32

static void on_samples(unsigned char *buf, uint32_t len, void *ctx)
{
    iqring_write(((rtl_t *)ctx)->ring, buf, len);
}

static void *reader(void *arg)
{
    rtl_t *r = arg;
    /* Small USB transfers (16 KiB = 4 ms at 2.048 MS/s; the library default is 256 KiB = 64 ms) so
     * little pre-retune data is still in flight when a sweep step starts. Returns when cancelled or
     * when the dongle is gone. */
    r->api.read_async(r->dev, on_samples, r, SDR_USB_BUFS, SDR_USB_BUF_BYTES);
    atomic_store(&r->alive, 0);
    return NULL;
}

static int rtl_open(sdr_t *s, const cfg_t *c, long rate, char *err, size_t n)
{
    rtl_t *r = calloc(1, sizeof *r);
    void **fn;
    size_t i;
    if (!(s->be = r)) {
        snprintf(err, n, "out of memory");
        return -1;
    }
    r->ring = &s->ring;
    if (!(r->lib = dlopen(c->rtlsdr_lib[0] ? c->rtlsdr_lib : "librtlsdr.so.0", RTLD_NOW | RTLD_LOCAL))) {
        snprintf(err, n, "librtlsdr0 not installed");
        return -1;
    }
    for (fn = (void **)&r->api, i = 0; i < sizeof rtl_names / sizeof rtl_names[0]; i++) {
        if (!(fn[i] = dlsym(r->lib, rtl_names[i]))) {
            snprintf(err, n, "librtlsdr: missing %s", rtl_names[i]);
            return -1;
        }
    }
    if (r->api.get_device_count() == 0) {
        snprintf(err, n, "no RTL-SDR device");
        return -1;
    }
    if (r->api.open(&r->dev, (uint32_t)c->rtl_dev) < 0) {
        r->dev = NULL;
        snprintf(err, n, "cannot open RTL-SDR %d (in use?)", c->rtl_dev);
        return -1;
    }
    if (r->api.set_sample_rate(r->dev, (uint32_t)rate) < 0) {
        snprintf(err, n, "sample rate %ld rejected", rate);
        return -1;
    }
    r->tuner = r->api.get_tuner_type(r->dev);
    if (c->ppm) r->api.set_freq_correction(r->dev, c->ppm);
    if (c->gain[0]) {
        r->api.set_tuner_gain_mode(r->dev, 1);
        r->api.set_tuner_gain(r->dev, (int)(atof(c->gain) * 10.0 + 0.5)); /* driver picks the nearest step */
    } else {
        r->api.set_tuner_gain_mode(r->dev, 0);
    }
    r->api.set_bias_tee(r->dev, c->bias_tee ? 1 : 0);
    return 0;
}

static int rtl_set_rate(sdr_t *s, long rate)
{
    rtl_t *r = s->be;
    return r->api.set_sample_rate(r->dev, (uint32_t)rate) < 0 ? -1 : 0;
}

/* Pre-retune transfers still in flight are small (SDR_USB_BUF_BYTES); the callers skip one after a retune. */
static int rtl_tune(sdr_t *s, long f_hz)
{
    rtl_t *r = s->be;
    return r->api.set_center_freq(r->dev, (uint32_t)f_hz) < 0 ? -1 : 0;
}

static int rtl_start(sdr_t *s)
{
    rtl_t *r = s->be;
    r->api.reset_buffer(r->dev);
    atomic_store(&r->alive, 1);
    if (pthread_create(&r->thread, NULL, reader, r) != 0) return -1;
    r->have_thread = 1;
    return 0;
}

static void rtl_stop(sdr_t *s)
{
    rtl_t *r = s->be;
    if (!r->have_thread) return;
    /* A cancel that lands before the thread has entered read_async is ignored (librtlsdr returns -2 while
     * the stream is not running), and a single attempt would then leave the join waiting forever. */
    while (atomic_load(&r->alive)) {
        r->api.cancel_async(r->dev);
        usleep(1000);
    }
    pthread_join(r->thread, NULL);
    r->have_thread = 0;
}

static int rtl_alive(const sdr_t *s)
{
    return atomic_load(&((rtl_t *)s->be)->alive);
}

static void rtl_close(sdr_t *s)
{
    rtl_t *r = s->be;
    if (r->dev) r->api.close(r->dev);
    if (r->lib) dlclose(r->lib);
    free(r);
    s->be = NULL;
}

/* The tuner's frequency range, from the manufacturers' data sheets as librtlsdr uses them (E4000: with its
 * 1100-1250 MHz gap; FC2580: up to its upper band); unknown tuners get the R820T's. */
static const char *rtl_range(const sdr_t *s, long *lo_hz, long *hi_hz)
{
    static const struct { const char *name; long lo, hi; } t[] = {
        {"RTL-SDR", 24000000L, 1766000000L},  {"E4000", 52000000L, 2200000000L},
        {"FC0012", 22000000L, 948600000L},    {"FC0013", 22000000L, 1100000000L},
        {"FC2580", 146000000L, 924000000L},   {"R820T", 24000000L, 1766000000L},
        {"R828D", 24000000L, 1766000000L},
    };
    int tuner = s && s->be ? ((const rtl_t *)s->be)->tuner : 0;
    int i = tuner > 0 && tuner < (int)(sizeof t / sizeof t[0]) ? tuner : 0;
    *lo_hz = t[i].lo;
    *hi_hz = t[i].hi;
    return t[i].name;
}

const sdr_ops_t sdr_rtlsdr_ops = {rtl_open, rtl_set_rate, rtl_tune, rtl_start, rtl_stop, rtl_alive, rtl_close, rtl_range};

/* ---------------- generic receiver ---------------- */
/* cfg sdr: "rtlsdr", "hackrf", or "auto" (default) for whichever is on USB, RTL-SDR first. */
static const sdr_ops_t *sdr_pick(const cfg_t *c)
{
    if (!strcmp(c->sdr, "hackrf")) return &sdr_hackrf_ops;
    if (!strcmp(c->sdr, "rtlsdr")) return &sdr_rtlsdr_ops;
    return sdr_present("/sys/bus/usb/devices") == SDR_HACKRF ? &sdr_hackrf_ops : &sdr_rtlsdr_ops;
}

sdr_t *sdr_open(const cfg_t *c, long rate, char *err, size_t n)
{
    sdr_t *s = calloc(1, sizeof *s);
    if (!s || iqring_init(&s->ring, SDR_RING_BYTES) < 0) {
        snprintf(err, n, "out of memory");
        sdr_close(s);
        return NULL;
    }
    s->ops = sdr_pick(c);
    if (s->ops->open(s, c, rate, err, n) < 0) goto fail;
    s->rate = rate;
    if (sdr_resume(s, rate) < 0) {
        snprintf(err, n, "cannot start the receiver stream");
        goto fail;
    }
    return s;
fail:
    sdr_close(s);
    return NULL;
}

void sdr_pause(sdr_t *s)
{
    if (!s->streaming) return;
    s->ops->stop(s);
    s->streaming = 0;
    iqring_flush(&s->ring);
}

/* Switching jobs keeps the device open: stopping the stream and changing the rate takes a fraction of
 * the ~600 ms that closing and reopening the dongle (USB and tuner initialisation) took. */
int sdr_resume(sdr_t *s, long rate)
{
    if (s->streaming && rate == s->rate) return 0;
    sdr_pause(s);
    if (rate != s->rate) {
        if (s->ops->set_rate(s, rate) < 0) return -1;
        s->rate = rate;
    }
    iqring_flush(&s->ring);
    if (s->ops->start(s) < 0) return -1;
    s->streaming = 1;
    return 0;
}

int sdr_tune(sdr_t *s, long f_hz)
{
    int r = s->ops->tune(s, f_hz);
    iqring_flush(&s->ring);
    return r < 0 ? -1 : 0;
}

size_t sdr_read(sdr_t *s, unsigned char *iq, size_t n)
{
    return iqring_read(&s->ring, iq, n);
}

void sdr_flush(sdr_t *s)
{
    iqring_flush(&s->ring);
}

int sdr_alive(const sdr_t *s)
{
    return s->streaming && s->ops->alive(s);
}

unsigned long sdr_dropped(const sdr_t *s)
{
    return atomic_load(&((sdr_t *)s)->ring.dropped);
}

const char *sdr_range(const sdr_t *s, long *lo_hz, long *hi_hz)
{
    return s ? s->ops->range(s, lo_hz, hi_hz) : rtl_range(NULL, lo_hz, hi_hz);
}

void sdr_close(sdr_t *s)
{
    if (!s) return;
    if (s->streaming) sdr_pause(s);
    if (s->be) s->ops->close(s);
    iqring_free(&s->ring);
    free(s);
}
