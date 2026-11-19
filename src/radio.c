/* Listening on the tuned frequency: NFM / AM / USB / LSB demodulator over the DDC's 48 kHz output
 * (see radio_t in sc.h). */
#include "sc.h"
#include <math.h>
#include <string.h>

#define RADIO_RATE 48000.0
#define FM_GAIN 30000.0f   /* radians -> PCM: an RS41's 2.4 kHz deviation (0.31 rad/sample) to ~9400 */
#define AM_GAIN 20000.0f   /* modulation depth -> PCM */
#define SSB_GAIN 20000.0f  /* envelope-normalised sideband -> PCM */
#define SSB_FLOOR 30.0f    /* AGC: largest gain, so a quiet channel stays quiet */
#define SQ_WINDOW 960      /* NFM squelch: 20 ms of input */
#define SQ_FLOOR 0.05f     /* closed squelch: a murmur, so the channel is still heard to be live */

#define WFM_GAIN 8000.0f   /* radians at 192 kHz -> PCM: 75 kHz deviation (2.45 rad/sample) to ~20000 */
#define WFM_DECIM 12       /* 192 kHz -> 16 kHz */

static const char *const mode_names[DEMOD_N] = {"NFM", "AM", "USB", "LSB", "WFM"};
/* Channel widths per mode, first = default. NFM: 16 kHz fits an RS41 (4800 Bd GFSK, +-2.4 kHz deviation). */
static const long bw_nfm[] = {16000, 25000, 6000, 12000}, bw_am[] = {6000, 10000}, bw_ssb[] = {2400, 3000};
static const long bw_wfm[] = {180000}; /* shown only: the 192 kHz stream is the channel */
/* NFM squelch threshold per width, between the discriminator's mean square for a carrier with RS41-like
 * deviation (~0.05 rad^2 at every width) and for noise alone through that filter (0.13, 0.44, 0.73, 1.39). */
static const struct { long bw; float thr; } sq_thr[] = {{6000, 0.08f}, {12000, 0.20f}, {16000, 0.30f}, {25000, 0.60f}};

static const long *bw_list(int mode, int *n)
{
    if (mode == DEMOD_NFM) { *n = 4; return bw_nfm; }
    if (mode == DEMOD_AM) { *n = 2; return bw_am; }
    if (mode == DEMOD_WFM) { *n = 1; return bw_wfm; }
    *n = 2;
    return bw_ssb;
}

const char *radio_mode_name(int mode)
{
    return mode >= 0 && mode < DEMOD_N ? mode_names[mode] : "?";
}

long radio_default_bw(int mode)
{
    int n;
    return bw_list(mode, &n)[0];
}

long radio_next_bw(int mode, long bw_hz)
{
    int n, i;
    const long *l = bw_list(mode, &n);
    for (i = 0; i < n; i++)
        if (l[i] == bw_hz) return l[(i + 1) % n];
    return l[0];
}

void radio_init(radio_t *r, int mode, long bw_hz)
{
    /* SSB: a low-pass half the width, moved up (USB) or down (LSB) by half the width: one sideband, 0..bw */
    const double half = bw_hz / 2.0, fc = half / RADIO_RATE, mid = (RADIO_TAPS - 1) / 2.0;
    const double shift = mode == DEMOD_USB ? half : (mode == DEMOD_LSB ? -half : 0.0);
    double sum = 0;
    int k;
    memset(r, 0, sizeof *r);
    r->mode = mode;
    r->bw_hz = bw_hz;
    for (k = 0; k < RADIO_TAPS; k++) { /* Blackman-windowed sinc */
        double x = k - mid, h = x == 0 ? 2.0 * fc : sin(2.0 * M_PI * fc * x) / (M_PI * x);
        h *= 0.42 - 0.5 * cos(2.0 * M_PI * k / (RADIO_TAPS - 1)) + 0.08 * cos(4.0 * M_PI * k / (RADIO_TAPS - 1));
        r->tr[k] = (float)h;
        sum += h;
    }
    for (k = 0; k < RADIO_TAPS; k++) {
        /* taps are applied oldest sample first, i.e. time-reversed: hence -shift */
        double h = r->tr[k] / sum, a = -2.0 * M_PI * shift * (k - mid) / RADIO_RATE;
        r->tr[k] = (float)(h * cos(a));
        r->ti[k] = (float)(h * sin(a));
    }
    r->sq_thr = sq_thr[2].thr;
    for (k = 0; k < (int)(sizeof sq_thr / sizeof sq_thr[0]); k++)
        if (sq_thr[k].bw == bw_hz) r->sq_thr = sq_thr[k].thr;
    r->gain = mode == DEMOD_NFM ? SQ_FLOOR : 1.0f; /* WFM: no squelch, a station is either there or hiss */
    r->open = mode != DEMOD_NFM;
}

