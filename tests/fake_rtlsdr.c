/* Stand-in for librtlsdr.so.0 in the tests (cfg rtlsdr_lib points here): no USB. read_async streams u8 IQ
 * in real time with a little noise and one carrier at FAKE_CARRIER_HZ, placed wherever the "dongle" is tuned
 * and sampling, so scanning and decoding run end to end on any machine. */
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define FAKE_CARRIER_HZ 403000000.0

typedef struct rtlsdr_dev {
    int unused;
} rtlsdr_dev_t;
typedef void (*rtl_cb_t)(unsigned char *buf, uint32_t len, void *ctx);

static rtlsdr_dev_t dev;
static _Atomic uint32_t freq, rate = 2048000;
static _Atomic int cancelled;

uint32_t rtlsdr_get_device_count(void) { return 1; }
int rtlsdr_open(rtlsdr_dev_t **d, uint32_t index) { (void)index; *d = &dev; return 0; }
int rtlsdr_close(rtlsdr_dev_t *d) { (void)d; return 0; }
int rtlsdr_set_sample_rate(rtlsdr_dev_t *d, uint32_t r) { (void)d; rate = r; return 0; }
int rtlsdr_set_center_freq(rtlsdr_dev_t *d, uint32_t f) { (void)d; freq = f; return 0; }
int rtlsdr_set_freq_correction(rtlsdr_dev_t *d, int ppm) { (void)d; (void)ppm; return 0; }
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *d, int manual) { (void)d; (void)manual; return 0; }
int rtlsdr_set_tuner_gain(rtlsdr_dev_t *d, int gain) { (void)d; (void)gain; return 0; }
int rtlsdr_set_bias_tee(rtlsdr_dev_t *d, int on) { (void)d; (void)on; return 0; }
int rtlsdr_reset_buffer(rtlsdr_dev_t *d) { (void)d; return 0; }
int rtlsdr_get_tuner_type(rtlsdr_dev_t *d) { (void)d; return 6; } /* R828D, as on an RTL-SDR Blog V4 */
int rtlsdr_cancel_async(rtlsdr_dev_t *d) { (void)d; cancelled = 1; return 0; }

int rtlsdr_read_async(rtlsdr_dev_t *d, rtl_cb_t cb, void *ctx, uint32_t buf_num, uint32_t buf_len)
{
    static unsigned char buf[16384];
    double ph = 0;
    uint32_t i, len = buf_len && buf_len < sizeof buf ? buf_len : sizeof buf;
    (void)d;
    (void)buf_num;
    cancelled = 0;
    while (!cancelled) {
        double r = rate, off = FAKE_CARRIER_HZ - (double)freq, step = 2.0 * M_PI * off / r;
        double amp = fabs(off) < r / 2 ? 40.0 : 0.0;
        for (i = 0; i < len / 2; i++, ph += step) {
            buf[2 * i] = (unsigned char)lround(127.5 + amp * cos(ph) + (rand() % 7 - 3));
            buf[2 * i + 1] = (unsigned char)lround(127.5 + amp * sin(ph) + (rand() % 7 - 3));
        }
        ph = fmod(ph, 2.0 * M_PI);
        cb(buf, len, ctx);
        usleep((useconds_t)((double)len / 2.0 / r * 1e6)); /* real time */
    }
    return 0;
}
