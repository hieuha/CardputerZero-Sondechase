/* Listening on the tuned frequency: FM demodulator over the DDC's 48 kHz output (see fm_t in sc.h). */
#include "sc.h"
#include <math.h>
#include <string.h>

#define FM_CUTOFF_HZ 8000.0 /* RS41: 4800 Bd GFSK, +-2.4 kHz deviation, Carson bandwidth ~+-7 kHz */
#define FM_GAIN 30000.0f    /* radians -> PCM: the RS41's 2.4 kHz deviation (0.31 rad/sample) to ~9400 */
#define FM_SQ_WINDOW 960    /* 20 ms of input */
#define FM_SQ_THR 0.3f      /* mean square of the discriminator: ~0.05 rad^2 for a clean RS41, ~0.7 for noise */
#define FM_SQ_FLOOR 0.05f   /* closed squelch: a murmur, so the channel is still heard to be live */

void fm_init(fm_t *f)
{
    const double fc = FM_CUTOFF_HZ / 48000.0, mid = (FM_TAPS - 1) / 2.0;
    double sum = 0;
    int k;
    memset(f, 0, sizeof *f);
    for (k = 0; k < FM_TAPS; k++) { /* Hamming-windowed sinc */
        double x = k - mid, h = x == 0 ? 2.0 * fc : sin(2.0 * M_PI * fc * x) / (M_PI * x);
        h *= 0.54 - 0.46 * cos(2.0 * M_PI * k / (FM_TAPS - 1));
        f->taps[k] = (float)h;
        sum += h;
    }
    for (k = 0; k < FM_TAPS; k++) f->taps[k] = (float)(f->taps[k] / sum);
    f->gain = FM_SQ_FLOOR;
}

int fm_process(fm_t *f, const int16_t *iq, size_t npairs, int16_t *out)
{
    size_t p;
    int n = 0, k;
    for (p = 0; p < npairs; p++) {
        float i = iq[2 * p], q = iq[2 * p + 1], si, sq, fi = 0, fq = 0, re, im, d;
        /* the tuned frequency, at +12 kHz = +fs/4, to 0 Hz: multiply by (-j)^n */
        switch (f->phase) {
        case 0: si = i; sq = q; break;
        case 1: si = q; sq = -i; break;
        case 2: si = -i; sq = -q; break;
        default: si = -q; sq = i; break;
        }
        f->phase = (f->phase + 1) & 3;
        f->hi[f->pos] = f->hi[f->pos + FM_TAPS] = si;
        f->hq[f->pos] = f->hq[f->pos + FM_TAPS] = sq;
        f->pos = (f->pos + 1) % FM_TAPS;
        for (k = 0; k < FM_TAPS; k++) { /* oldest sample first */
            fi += f->taps[k] * f->hi[f->pos + k];
            fq += f->taps[k] * f->hq[f->pos + k];
        }
        re = fi * f->pi + fq * f->pq; /* x[n] * conj(x[n-1]) */
        im = fq * f->pi - fi * f->pq;
        f->pi = fi;
        f->pq = fq;
        d = (re == 0 && im == 0) ? 0 : atan2f(im, re);
        f->sq_sum += d * d;
        if (++f->sq_n == FM_SQ_WINDOW) {
            f->open = f->sq_sum / FM_SQ_WINDOW < FM_SQ_THR;
            f->sq_sum = 0;
            f->sq_n = 0;
        }
        f->acc += d;
        if (++f->nacc == 3) { /* 48 kHz -> 16 kHz; the average is the anti-alias filter */
            float v;
            f->gain += ((f->open ? 1.0f : FM_SQ_FLOOR) - f->gain) * 0.01f; /* ~6 ms, no clicks */
            v = f->acc / 3.0f * FM_GAIN * f->gain;
            out[n++] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32767.0f ? -32767.0f : v));
            f->acc = 0;
            f->nacc = 0;
        }
    }
    return n;
}
