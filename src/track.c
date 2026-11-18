/* Flight history, burst detection, proximity beeper, PCM audio stream, slippy-map tile maths. */
#define _GNU_SOURCE
#include "sc.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------------- history for the graphs ---------------- */
void hist_reset(hist_t *h)
{
    h->n = 0;
    h->t0 = h->t_last = 0;
    h->step_s = 5.0;
}

int hist_add(hist_t *h, const sonde_t *s, double now)
{
    hist_pt_t *p;
    if (h->n == 0) h->t0 = now;
    else if (now - h->t_last < h->step_s) return 0;
    if (h->n == HIST_MAX) { /* keep every second sample, halve the time resolution */
        int i;
        for (i = 0; i < HIST_MAX / 2; i++) h->p[i] = h->p[2 * i];
        h->n = HIST_MAX / 2;
        h->step_s *= 2.0;
    }
    p = &h->p[h->n++];
    p->t_s = (float)(now - h->t0);
    p->alt = (float)s->alt;
    p->lat = (float)s->lat;
    p->lon = (float)s->lon;
    p->vv = (float)s->vel_v;
    p->temp = (float)s->temp;
    p->rh = (float)s->humidity;
    p->pres = (float)s->pressure;
    p->ws = (float)s->vel_h;
    p->wd = (float)norm360(s->heading + 180.0); /* direction the wind blows from */
    h->t_last = now;
    return 1;
}

/* ---------------- flight phase and burst detection ---------------- */
void flight_reset(flight_t *f)
{
    memset(f, 0, sizeof *f);
}

/* Burst = vertical speed below -3 m/s on 3 consecutive frames, above 1000 m, after an
 * observed ascent. A descent seen without a prior ascent (receiver started late) sets the
 * phase but raises no alert. */
int flight_update(flight_t *f, const sonde_t *s, double now)
{
    int event = 0;
    if (f->t_first == 0) f->t_first = now;
    if (s->alt > f->max_alt) f->max_alt = s->alt;
    f->neg_cnt = (s->vel_v < -3.0) ? f->neg_cnt + 1 : 0;
    f->still_cnt = (fabs(s->vel_v) < 0.5) ? f->still_cnt + 1 : 0;

    if (f->phase == PH_UNKNOWN || f->phase == PH_ASCENT) {
        if (f->neg_cnt >= 3) {
            if (f->phase == PH_ASCENT && s->alt > 1000.0) {
                f->burst = 1;
                f->burst_alt = f->max_alt;
                f->burst_t = now;
                event = 1;
            }
            f->phase = PH_DESCENT;
        } else if (s->vel_v > 1.0) {
            f->phase = PH_ASCENT;
        }
    } else if (f->phase == PH_DESCENT) {
        if (f->still_cnt >= 5) f->phase = PH_LANDED;
    }
    return event;
}

/* ---------------- proximity beeper ---------------- */
/* T = 0.12 + 1.88 (d / 10 km)^0.6 seconds: 2.0 s at 10 km, 0.59 s at 1 km, 0.24 s at 100 m. */
double prox_interval(double d)
{
    if (d < 0) d = 0;
    if (d > 10000.0) d = 10000.0;
    return 0.12 + 1.88 * pow(d / 10000.0, 0.6);
}

/* V = 1 - 0.35 log10(d / 100 m), limited to 0.3..1.0: 30 % at 10 km, 65 % at 1 km, 100 % at 100 m. */
double prox_volume(double d)
{
    double v = d <= 100.0 ? 1.0 : 1.0 - 0.35 * log10(d / 100.0);
    return v < 0.3 ? 0.3 : (v > 1.0 ? 1.0 : v);
}

int prox_update(prox_t *p, int have_dist, double d, double now, double radius, double hyst)
{
    if (!have_dist) {
        p->active = 0;
        return 0;
    }
    if (!p->active && d <= radius) {
        p->active = 1;
        p->t_next = now;
    } else if (p->active && d > radius + hyst) {
        p->active = 0;
    }
    if (!p->active || now < p->t_next) return 0;
    p->t_next = now + prox_interval(d);
    return !p->muted;
}

/* ---------------- audio: continuous raw PCM stream into an external player ---------------- */
void audio_init(audio_t *a)
{
    memset(a, 0, sizeof *a);
    a->fd = -1;
}

