/* Stand-in for libhackrf.so.0 in the tests (cfg hackrf_lib points here): no USB. start_rx runs a thread that
 * hands over signed 8-bit IQ in 256 KiB transfers, in real time, with a little noise and one carrier at
 * FAKE_CARRIER_HZ wherever the "HackRF" is tuned and sampling. The fake_hackrf_* getters let the tests see
 * what the receiver asked for. */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define FAKE_CARRIER_HZ 403000000.0
#define FAKE_TRANSFER 262144

typedef struct hackrf_device {
    int unused;
} hackrf_device;
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

static hackrf_device dev;
static hackrf_device_list_t list = {NULL, NULL, NULL, 1, NULL, 1};
static _Atomic double rate = 10e6;
static _Atomic uint64_t freq;
static _Atomic uint32_t bw, lna, vga;
static _Atomic int amp, antenna, streaming, stop;
static hackrf_cb_t cb;
static void *cb_ctx;
static pthread_t thread;

double fake_hackrf_rate(void) { return rate; }
uint32_t fake_hackrf_bw(void) { return bw; }
uint32_t fake_hackrf_lna(void) { return lna; }
uint32_t fake_hackrf_vga(void) { return vga; }
int fake_hackrf_amp(void) { return amp; }
int fake_hackrf_antenna(void) { return antenna; }

int hackrf_init(void) { return 0; }
int hackrf_exit(void) { return 0; }
hackrf_device_list_t *hackrf_device_list(void) { return &list; }
void hackrf_device_list_free(hackrf_device_list_t *l) { (void)l; }
int hackrf_device_list_open(hackrf_device_list_t *l, int idx, hackrf_device **d)
{
    if (idx < 0 || idx >= l->devicecount) return -1;
    *d = &dev;
    return 0;
}
int hackrf_close(hackrf_device *d) { (void)d; return 0; }
int hackrf_set_sample_rate(hackrf_device *d, double r) { (void)d; if (r < 2e6 || r > 20e6) return -2; rate = r; return 0; }
int hackrf_set_baseband_filter_bandwidth(hackrf_device *d, uint32_t b) { (void)d; bw = b; return 0; }
uint32_t hackrf_compute_baseband_filter_bw(uint32_t b) /* the MAX2837's steps, the widest one not above b */
{
    static const uint32_t st[] = {1750000, 2500000, 3500000, 5000000, 5500000, 6000000, 7000000, 8000000};
    size_t i = 0;
    while (i + 1 < sizeof st / sizeof st[0] && st[i + 1] <= b) i++;
    return st[i];
}
int hackrf_set_freq(hackrf_device *d, uint64_t f) { (void)d; freq = f; return 0; }
int hackrf_set_lna_gain(hackrf_device *d, uint32_t v) { (void)d; lna = v; return 0; }
int hackrf_set_vga_gain(hackrf_device *d, uint32_t v) { (void)d; vga = v; return 0; }
int hackrf_set_amp_enable(hackrf_device *d, uint8_t v) { (void)d; amp = v; return 0; }
int hackrf_set_antenna_enable(hackrf_device *d, uint8_t v) { (void)d; antenna = v; return 0; }
int hackrf_is_streaming(hackrf_device *d) { (void)d; return streaming ? 1 : -1003; }

/* Like the hardware, a transfer holds the frequency it was tuned to when it began, and is handed over when its
 * last sample is in: start + its length on the clock. */
static void *rx(void *arg)
{
    static uint8_t buf[FAKE_TRANSFER];
    hackrf_transfer t = {&dev, buf, FAKE_TRANSFER, FAKE_TRANSFER, NULL, NULL};
    double ph = 0;
    struct timespec due;
    (void)arg;
    t.rx_ctx = cb_ctx;
    clock_gettime(CLOCK_MONOTONIC, &due);
    while (!stop) {
        double r = rate, off = FAKE_CARRIER_HZ - (double)freq, step = 2.0 * M_PI * off / r;
        double amp_iq = fabs(off) < r / 2 ? 40.0 : 0.0;
        long ns = (long)(FAKE_TRANSFER / 2 / r * 1e9);
        int i;
        for (i = 0; i < FAKE_TRANSFER / 2; i++, ph += step) {
            buf[2 * i] = (uint8_t)(int8_t)lround(amp_iq * cos(ph) + (rand() % 7 - 3));
            buf[2 * i + 1] = (uint8_t)(int8_t)lround(amp_iq * sin(ph) + (rand() % 7 - 3));
        }
        ph = fmod(ph, 2.0 * M_PI);
        due.tv_nsec += ns;
        due.tv_sec += due.tv_nsec / 1000000000L;
        due.tv_nsec %= 1000000000L;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL);
        if (!stop && cb(&t) != 0) break;
    }
    return NULL;
}

int hackrf_start_rx(hackrf_device *d, hackrf_cb_t c, void *ctx)
{
    (void)d;
    if (streaming) return -1000; /* HACKRF_ERROR_BUSY */
    cb = c;
    cb_ctx = ctx;
    stop = 0;
    if (pthread_create(&thread, NULL, rx, NULL) != 0) return -1;
    streaming = 1;
    return 0;
}

int hackrf_stop_rx(hackrf_device *d)
{
    (void)d;
    if (!streaming) return 0;
    stop = 1;
    pthread_join(thread, NULL);
    streaming = 0;
    return 0;
}
