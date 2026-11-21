#define _GNU_SOURCE
#include "sc.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* ---------------- NMEA straight from a serial GPS ----------------
 * A GPS on a cap (the LoRa-1262 cap on the EXT header: /dev/ttyS0) is not something gpsd finds by itself, so the
 * app reads it. gpsd and the app exclude each other on the port: gpsd sets TIOCEXCL and refuses a port someone
 * else has open, and so does this reader. While gpsd reports fixes from any receiver, the sentences read here are
 * dropped, so a USB GPS behind gpsd still wins. */

#define NMEA_FIELDS 24

static const speed_t nmea_bauds[] = {B115200, B9600}; /* the cap's GPS talks 115200; many modules ship at 9600 */
#define NMEA_NBAUD ((int)(sizeof nmea_bauds / sizeof nmea_bauds[0]))

/* "$GNGGA,...*64": 1 when the XOR of the characters between '$' and '*' matches the two hex digits. */
static int nmea_checksum_ok(const char *s)
{
    unsigned x = 0, want;
    char *end;
    if (*s++ != '$') return 0;
    for (; *s && *s != '*'; s++) x ^= (unsigned char)*s;
    if (*s != '*' || !s[1] || !s[2]) return 0;
    want = (unsigned)strtoul(s + 1, &end, 16);
    return end == s + 3 && want == x;
}

/* Splits the sentence body (between '$' and '*') on commas, keeping empty fields. */
static int nmea_split(const char *line, char *buf, size_t n, char **f)
{
    char *p, *star;
    int k = 0;
    snprintf(buf, n, "%s", line + 1);
    if ((star = strchr(buf, '*')) != NULL) *star = 0;
    for (p = buf; k < NMEA_FIELDS; k++) {
        f[k] = p;
        if ((p = strchr(p, ',')) == NULL) return k + 1;
        *p++ = 0;
    }
    return k;
}

/* ddmm.mmmm with its hemisphere letter to signed degrees; 0 when empty or out of range. */
static int nmea_coord(const char *v, const char *hemi, double max, double *out)
{
    char *end;
    double x = strtod(v, &end), d;
    if (end == v || !isfinite(x) || x < 0) return 0;
    d = floor(x / 100.0);
    x = d + (x - d * 100.0) / 60.0;
    if (x > max) return 0;
    if (hemi[0] == 'S' || hemi[0] == 'W') x = -x;
    else if (hemi[0] != 'N' && hemi[0] != 'E') return 0;
    *out = x;
    return 1;
}

/* GP GPS, GL GLONASS, GA Galileo, GB/BD BeiDou, GQ QZSS, GI NavIC: one slot each for satellites in view. */
static int nmea_talker(const char *id)
{
    static const char *t[] = {"GP", "GL", "GA", "GB", "GQ", "GI"};
    int i;
    if (!strncmp(id, "BD", 2)) return 3;
    for (i = 0; i < (int)(sizeof t / sizeof t[0]); i++)
        if (!strncmp(id, t[i], 2)) return i;
    return -1;
}

int nmea_parse(gps_t *g, const char *line, double now)
{
    char buf[256], *f[NMEA_FIELDS];
    const char *type;
    int nf, i;
    if (!nmea_checksum_ok(line)) return 0;
    nf = nmea_split(line, buf, sizeof buf, f);
    if (strlen(f[0]) != 5) return 0;
    type = f[0] + 2;
    g->t_nmea = now;
    if (!strcmp(type, "GSA") && nf > 2) {
        g->nmea_mode = atoi(f[2]) >= 1 && atoi(f[2]) <= 3 ? atoi(f[2]) : 0;
    } else if (!strcmp(type, "GSV") && nf > 3) {
        int t = nmea_talker(f[0]), sum = 0;
        if (t < 0) return 1;
        g->gsv[t] = (unsigned char)(atoi(f[3]) > 0 && atoi(f[3]) < 100 ? atoi(f[3]) : 0);
        for (i = 0; i < (int)sizeof g->gsv; i++) sum += g->gsv[i];
        g->n_sat = sum;
        g->t_sky = now;
    } else if (!strcmp(type, "GGA") && nf > 9) {
        double lat, lon;
        int used = atoi(f[7]);
        g->n_used = used > 0 && used < 100 ? used : 0;
        g->t_sky = now;
        if (atoi(f[6]) > 0 && nmea_coord(f[2], f[3], 90.0, &lat) && nmea_coord(f[4], f[5], 180.0, &lon)) {
            double alt = atof(f[9]);
            g->lat = lat;
            g->lon = lon;
            g->alt = isfinite(alt) ? alt : 0.0;
            g->mode = g->nmea_mode >= 2 ? g->nmea_mode : 2; /* GSA follows GGA: its mode is one second old */
            g->t_fix = now;
        } else {
            g->mode = 1;
        }
    } else if (!strcmp(type, "RMC") && nf > 8 && f[2][0] == 'A') {
        double kn = atof(f[7]);
        g->speed = isfinite(kn) && kn >= 0 ? kn * 0.514444 : 0.0;
        g->has_track = f[8][0] != 0;
        g->track = g->has_track ? fmod(atof(f[8]), 360.0) : 0.0;
    }
    return 1;
}

static void nmea_set_baud(gps_t *g, double now)
{
    struct termios t;
    g->t_baud = now;
    if (tcgetattr(g->ser.fd, &t) != 0) return; /* not a tty (a pipe in the tests): read as it comes */
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    cfsetispeed(&t, nmea_bauds[g->ser_baud]);
    cfsetospeed(&t, nmea_bauds[g->ser_baud]);
    tcsetattr(g->ser.fd, TCSANOW, &t);
    tcflush(g->ser.fd, TCIFLUSH);
}

void nmea_close(gps_t *g)
{
    if (g->ser.fd >= 0) close(g->ser.fd);
    g->ser.fd = -1;
    g->ser.len = 0;
    g->ser.skip = 0;
}

void nmea_poll(gps_t *g, const cfg_t *c, double now)
{
    char line[512];
    int r;
    if (!c->gps_serial[0]) return;
    if (g->ser.fd < 0) {
        if (now < g->t_ser_retry) return;
        g->t_ser_retry = now + 5.0; /* missing, not permitted, or held by gpsd: look again later */
        g->ser.fd = open(c->gps_serial, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (g->ser.fd < 0) return;
        ioctl(g->ser.fd, TIOCEXCL); /* gpsd's hotplug then leaves the port to the app */
        nmea_set_baud(g, now);
    }
    while ((r = lr_readline(&g->ser, line, sizeof line)) == 1) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[n - 1] = 0;
        if (now - g->t_tpv < 3.0) continue; /* gpsd has a receiver: it is the one used */
        nmea_parse(g, line, now);
    }
    if (r < 0) {
        nmea_close(g);
        return;
    }
    if (g->t_nmea < g->t_baud && now - g->t_baud > 3.0) { /* nothing readable at this rate: try the next */
        g->ser_baud = (g->ser_baud + 1) % NMEA_NBAUD;
        nmea_set_baud(g, now);
    }
}
