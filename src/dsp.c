/* DSP on u8 IQ from the RTL-SDR: windowed FFT spectrum for the Scan screen, and the decoder
 * front end (u8 IQ at 1.536 MS/s -> complex int16 at 48 kHz for fsk_demod).
 *
 * The decoder front end (rotate, fifth_order, generic_fir, cic_9_tables) is ported from rtl_fm.c,
 * rtl-sdr 2.0.2, so the decoders see the same stream they got from "rtl_fm -M raw -F9 -s 48000":
 *   Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 *   Copyright (C) 2012 by Hoernchen <la@tfc-server.de>
 *   Copyright (C) 2012 by Kyle Keen <keenerd@gmail.com>
 *   Copyright (C) 2013 by Elias Oenal <EliasOenal@gmail.com>
 *   GNU General Public License version 2 or (at your option) any later version.
 */
#include "sc.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- FFT spectrum ---------------- */
void fft_free(fft_t *f)
{
    free(f->re);
    free(f->im);
    free(f->win);
    free(f->cs);
    free(f->sn);
    free(f->rev);
    memset(f, 0, sizeof *f);
}

int fft_init(fft_t *f, int n)
{
    int i, bits = 0;
    memset(f, 0, sizeof *f);
    if (n < 4 || (n & (n - 1))) return -1;
    while ((1 << bits) < n) bits++;
    f->n = n;
    f->re = malloc(n * sizeof *f->re);
    f->im = malloc(n * sizeof *f->im);
    f->win = malloc(n * sizeof *f->win);
    f->cs = malloc(n / 2 * sizeof *f->cs);
    f->sn = malloc(n / 2 * sizeof *f->sn);
    f->rev = malloc(n * sizeof *f->rev);
    if (!f->re || !f->im || !f->win || !f->cs || !f->sn || !f->rev) {
        fft_free(f);
        return -1;
    }
    for (i = 0; i < n; i++) {
        int r = 0, b;
        for (b = 0; b < bits; b++)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        f->rev[i] = r;
        f->win[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / n));
    }
    for (i = 0; i < n / 2; i++) {
        f->cs[i] = (float)cos(-2.0 * M_PI * i / n);
        f->sn[i] = (float)sin(-2.0 * M_PI * i / n);
    }
    return 0;
}

/* In-place iterative radix-2 on re/im, input already in bit-reversed order. */
static void fft_run(fft_t *f)
{
    int len, i, j, n = f->n;
    for (len = 2; len <= n; len <<= 1) {
        int half = len / 2, step = n / len;
        for (i = 0; i < n; i += len)
            for (j = 0; j < half; j++) {
                float wr = f->cs[j * step], wi = f->sn[j * step];
                int a = i + j, b = a + half;
                float tr = f->re[b] * wr - f->im[b] * wi, ti = f->re[b] * wi + f->im[b] * wr;
                f->re[b] = f->re[a] - tr;
                f->im[b] = f->im[a] - ti;
                f->re[a] += tr;
                f->im[a] += ti;
            }
    }
}

void fft_spectrum(fft_t *f, const unsigned char *iq, int frames, float *db)
{
    int n = f->n, k, i;
    double wsum = 0, ref;
    for (i = 0; i < n; i++) {
        wsum += f->win[i];
        db[i] = 0.0f; /* power accumulator until the end */
    }
    for (k = 0; k < frames; k++, iq += 2 * n) {
        for (i = 0; i < n; i++) {
            int r = f->rev[i];
            f->re[r] = (iq[2 * i] - 127.5f) * f->win[i];
            f->im[r] = (iq[2 * i + 1] - 127.5f) * f->win[i];
        }
        fft_run(f);
        for (i = 0; i < n; i++) db[i] += f->re[i] * f->re[i] + f->im[i] * f->im[i];
    }
    ref = 127.5 * wsum; /* magnitude of a full-scale tone in its bin */
    for (i = 0; i < n; i++) f->re[i] = db[i]; /* reuse re as scratch for the FFT shift */
    for (i = 0; i < n; i++) {
        double p = f->re[(i + n / 2) % n] / (frames > 0 ? frames : 1);
        db[i] = (float)(10.0 * log10(p / (ref * ref) + 1e-20));
    }
}

/* ---------------- decoder front end (from rtl_fm.c) ---------------- */
static const int cic_9_tables[][10] = {
    {0},
    {9, -156, -97, 2798, -15489, 61019, -15489, 2798, -97, -156},
    {9, -128, -568, 5593, -24125, 74126, -24125, 5593, -568, -128},
    {9, -129, -639, 6187, -26281, 77511, -26281, 6187, -639, -129},
    {9, -122, -612, 6082, -26353, 77818, -26353, 6082, -612, -122},
    {9, -120, -602, 6015, -26269, 77757, -26269, 6015, -602, -120},
};
#define DDC_PASSES 5 /* log2(DDC_DECIM) */

