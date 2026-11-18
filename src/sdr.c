/* In-process RTL-SDR: librtlsdr loaded with dlopen (no build-time dependency, so the cross build
 * needs no librtlsdr in the sysroot), one USB reader thread, and a lock-free IQ ring for the main loop. */
#define _GNU_SOURCE
#include "sc.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
    "rtlsdr_reset_buffer",     "rtlsdr_read_async",    "rtlsdr_cancel_async",
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
} rtl_api_t;

struct sdr {
    void *lib;
    rtl_api_t api;
    rtlsdr_dev_t *dev;
    pthread_t thread;
    int have_thread;
    _Atomic int alive;
    iqring_t ring;
};

#define SDR_RING_BYTES (4u << 20) /* ~1.4 s of IQ at 1.536 MS/s */
#define SDR_USB_BUFS 32

static void on_samples(unsigned char *buf, uint32_t len, void *ctx)
{
    iqring_write(&((sdr_t *)ctx)->ring, buf, len);
}

static void *reader(void *arg)
{
    sdr_t *s = arg;
    /* Small USB transfers (16 KiB = 4 ms at 2.048 MS/s; the library default is 256 KiB = 64 ms) so
     * little pre-retune data is still in flight when a sweep step starts. Returns when cancelled or
     * when the dongle is gone. */
    s->api.read_async(s->dev, on_samples, s, SDR_USB_BUFS, SDR_USB_BUF_BYTES);
    atomic_store(&s->alive, 0);
    return NULL;
}

static int load_api(sdr_t *s, char *err, size_t n)
{
    void **fn = (void **)&s->api;
    size_t i;
    s->lib = dlopen("librtlsdr.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!s->lib) {
        snprintf(err, n, "librtlsdr0 not installed");
        return -1;
    }
    for (i = 0; i < sizeof rtl_names / sizeof rtl_names[0]; i++) {
        fn[i] = dlsym(s->lib, rtl_names[i]);
        if (!fn[i]) {
            snprintf(err, n, "librtlsdr: missing %s", rtl_names[i]);
            return -1;
        }
    }
    return 0;
}

sdr_t *sdr_open(const cfg_t *c, long rate, char *err, size_t n)
{
    sdr_t *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(err, n, "out of memory");
        return NULL;
    }
    if (load_api(s, err, n) < 0) goto fail;
    if (s->api.get_device_count() == 0) {
        snprintf(err, n, "no RTL-SDR device");
        goto fail;
    }
    if (s->api.open(&s->dev, (uint32_t)c->rtl_dev) < 0) {
        s->dev = NULL;
        snprintf(err, n, "cannot open RTL-SDR %d (in use?)", c->rtl_dev);
        goto fail;
    }
    if (s->api.set_sample_rate(s->dev, (uint32_t)rate) < 0) {
        snprintf(err, n, "sample rate %ld rejected", rate);
        goto fail;
    }
    if (c->ppm) s->api.set_freq_correction(s->dev, c->ppm);
    if (c->gain[0]) {
        s->api.set_tuner_gain_mode(s->dev, 1);
        s->api.set_tuner_gain(s->dev, (int)(atof(c->gain) * 10.0 + 0.5)); /* driver picks the nearest step */
    } else {
        s->api.set_tuner_gain_mode(s->dev, 0);
    }
    s->api.set_bias_tee(s->dev, c->bias_tee ? 1 : 0);
    s->api.reset_buffer(s->dev);
    if (iqring_init(&s->ring, SDR_RING_BYTES) < 0) {
        snprintf(err, n, "out of memory");
        goto fail;
    }
    atomic_store(&s->alive, 1);
    if (pthread_create(&s->thread, NULL, reader, s) != 0) {
        snprintf(err, n, "cannot start the USB thread");
        goto fail;
    }
    s->have_thread = 1;
    return s;
fail:
    sdr_close(s);
    return NULL;
}

int sdr_tune(sdr_t *s, long f_hz)
{
    int r = s->api.set_center_freq(s->dev, (uint32_t)f_hz);
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
    return atomic_load(&((sdr_t *)s)->alive);
}

unsigned long sdr_dropped(const sdr_t *s)
{
    return atomic_load(&((sdr_t *)s)->ring.dropped);
}

void sdr_close(sdr_t *s)
{
    if (!s) return;
    if (s->have_thread) {
        s->api.cancel_async(s->dev);
        pthread_join(s->thread, NULL);
    }
    if (s->dev) s->api.close(s->dev);
    if (s->lib) dlclose(s->lib);
    iqring_free(&s->ring);
    free(s);
}
