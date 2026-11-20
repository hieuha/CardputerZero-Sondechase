#define _GNU_SOURCE
#include "sc.h"
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---------------- gpsd client (JSON over TCP) ---------------- */
void gps_init(gps_t *g)
{
    memset(g, 0, sizeof *g);
    g->lr.fd = -1;
    g->t_fix = -1e9;
}

void gps_close(gps_t *g)
{
    if (g->lr.fd >= 0) close(g->lr.fd);
    g->lr.fd = -1;
    g->lr.len = 0;
}

static void gps_connect(gps_t *g, const cfg_t *c)
{
    static const char watch[] = "?WATCH={\"enable\":true,\"json\":true}\n";
    struct addrinfo hints, *res = NULL;
    char port[12];
    int fd;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%d", c->gpsd_port);
    if (getaddrinfo(c->gpsd_host, port, &hints, &res) != 0 || !res) return;
    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) == 0 &&
        write(fd, watch, sizeof watch - 1) == (ssize_t)(sizeof watch - 1)) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        g->lr.fd = fd;
        g->lr.len = 0;
    } else if (fd >= 0) {
        close(fd);
    }
    freeaddrinfo(res);
}

int gps_parse(gps_t *g, const char *line, double now)
{
    char cls[16];
    double v;
    if (!js_str(line, "class", cls, sizeof cls)) return 0;
    if (!strcmp(cls, "SKY")) { /* gpsd 3.20+ puts the counts at the top; older ones only list satellites */
        if (js_num(line, "nSat", &v)) {
            g->n_sat = (int)v;
            g->n_used = js_num(line, "uSat", &v) ? (int)v : 0;
            g->t_sky = now;
        }
        return 0;
    }
    if (strcmp(cls, "TPV")) return 0;
    g->mode = js_num(line, "mode", &v) ? (int)v : 0;
    if (g->mode >= 2 && js_num(line, "lat", &g->lat) && js_num(line, "lon", &g->lon)) {
        g->t_fix = now;
        if (!js_num(line, "altMSL", &g->alt)) js_num(line, "alt", &g->alt);
        if (!js_num(line, "speed", &g->speed)) g->speed = 0;
        g->has_track = js_num(line, "track", &g->track);
    }
    return 1;
}

void gps_poll(gps_t *g, const cfg_t *c)
{
    char line[2048];
    double now = sc_now();
    int r;
    if (c->gpsd_port <= 0) return;
    if (g->lr.fd < 0) {
        if (now < g->t_retry) return;
        g->t_retry = now + 5.0;
        gps_connect(g, c);
        if (g->lr.fd < 0) return;
    }
    while ((r = lr_readline(&g->lr, line, sizeof line)) == 1) gps_parse(g, line, now);
    if (r < 0) gps_close(g);
}

int gps_ok(const gps_t *g, double now)
{
    return g->mode >= 2 && now - g->t_fix < 5.0;
}

int gps_sky(const gps_t *g, double now)
{
    return g->t_sky > 0 && now - g->t_sky < 10.0;
}

/* ---------------- SDR presence ---------------- */
static int read_hex(const char *dir, const char *name, const char *file, unsigned *v)
{
    char p[400];
    FILE *f;
    int ok;
    snprintf(p, sizeof p, "%s/%s/%s", dir, name, file);
    f = fopen(p, "r");
    if (!f) return 0;
    ok = fscanf(f, "%x", v) == 1;
    fclose(f);
    return ok;
}

int sdr_present(const char *usb_sysfs)
{
    DIR *d = opendir(usb_sysfs);
    struct dirent *e;
    int found = 0;
    if (!d) return 0;
    while (!found && (e = readdir(d)) != NULL) {
        unsigned vid, pid;
        if (e->d_name[0] == '.') continue;
        if (read_hex(usb_sysfs, e->d_name, "idVendor", &vid) && read_hex(usb_sysfs, e->d_name, "idProduct", &pid))
            found = vid == 0x0bda && (pid == 0x2838 || pid == 0x2832);
    }
    closedir(d);
    return found;
}

