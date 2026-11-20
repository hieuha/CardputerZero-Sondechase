/* Receiver back ends behind sdr_t: each one loads its vendor library with dlopen (no build-time dependency) and
 * streams u8 IQ pairs (rtl-sdr format, centre 127.5) into the shared ring. sdr.c holds the ring, the generic
 * sdr_* calls and the librtlsdr back end; sdr_hackrf.c the libhackrf one. A new receiver is one more sdr_ops_t. */
#ifndef SDR_PRIV_H
#define SDR_PRIV_H
#include "sc.h"

typedef struct sdr_ops {
    /* Load the library, open cfg rtl_dev at rate S/s, apply ppm, gain and bias_tee; do not stream yet.
     * On failure the reason goes to err and s->be may be partly set up: close() cleans it. */
    int (*open)(sdr_t *s, const cfg_t *c, long rate, char *err, size_t n);
    int (*set_rate)(sdr_t *s, long rate); /* only while stopped */
    int (*tune)(sdr_t *s, long f_hz);     /* IQ sampled before the retune must not reach the ring afterwards */
    int (*start)(sdr_t *s);               /* stream into s->ring */
    void (*stop)(sdr_t *s);               /* returns once nothing more is written to the ring */
    int (*alive)(const sdr_t *s);         /* 0 once the stream ended by itself (device unplugged) */
    void (*close)(sdr_t *s);              /* close the device, unload the library, free s->be */
    const char *(*range)(const sdr_t *s, long *lo_hz, long *hi_hz);
} sdr_ops_t;

struct sdr {
    const sdr_ops_t *ops;
    void *be;      /* the back end's own state */
    long rate;     /* S/s as delivered to the ring */
    int streaming;
    iqring_t ring;
};

#define SDR_RING_BYTES (4u << 20) /* ~1.4 s of IQ at 1.536 MS/s */

extern const sdr_ops_t sdr_rtlsdr_ops, sdr_hackrf_ops;
#endif