void audio_tone(audio_t *a, double freq, double dur_s, double vol)
{
    if (a->nseg >= AUDIO_SEGS) return;
    a->seg[a->nseg].freq = (float)freq;
    a->seg[a->nseg].vol = (float)(vol < 0 ? 0 : (vol > 1 ? 1 : vol));
    a->seg[a->nseg].n = (int)(dur_s * AUDIO_RATE);
    a->nseg++;
}

int audio_render(audio_t *a, short *out, int n)
{
    int i, active = 0;
    for (i = 0; i < n; i++) {
        short v = 0;
        if (a->nseg > 0) {
            int len = a->seg[0].n, pos = a->seg_pos, edge = AUDIO_RATE / 200; /* 5 ms ramps: no clicks */
            double env = 1.0;
            if (pos < edge) env = (double)pos / edge;
            if (len - pos < edge) env = (double)(len - pos) / edge;
            if (a->seg[0].freq > 0) {
                v = (short)(sin(a->phase) * 30000.0 * a->seg[0].vol * env);
                a->phase += 2.0 * M_PI * a->seg[0].freq / AUDIO_RATE;
                if (a->phase > 2.0 * M_PI) a->phase -= 2.0 * M_PI;
            }
            active = 1;
            if (++a->seg_pos >= len) {
                memmove(&a->seg[0], &a->seg[1], sizeof a->seg[0] * (size_t)(a->nseg - 1));
                a->nseg--;
                a->seg_pos = 0;
                a->phase = 0;
            }
        }
        out[i] = v;
    }
    return active;
}

static void audio_open(audio_t *a, const cfg_t *c)
{
    int pf[2];
    pid_t pid;
    if (pipe(pf) < 0) return;
    pid = fork();
    if (pid < 0) {
        close(pf[0]);
        close(pf[1]);
        return;
    }
    if (pid == 0) {
        setsid();
        dup2(pf[0], 0);
        close(pf[0]);
        close(pf[1]);
        execl("/bin/sh", "sh", "-c", c->audio_cmd, (char *)NULL);
        _exit(127);
    }
    close(pf[0]);
    fcntl(pf[1], F_SETFL, O_NONBLOCK);
    fcntl(pf[1], F_SETFD, FD_CLOEXEC);
    a->pid = pid;
    a->fd = pf[1];
}

void audio_close(audio_t *a)
{
    if (a->fd >= 0) close(a->fd);
    a->fd = -1;
    if (a->pid > 0) {
        kill(-a->pid, SIGTERM);
        waitpid(a->pid, NULL, 0);
        reap_group(a->pid);
    }
    a->pid = 0;
}

/* Called every main-loop pass: writes as many samples as wall-clock time has advanced, tone
 * or silence, so the player never underruns and beeps start within one buffer period. */
void audio_poll(audio_t *a, const cfg_t *c, double now)
{
    short buf[AUDIO_RATE / 5];
    int n;
    if (!c->audio_enable) return;
    if (a->fd < 0) {
        if (now < a->t_retry) return;
        a->t_retry = now + 5.0;
        audio_open(a, c);
        a->t_last = now;
        if (a->fd < 0) return;
    }
    n = (int)((now - a->t_last) * AUDIO_RATE);
    if (n <= 0) return;
    if (n > (int)(sizeof buf / sizeof buf[0])) n = (int)(sizeof buf / sizeof buf[0]);
    a->t_last = now;
    audio_render(a, buf, n);
    if (write(a->fd, buf, (size_t)n * sizeof buf[0]) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        audio_close(a); /* player exited: reopen after t_retry */
    }
}

/* ---------------- slippy-map tiles (Web Mercator) ---------------- */
void tile_frac(double lat, double lon, int z, double *tx, double *ty)
{
    double n = (double)(1 << z), r = lat * M_PI / 180.0;
    *tx = (lon + 180.0) / 360.0 * n;
    *ty = (1.0 - asinh(tan(r)) / M_PI) / 2.0 * n;
}

double tile_mpp(double lat, int z)
{
    return 156543.03392 * cos(lat * M_PI / 180.0) / (double)(1 << z);
}

/* Largest zoom at which both points fit inside a w x h pixel window. */
int tile_fit_zoom(double lat1, double lon1, double lat2, double lon2, int w, int h, int zmin, int zmax)
{
    int z;
    for (z = zmax; z > zmin; z--) {
        double x1, y1, x2, y2;
        tile_frac(lat1, lon1, z, &x1, &y1);
        tile_frac(lat2, lon2, z, &x2, &y2);
        if (fabs(x2 - x1) * TILE_PX <= w && fabs(y2 - y1) * TILE_PX <= h) break;
    }
    return z;
}
