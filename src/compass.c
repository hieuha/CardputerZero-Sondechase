/* Heading: GPS course while moving, else the magnetometer, tilt-compensated with the accelerometer once the
 * calibration (Info, K) has found how the two sensors' axes line up. */
#define _GNU_SOURCE
#include "sc.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ACC_SMOOTH 0.3 /* low-pass on the accelerometer at 5 Hz: about 0.6 s, so steps while walking average out */

static int read_num(const char *path, double *v)
{
    FILE *f = fopen(path, "r");
    int ok;
    if (!f) return 0;
    ok = fscanf(f, "%lf", v) == 1;
    fclose(f);
    return ok;
}

static double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void cross3(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static int unit3(double v[3])
{
    double n = sqrt(dot3(v, v));
    if (n < 1e-9) return 0;
    v[0] /= n; v[1] /= n; v[2] /= n;
    return 1;
}

/* "+y-x+z": magnetometer axis i reads sign[i] * accelerometer axis perm[i]. 0 = not three signed, distinct axes. */
static int axes_parse(const char *s, int perm[3], int sign[3])
{
    int i, used = 0;
    for (i = 0; i < 3; i++) {
        if ((s[0] != '+' && s[0] != '-') || s[1] < 'x' || s[1] > 'z') return 0;
        sign[i] = s[0] == '-' ? -1 : 1;
        perm[i] = s[1] - 'x';
        if (used & (1 << perm[i])) return 0;
        used |= 1 << perm[i];
        s += 2;
    }
    return *s == 0;
}

static void axes_map(const int perm[3], const int sign[3], const double acc[3], double out[3])
{
    int i;
    for (i = 0; i < 3; i++) out[i] = sign[i] * acc[perm[i]];
}

/* ---------------- sensors ---------------- */
/* The first IIO device that has all of the given channel's x, y, z files; paths written to px, py, pz. */
static int iio_find(const char *chan, char *px, char *py, char *pz, size_t n, int *dev)
{
    int i;
    for (i = 0; i < 10; i++) {
        snprintf(px, n, "/sys/bus/iio/devices/iio:device%d/in_%s_x_raw", i, chan);
        snprintf(py, n, "/sys/bus/iio/devices/iio:device%d/in_%s_y_raw", i, chan);
        snprintf(pz, n, "/sys/bus/iio/devices/iio:device%d/in_%s_z_raw", i, chan);
        if (access(px, R_OK) == 0 && access(py, R_OK) == 0) {
            *dev = i;
            return 1;
        }
    }
    return 0;
}

void heading_init(heading_t *h, const cfg_t *c)
{
    int dev;
    memset(h, 0, sizeof *h);
    if (!c->mag_enable) return;
    h->mag_found = iio_find("magn", h->mag_x, h->mag_y, h->mag_z, sizeof h->mag_x, &dev);
    if (h->mag_found) {
        char p[160];
        snprintf(p, sizeof p, "/sys/bus/iio/devices/iio:device%d/in_magn_scale", dev);
        if (!read_num(p, &h->mag_scale)) h->mag_scale = 0;
        /* tilt compensation needs the magnetometer's z axis as well as all three accelerometer axes */
        h->acc_found = access(h->mag_z, R_OK) == 0 &&
                       iio_find("accel", h->acc_x, h->acc_y, h->acc_z, sizeof h->acc_x, &dev) &&
                       access(h->acc_z, R_OK) == 0;
    }
}

int mag_read(const heading_t *h, double mag[3])
{
    if (!h->mag_found || !read_num(h->mag_x, &mag[0]) || !read_num(h->mag_y, &mag[1])) return 0;
    if (!h->acc_found || !read_num(h->mag_z, &mag[2])) mag[2] = 0;
    return 1;
}

int acc_read(const heading_t *h, double acc[3])
{
    return h->acc_found && read_num(h->acc_x, &acc[0]) && read_num(h->acc_y, &acc[1]) && read_num(h->acc_z, &acc[2]);
}

/* ---------------- heading ---------------- */
/* Device held level. Axis orientation depends on how the sensor is mounted:
 * mag_flip_y and mag_rot_deg are set during calibration. */
double heading_from_mag(double x, double y, const cfg_t *c)
{
    x -= c->mag_off_x;
    y -= c->mag_off_y;
    if (c->mag_flip_y) y = -y;
    return norm360(atan2(y, x) * 180.0 / M_PI + c->mag_rot_deg + c->declination);
}

/* Any attitude: the accelerometer (mapped into the magnetometer's axes) gives "up"; the field minus its vertical
 * part points to magnetic north; the heading is the angle from there to the top of the device, which lies in the
 * magnetometer's x/y plane at -mag_rot_deg, seen from above. mag_flip_y says the magnetometer's z points down when
 * the device is flat, so seen from its own z that angle is mirrored. Held flat this equals heading_from_mag.
 * NAN when the top of the device points nearly straight up or down, or the axes are not set. */
double heading_tilt(const double mag[3], const double acc[3], const cfg_t *c)
{
    int perm[3], sign[3];
    double m[3], u[3], north[3], east[3], f[3], r = (c->mag_flip_y ? -1 : 1) * c->mag_rot_deg * M_PI / 180.0;
    if (!axes_parse(c->accel_axes, perm, sign)) return NAN;
    m[0] = mag[0] - c->mag_off_x;
    m[1] = mag[1] - c->mag_off_y;
    m[2] = mag[2] - c->mag_off_z;
    axes_map(perm, sign, acc, u);
    if (!unit3(u)) return NAN;
    north[0] = m[0] - dot3(m, u) * u[0];
    north[1] = m[1] - dot3(m, u) * u[1];
    north[2] = m[2] - dot3(m, u) * u[2];
    cross3(north, u, east);
    f[0] = cos(r);
    f[1] = -sin(r);
    f[2] = 0;
    if (fabs(dot3(f, u)) > 0.97 || dot3(north, north) < 1e-12) return NAN; /* within 14° of vertical */
    return norm360(atan2(dot3(f, east), dot3(f, north)) * 180.0 / M_PI + c->declination);
}

void heading_update(heading_t *h, const gps_t *g, const cfg_t *c, double now)
{
    if (gps_ok(g, now) && g->has_track && g->speed >= c->cog_min_speed) {
        h->deg = norm360(g->track);
        h->src = HDG_GPS;
        return;
    }
    if (h->mag_found) {
        double m[3], a[3];
        if (now - h->t_mag < 0.2) return; /* 5 Hz sysfs polling */
        h->t_mag = now;
        if (c->mag_tilt && h->acc_found) {
            if (acc_read(h, a)) {
                int i;
                for (i = 0; i < 3; i++) h->acc[i] = h->acc_ok ? h->acc[i] + ACC_SMOOTH * (a[i] - h->acc[i]) : a[i];
                h->acc_ok = 1;
            }
            if (h->acc_ok && mag_read(h, m)) {
                double d = heading_tilt(m, h->acc, c);
                if (!isnan(d)) {
                    h->deg = d;
                    h->src = HDG_MAG;
                    return;
                }
            }
        } else if (mag_read(h, m)) {
            h->deg = heading_from_mag(m[0], m[1], c);
            h->src = HDG_MAG;
            return;
        }
    }
    if (h->src != HDG_NONE) h->src = HDG_HOLD;
}

/* ---------------- calibration ---------------- */
/* A level turn traces a circle as wide as the horizontal part of the Earth's field: about 0.8 G across in Vietnam,
 * still over 0.3 G at 60° latitude. 0.2 G is well above the sensor noise (about 0.04 G on the BMM150). */
double mag_min_span(const heading_t *h)
{
    return h->mag_scale > 0 ? 0.2 / h->mag_scale : 300.0;
}

void magcal_start(magcal_t *m, double min_span, int use_acc)
{
    memset(m, 0, sizeof *m);
    m->step = MAGCAL_TURN;
    m->min_span = min_span;
    m->use_acc = use_acc;
}

static double magcal_ang(const magcal_t *m, double x, double y)
{
    return atan2(y - (m->ymin + m->ymax) / 2, x - (m->xmin + m->xmax) / 2) * 180.0 / M_PI;
}

/* Tilt step: which way (4 sectors around "up" while level) and how far the device leans; samples that lean at
 * least 10° are kept for the fit, MAGCAL_TILT_DEG in a sector counts it as done. Samples taken while the device
 * is being moved (more than 5% away from 1 g) are skipped: the accelerometer then does not point up. */
static void magcal_tilt_add(magcal_t *m, const double mag[3], const double acc[3])
{
    double up[3], a[3] = {acc[0], acc[1], acc[2]}, e1[3] = {0, 0, 0}, e2[3], p[3], c, g;
    int i, k = 0;
    if (m->acc_level_n == 0) return;
    for (i = 0; i < 3; i++) up[i] = m->acc_level[i];
    g = sqrt(dot3(up, up)) / m->acc_level_n;
    if (fabs(sqrt(dot3(a, a)) / g - 1.0) > 0.05 || !unit3(up) || !unit3(a)) return;
    c = dot3(a, up);
    if (c > cos(10.0 * M_PI / 180.0)) return;
    for (i = 1; i < 3; i++) if (fabs(up[i]) < fabs(up[k])) k = i; /* the axis furthest from up */
    e1[k] = 1;
    for (i = 0; i < 3; i++) e1[i] -= up[k] * up[i];
    unit3(e1);
    cross3(up, e1, e2);
    for (i = 0; i < 3; i++) p[i] = a[i] - c * up[i];
    if (c <= cos(MAGCAL_TILT_DEG * M_PI / 180.0))
        m->tilt_seen[(int)((norm360(atan2(dot3(p, e2), dot3(p, e1)) * 180.0 / M_PI) + 45.0) / 90.0) % 4] = 1;
    i = m->tn < MAGCAL_TILT_MAX ? m->tn : m->tn_all % MAGCAL_TILT_MAX; /* full: keep replacing, all leans stay */
    m->tm[i][0] = (float)mag[0]; m->tm[i][1] = (float)mag[1]; m->tm[i][2] = (float)mag[2];
    m->ta[i][0] = (float)acc[0]; m->ta[i][1] = (float)acc[1]; m->ta[i][2] = (float)acc[2];
    if (m->tn < MAGCAL_TILT_MAX) m->tn++;
    m->tn_all++;
    if (magcal_tilt_covered(m) == 4 && m->tn >= 40) m->step = MAGCAL_NORTH;
}

void magcal_add(magcal_t *m, const double mag[3], const double *acc)
{
    int i;
    double x = mag[0], y = mag[1];
    if (m->step == MAGCAL_OFF) return;
    if (m->step == MAGCAL_TILT) {
        if (acc) magcal_tilt_add(m, mag, acc);
        return;
    }
    if (m->n == 0) { m->xmin = m->xmax = x; m->ymin = m->ymax = y; }
    if (m->step == MAGCAL_TURN) {
        if (x < m->xmin) m->xmin = x;
        if (x > m->xmax) m->xmax = x;
        if (y < m->ymin) m->ymin = y;
        if (y > m->ymax) m->ymax = y;
        if (acc) { /* "up" while held flat, summed */
            for (i = 0; i < 3; i++) m->acc_level[i] += acc[i];
            m->acc_level_n++;
        }
    }
    if (m->n < MAGCAL_MAX) m->n++;
    else { /* full: drop the oldest sample */
        memmove(m->xs, m->xs + 1, sizeof m->xs - sizeof m->xs[0]);
        memmove(m->ys, m->ys + 1, sizeof m->ys - sizeof m->ys[0]);
        memmove(m->zs, m->zs + 1, sizeof m->zs - sizeof m->zs[0]);
        memmove(m->as, m->as + 1, sizeof m->as - sizeof m->as[0]);
    }
    m->xs[m->n - 1] = (float)x;
    m->ys[m->n - 1] = (float)y;
    m->zs[m->n - 1] = (float)mag[2];
    for (i = 0; i < 3; i++) m->as[m->n - 1][i] = acc ? (float)acc[i] : 0.0f;
    if (m->step != MAGCAL_TURN) return;
    /* the centre moves while the circle fills in, so the sectors are recounted around the latest one; nothing counts
     * until the readings have moved further than noise, or a device lying still would fill the ring */
    memset(m->seen, 0, sizeof m->seen);
    if (m->xmax - m->xmin < m->min_span / 2 || m->ymax - m->ymin < m->min_span / 2) return;
    for (i = 0; i < m->n; i++)
        m->seen[(int)(norm360(magcal_ang(m, m->xs[i], m->ys[i])) / (360.0 / MAGCAL_SECTORS)) % MAGCAL_SECTORS] = 1;
    if (magcal_covered(m) == MAGCAL_SECTORS && m->xmax - m->xmin >= m->min_span && m->ymax - m->ymin >= m->min_span)
        m->step = m->use_acc ? MAGCAL_TILT : MAGCAL_NORTH;
}

int magcal_covered(const magcal_t *m)
{
    int i, k = 0;
    for (i = 0; i < MAGCAL_SECTORS; i++) k += m->seen[i];
    return k;
}

int magcal_tilt_covered(const magcal_t *m)
{
    return m->tilt_seen[0] + m->tilt_seen[1] + m->tilt_seen[2] + m->tilt_seen[3];
}

/* How the accelerometer's axes line up with the magnetometer's, from the tilt samples. The angle between gravity
 * and the Earth's field does not change however the device is held, so with the right mapping the dot product of
 * the two unit vectors stays constant: the mapping with the least spread wins. A mapping and its negative fit
 * equally well ("up" or "down"); the one where step 1's right turn makes the heading go up is kept. Mirror-image
 * mappings (a left-handed sensor) are among the 48 signed permutations, so mag_flip_y is not needed here. Both
 * sensors sit flat on the board, so "up" while level must lie along the magnetometer's z axis. Returns the spread
 * (standard deviation) of the winner, and of the runner-up through *second. */
static double magcal_axes(const magcal_t *m, const double off[3], double turned, double *second, int perm[3],
                          int sign[3])
{
    static const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    double best = INFINITY, next = INFINITY;
    int p, s, i;
    for (p = 0; p < 6; p++)
        for (s = 0; s < 8; s++) {
            int sg[3] = {s & 1 ? -1 : 1, s & 2 ? -1 : 1, s & 4 ? -1 : 1};
            double u[3], sum = 0, sum2 = 0, sd;
            axes_map(perms[p], sg, m->acc_level, u);
            if (!unit3(u) || fabs(u[2]) < 0.8) continue;
            if ((u[2] > 0 ? 1 : -1) * turned <= 0) continue; /* level heading moves with the raw angle, or against it */
            for (i = 0; i < m->tn; i++) {
                double a[3] = {m->ta[i][0], m->ta[i][1], m->ta[i][2]}, b[3], d;
                b[0] = m->tm[i][0] - off[0];
                b[1] = m->tm[i][1] - off[1];
                b[2] = m->tm[i][2] - off[2];
                axes_map(perms[p], sg, a, u);
                if (!unit3(u) || !unit3(b)) continue;
                d = dot3(u, b);
                sum += d;
                sum2 += d * d;
            }
            sd = sqrt(fmax(0.0, sum2 / m->tn - (sum / m->tn) * (sum / m->tn)));
            if (sd < best) {
                next = best;
                best = sd;
                memcpy(perm, perms[p], sizeof perms[p]);
                memcpy(sign, sg, sizeof sg);
            } else if (sd < next) next = sd;
        }
    *second = next;
    return best;
}

int magcal_apply(const magcal_t *m, cfg_t *c)
{
    double turned = 0, x = 0, y = 0, z = 0, a[3] = {0, 0, 0};
    int i, k = 0;
    c->mag_tilt = 0;
    if (m->n < 2) return 0;
    c->mag_off_x = (m->xmin + m->xmax) / 2;
    c->mag_off_y = (m->ymin + m->ymax) / 2;
    for (i = 1; i < m->n; i++) turned += norm180(magcal_ang(m, m->xs[i], m->ys[i]) - magcal_ang(m, m->xs[i - 1], m->ys[i - 1]));
    c->mag_flip_y = turned < 0; /* turning right, the heading goes up: the raw angle must too */
    for (i = m->n - 1; i >= 0 && k < 10; i--, k++) { /* the last 0.5 s, pointing north */
        x += m->xs[i]; y += m->ys[i]; z += m->zs[i];
        a[0] += m->as[i][0]; a[1] += m->as[i][1]; a[2] += m->as[i][2];
    }
    c->mag_rot_deg = 0;
    c->mag_rot_deg = norm180(-heading_from_mag(x / k, y / k, c));
    if (m->use_acc && m->tn >= 40) {
        double off[3], sz = 0, sq = 0, szz = 0, szq = 0, mean_z, var_z, spread, second, mag[3] = {x / k, y / k, z / k}, h;
        int perm[3], sign[3], s;
        /* z offset: on a sphere around the offsets, |m - off|^2 is the same for every sample; with x and y offsets
         * known from the turn, x^2 + y^2 + z^2 (offset-corrected in x, y) is linear in z with slope 2 * z offset */
        for (i = 0; i < m->tn; i++) {
            double dx = m->tm[i][0] - c->mag_off_x, dy = m->tm[i][1] - c->mag_off_y, mz = m->tm[i][2];
            double q = dx * dx + dy * dy + mz * mz;
            sz += mz; sq += q; szz += mz * mz; szq += mz * q;
        }
        mean_z = sz / m->tn;
        var_z = szz / m->tn - mean_z * mean_z;
        if (var_z < (m->min_span / 8) * (m->min_span / 8)) return 0; /* the leans barely moved z: no fit */
        c->mag_off_z = (szq / m->tn - mean_z * sq / m->tn) / var_z / 2;
        off[0] = c->mag_off_x;
        off[1] = c->mag_off_y;
        off[2] = c->mag_off_z;
        spread = magcal_axes(m, off, turned, &second, perm, sign);
        /* no clear winner: keep the flat-only compass. On the CardputerZero the right mapping spreads about 0.1
         * (soft iron, the magnetometer lagging the accelerometer) and the next best over twice that. */
        if (!(spread < 0.2 && spread < 0.6 * second)) return 0;
        for (i = 0; i < 3; i++) {
            c->accel_axes[2 * i] = sign[i] < 0 ? '-' : '+';
            c->accel_axes[2 * i + 1] = (char)('x' + perm[i]);
        }
        c->accel_axes[6] = 0;
        c->mag_tilt = 1;
        /* flat at north (the last samples): "up" along the magnetometer's -z means it is mounted upside down;
         * then make "north now" read 0, which also sets the flat-only fallback to the same heading */
        for (i = 0; i < 3; i++) a[i] /= k;
        s = sign[2] * a[perm[2]] > 0 ? 1 : -1;
        c->mag_flip_y = s < 0;
        c->mag_rot_deg = 0;
        h = heading_tilt(mag, a, c);
        if (isnan(h)) return c->mag_tilt = 0;
        c->mag_rot_deg = norm180(-h);
    }
    return c->mag_tilt;
}

int magcal_save(const cfg_t *c, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# Written by SondeChase compass calibration (Info, K). Read after the main config.\n");
    fprintf(f, "mag_off_x = %.3f\nmag_off_y = %.3f\nmag_flip_y = %d\nmag_rot_deg = %.1f\n", c->mag_off_x, c->mag_off_y,
            c->mag_flip_y, c->mag_rot_deg);
    fprintf(f, "mag_off_z = %.3f\nmag_tilt = %d\naccel_axes = %s\n", c->mag_off_z, c->mag_tilt,
            c->accel_axes[0] ? c->accel_axes : "+x+y+z");
    return fclose(f) == 0 ? 0 : -1;
}