/* ---------------- scan spectrum ---------------- */
void spec_reset(spectrum_t *s, long lo, long hi, long step)
{
    memset(s, 0, sizeof *s);
    if (step <= 0) step = 5000;
    s->f0 = lo;
    s->step = step;
    s->n = (int)((hi - lo) / step);
    if (s->n > SPEC_MAX) s->n = SPEC_MAX;
    if (s->n < 0) s->n = 0;
}

static int spec_cell(const spectrum_t *s, double f_hz)
{
    return (int)floor((f_hz - (double)s->f0) / (double)s->step + 0.5);
}

/* One in-process sweep step: each cell it covers gets the max of the FFT bins falling in it, held
 * against the previous pass decayed by decay_db. A step sees only milliseconds of signal and an RS41
 * pauses between frames, so without the hold its peak would come and go; with it, a signal that
 * stops still fades out at decay_db per pass. */
int spec_add_bins(spectrum_t *s, double f_lo, double bin_hz, const float *db, int n, double decay_db)
{
    static float best[SPEC_MAX];
    static unsigned char seen[SPEC_MAX];
    int i, lo = s->n, hi = -1, added = 0;
    for (i = 0; i < n; i++) {
        int idx = spec_cell(s, f_lo + i * bin_hz);
        if (idx < 0 || idx >= s->n || !isfinite(db[i])) continue;
        if (!seen[idx] || db[i] > best[idx]) best[idx] = db[i];
        seen[idx] = 1;
        if (idx < lo) lo = idx;
        if (idx > hi) hi = idx;
    }
    for (i = lo; i <= hi; i++)
        if (seen[i]) {
            float held = (float)(s->db[i] - decay_db);
            s->db[i] = s->ok[i] && held > best[i] ? held : best[i];
            s->ok[i] = 1;
            seen[i] = 0;
            added++;
        }
    return added;
}

static int cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static int cmp_peak(const void *a, const void *b)
{
    float x = ((const peak_t *)a)->snr_db, y = ((const peak_t *)b)->snr_db;
    return (x < y) - (x > y);
}

/* Peaks = contiguous runs above (median + snr_db). Centre frequency is the power-weighted
 * centroid of the run, rounded to the 10 kHz raster used by 400 MHz radiosondes. */
int spec_peaks(spectrum_t *s, double snr_db, peak_t *out, int max)
{
    static float tmp[SPEC_MAX];
    int i, m = 0, np = 0;
    for (i = 0; i < s->n; i++)
        if (s->ok[i]) tmp[m++] = s->db[i];
    if (m < 8) return 0;
    qsort(tmp, (size_t)m, sizeof tmp[0], cmp_f);
    s->floor_db = tmp[m / 2];
    s->max_db = tmp[m - 1];
    for (i = 0; i < s->n; i++)
        if (!s->ok[i]) s->db[i] = (i > 0) ? s->db[i - 1] : s->floor_db;

    float thr = s->floor_db + (float)snr_db;
    i = 0;
    while (i < s->n) {
        if (s->db[i] < thr) {
            i++;
            continue;
        }
        double sw = 0, sf = 0;
        float pk = s->db[i];
        for (; i < s->n && s->db[i] >= thr; i++) {
            double wgt = pow(10.0, (s->db[i] - s->floor_db) / 10.0);
            sw += wgt;
            sf += wgt * ((double)s->f0 + (double)i * (double)s->step);
            if (s->db[i] > pk) pk = s->db[i];
        }
        peak_t p;
        p.f_hz = (long)floor(sf / sw / 10000.0 + 0.5) * 10000L;
        p.snr_db = pk - s->floor_db;
        int j, dup = 0;
        for (j = 0; j < np; j++)
            if (out[j].f_hz == p.f_hz) {
                dup = 1;
                if (p.snr_db > out[j].snr_db) out[j].snr_db = p.snr_db;
            }
        if (dup) continue;
        if (np < max) {
            out[np++] = p;
        } else {
            int w = 0;
            for (j = 1; j < np; j++)
                if (out[j].snr_db < out[w].snr_db) w = j;
            if (p.snr_db > out[w].snr_db) out[w] = p;
        }
    }
    qsort(out, (size_t)np, sizeof out[0], cmp_peak);
    return np;
}
