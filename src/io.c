#define _GNU_SOURCE
#include "sc.h"
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---------------- gpsd client (JSON over TCP) ---------------- */
void gps_init(gps_t *g)
{
    memset(g, 0, sizeof *g);
    g->lr.fd = -1;
    g->ser.fd = -1;
    g->t_fix = -1e9;
    g->t_tpv = -1e9;
}

static void gpsd_close(gps_t *g)
{
    if (g->lr.fd >= 0) close(g->lr.fd);
    g->lr.fd = -1;
    g->lr.len = 0;
    g->lr.skip = 0;
    g->connecting = 0;
}

void gps_close(gps_t *g)
{
    gpsd_close(g);
    nmea_close(g);
}

/* Start a non-blocking connect: gpsd on another host (a phone over Wi-Fi) may not answer at all, and a blocking
 * connect would hold the UI for the whole TCP timeout. gps_poll finishes it. A name that does not resolve is
 * looked up again only after 30 s, as each lookup can take seconds without a network. */
static void gps_connect(gps_t *g, const cfg_t *c, double now)
{
    struct addrinfo hints, *res = NULL;
    char port[12];
    int fd;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%d", c->gpsd_port);
    if (getaddrinfo(c->gpsd_host, port, &hints, &res) != 0 || !res) {
        g->t_retry = now + 30.0;
        return;
    }
    fd = socket(res->ai_family, res->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, res->ai_protocol);
    if (fd >= 0 && (connect(fd, res->ai_addr, res->ai_addrlen) == 0 || errno == EINPROGRESS)) {
        g->lr.fd = fd;
        g->lr.len = 0;
        g->lr.skip = 0;
        g->connecting = 1;
        g->t_conn = now;
    } else if (fd >= 0) {
        close(fd);
    }
    freeaddrinfo(res);
}

/* 1 = connected and watching, 0 = still connecting, -1 = failed (closed). */
static int gps_connected(gps_t *g, double now)
{
    static const char watch[] = "?WATCH={\"enable\":true,\"json\":true}\n";
    struct pollfd p = {g->lr.fd, POLLOUT, 0};
    int err = 0;
    socklen_t len = sizeof err;
    if (!g->connecting) return 1;
    if (poll(&p, 1, 0) == 0) {
        if (now - g->t_conn < 5.0) return 0;
        gpsd_close(g); /* no answer */
        return -1;
    }
    if (getsockopt(g->lr.fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0 ||
        write(g->lr.fd, watch, sizeof watch - 1) != (ssize_t)(sizeof watch - 1)) {
        gpsd_close(g);
        return -1;
    }
    g->connecting = 0;
    return 1;
}

int gps_parse(gps_t *g, const char *line, double now)
{
    char cls[16];
    double v;
    if (!js_str(line, "class", cls, sizeof cls)) return 0;
    if (!strcmp(cls, "SKY")) { /* gpsd 3.20+ puts the counts at the top; older ones only list satellites */
        if (js_num(line, "nSat", &v) && v >= 0 && v < 1000) {
            g->n_sat = (int)v;
            g->n_used = js_num(line, "uSat", &v) && v >= 0 && v < 1000 ? (int)v : 0;
            g->t_sky = now;
        }
        return 0;
    }
    if (strcmp(cls, "TPV")) return 0;
    g->t_tpv = now;
    g->mode = js_num(line, "mode", &v) && v >= 0 && v <= 3 ? (int)v : 0;
    if (g->mode >= 2) {
        double lat, lon, alt = g->alt, speed = 0, track = 0;
        if (!js_num(line, "lat", &lat) || !js_num(line, "lon", &lon) || !isfinite(lat) || !isfinite(lon) ||
            fabs(lat) > 90.0 || fabs(lon) > 180.0) {
            g->mode = 0;
            return 1;
        }
        if (!js_num(line, "altMSL", &alt)) js_num(line, "alt", &alt);
        js_num(line, "speed", &speed);
        g->has_track = js_num(line, "track", &track) && isfinite(track);
        g->lat = lat;
        g->lon = lon;
        g->alt = isfinite(alt) ? alt : 0.0;
        g->speed = isfinite(speed) ? speed : 0.0;
        g->track = g->has_track ? track : 0.0;
        g->t_fix = now;
    }
    return 1;
}

void gps_poll(gps_t *g, const cfg_t *c)
{
    char line[2048];
    double now = sc_now();
    int r;
    nmea_poll(g, c, now);
    if (c->gpsd_port <= 0) return;
    if (g->lr.fd < 0) {
        if (now < g->t_retry) return;
        g->t_retry = now + 5.0;
        gps_connect(g, c, now);
        if (g->lr.fd < 0) return;
    }
    if (gps_connected(g, now) <= 0) return;
    while ((r = lr_readline(&g->lr, line, sizeof line)) == 1) gps_parse(g, line, now);
    if (r < 0) gpsd_close(g);
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
    while (found != SDR_RTLSDR && (e = readdir(d)) != NULL) {
        unsigned vid, pid;
        if (e->d_name[0] == '.') continue;
        if (!read_hex(usb_sysfs, e->d_name, "idVendor", &vid) || !read_hex(usb_sysfs, e->d_name, "idProduct", &pid)) continue;
        if (vid == 0x0bda && (pid == 0x2838 || pid == 0x2832)) found = SDR_RTLSDR;
        else if (vid == 0x1d50 && (pid == 0x6089 || pid == 0xcc15)) found = SDR_HACKRF; /* HackRF One, rad1o */
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