/* One demodulated 48 kHz sample from the filtered channel (fi, fq). */
static float demod(radio_t *r, float fi, float fq)
{
    float v;
    if (r->mode == DEMOD_NFM) {
        float re = fi * r->pi + fq * r->pq, im = fq * r->pi - fi * r->pq; /* x[n] * conj(x[n-1]) */
        float d = (re == 0 && im == 0) ? 0 : atan2f(im, re);
        r->pi = fi;
        r->pq = fq;
        r->sq_sum += d * d;
        if (++r->sq_n == SQ_WINDOW) {
            r->open = r->sq_sum / SQ_WINDOW < r->sq_thr;
            r->sq_sum = 0;
            r->sq_n = 0;
        }
        return d * FM_GAIN;
    }
    if (r->mode == DEMOD_AM) { /* envelope around the carrier level, which also sets the gain */
        float env = sqrtf(fi * fi + fq * fq);
        r->level += (env - r->level) * 0.0005f; /* ~40 ms */
        return r->level > 1.0f ? (env - r->level) / r->level * AM_GAIN : 0.0f;
    }
    v = fi; /* SSB: the sideband filter left one side, its real part is the audio */
    r->level = fabsf(v) > r->level ? fabsf(v) : r->level * 0.9998f; /* AGC: fast attack, ~100 ms decay */
    return v / (r->level > SSB_FLOOR ? r->level : SSB_FLOOR) * SSB_GAIN;
}

int radio_process(radio_t *r, const int16_t *iq, size_t npairs, int16_t *out)
{
    size_t p;
    int n = 0, k;
    for (p = 0; p < npairs; p++) {
        float i = iq[2 * p], q = iq[2 * p + 1], si, sq, fi = 0, fq = 0, v;
        const float *hi, *hq;
        /* the tuned frequency, at +12 kHz = +fs/4, to 0 Hz: multiply by (-j)^n */
        switch (r->phase) {
        case 0: si = i; sq = q; break;
        case 1: si = q; sq = -i; break;
        case 2: si = -i; sq = -q; break;
        default: si = -q; sq = i; break;
        }
        r->phase = (r->phase + 1) & 3;
        r->hi[r->pos] = r->hi[r->pos + RADIO_TAPS] = si;
        r->hq[r->pos] = r->hq[r->pos + RADIO_TAPS] = sq;
        r->pos = (r->pos + 1) % RADIO_TAPS;
        hi = r->hi + r->pos; /* oldest sample first */
        hq = r->hq + r->pos;
        for (k = 0; k < RADIO_TAPS; k++) { /* complex taps times complex samples */
            fi += r->tr[k] * hi[k] - r->ti[k] * hq[k];
            fq += r->tr[k] * hq[k] + r->ti[k] * hi[k];
        }
        r->acc += demod(r, fi, fq);
        if (++r->nacc == 3) { /* 48 kHz -> 16 kHz; the average is the anti-alias filter */
            r->gain += ((r->open ? 1.0f : SQ_FLOOR) - r->gain) * 0.01f; /* squelch ramps in ~6 ms, no clicks */
            v = r->acc / 3.0f * r->gain;
            out[n++] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32767.0f ? -32767.0f : v));
            r->acc = 0;
            r->nacc = 0;
        }
    }
    return n;
}

int radio_process_wfm(radio_t *r, const int16_t *iq, size_t npairs, int16_t *out)
{
    /* the tuned frequency, at +12 kHz = +fs/16, to 0 Hz */
    static const float c16[16] = {1, 0.92388f, 0.70711f, 0.38268f, 0, -0.38268f, -0.70711f, -0.92388f,
                                  -1, -0.92388f, -0.70711f, -0.38268f, 0, 0.38268f, 0.70711f, 0.92388f};
    const float a_de = 0.0989f; /* 1 - exp(-1 / (192 kHz * 50 us)): de-emphasis, 3.2 kHz */
    const float a_lp = 0.2788f; /* 1 - exp(-2 pi 10 kHz / 192 kHz): with the average below, keeps the 19 kHz pilot out */
    size_t p;
    int n = 0;
    for (p = 0; p < npairs; p++) {
        float i = iq[2 * p], q = iq[2 * p + 1], c = c16[r->phase], s = -c16[(r->phase + 4) & 15];
        float si = i * c + q * s, sq = q * c - i * s; /* times exp(-j 2 pi n / 16) */
        float re = si * r->pi + sq * r->pq, im = sq * r->pi - si * r->pq, v;
        r->phase = (r->phase + 1) & 15;
        r->pi = si;
        r->pq = sq;
        r->deemph += (((re == 0 && im == 0) ? 0 : atan2f(im, re)) - r->deemph) * a_de;
        r->lp += (r->deemph - r->lp) * a_lp;
        r->acc += r->lp;
        if (++r->nacc == WFM_DECIM) {
            v = r->acc / WFM_DECIM * WFM_GAIN;
            out[n++] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32767.0f ? -32767.0f : v));
            r->acc = 0;
            r->nacc = 0;
        }
    }
    return n;
}