/* Half of interleaved data: decimate-by-2 with a [1 5 10 10 5 1]/16 binomial filter, in place. */
static void fifth_order(int16_t *data, int length, int16_t *hist)
{
    int i;
    int16_t a, b, c, d, e, f;
    a = hist[1];
    b = hist[2];
    c = hist[3];
    d = hist[4];
    e = hist[5];
    f = data[0];
    /* a downsample should improve resolution, so don't fully shift */
    data[0] = (int16_t)((a + (b + e) * 5 + (c + d) * 10 + f) >> 4);
    for (i = 4; i < length; i += 4) {
        a = c;
        b = d;
        c = e;
        d = f;
        e = data[i - 2];
        f = data[i];
        data[i / 2] = (int16_t)((a + (b + e) * 5 + (c + d) * 10 + f) >> 4);
    }
    hist[0] = a;
    hist[1] = b;
    hist[2] = c;
    hist[3] = d;
    hist[4] = e;
    hist[5] = f;
}

/* Half of interleaved data: 9-tap symmetric FIR, in place. */
static void generic_fir(int16_t *data, int length, const int *fir, int16_t *hist)
{
    int d, temp, sum;
    for (d = 0; d < length; d += 2) {
        temp = data[d];
        sum = 0;
        sum += (hist[0] + hist[8]) * fir[1];
        sum += (hist[1] + hist[7]) * fir[2];
        sum += (hist[2] + hist[6]) * fir[3];
        sum += (hist[3] + hist[5]) * fir[4];
        sum += hist[4] * fir[5];
        data[d] = (int16_t)(sum >> 15);
        memmove(hist, hist + 1, 8 * sizeof *hist);
        hist[8] = (int16_t)temp;
    }
}

void ddc_init(ddc_t *d)
{
    memset(d, 0, sizeof *d);
}

/* Rotate one u8 IQ pair by j^phase (rtl_fm's rotate_90, with uint8 negation 255 - x) and
 * centre it the way rtl_fm does (x - 127). */
static void rotate_pair(unsigned phase, unsigned char i, unsigned char q, int16_t *out)
{
    unsigned char ri, rq;
    switch (phase) {
    case 0: ri = i; rq = q; break;
    case 1: ri = (unsigned char)(255 - q); rq = i; break;
    case 2: ri = (unsigned char)(255 - i); rq = (unsigned char)(255 - q); break;
    default: ri = q; rq = (unsigned char)(255 - i); break;
    }
    out[0] = (int16_t)(ri - 127);
    out[1] = (int16_t)(rq - 127);
}

size_t ddc_process(ddc_t *d, const unsigned char *iq, size_t nbytes, int16_t *out)
{
    return ddc_process_wide(d, iq, nbytes, out, NULL, NULL);
}

size_t ddc_process_wide(ddc_t *d, const unsigned char *iq, size_t nbytes, int16_t *out, int16_t *wide, size_t *nwide)
{
    size_t p, npairs = nbytes / 2, written = 0;
    if (nwide) *nwide = 0;
    for (p = 0; p < npairs; p++) {
        rotate_pair(d->phase, iq[2 * p], iq[2 * p + 1], d->pend + 2 * d->npend);
        d->phase = (d->phase + 1) & 3;
        if (++d->npend < DDC_BLOCK) continue;
        {
            int i, len = 2 * DDC_BLOCK;
            for (i = 0; i < DDC_PASSES; i++) {
                fifth_order(d->pend, len >> i, d->hist_i[i]);
                fifth_order(d->pend + 1, (len >> i) - 1, d->hist_q[i]);
                if (wide && i == DDC_WIDE_PASS) { /* the stream at DDC_WIDE_RATE, before the last two halvings */
                    memcpy(wide + 2 * *nwide, d->pend, (len >> (i + 1)) * sizeof *wide);
                    *nwide += (size_t)(len >> (i + 2));
                }
            }
            len >>= DDC_PASSES;
            generic_fir(d->pend, len, cic_9_tables[DDC_PASSES], d->droop_i);
            generic_fir(d->pend + 1, len - 1, cic_9_tables[DDC_PASSES], d->droop_q);
            memcpy(out + 2 * written, d->pend, len * sizeof *out);
            written += (size_t)len / 2;
        }
        d->npend = 0;
    }
    return written;
}
