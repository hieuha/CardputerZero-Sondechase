/* Map tiles: lookup (preloaded pack or download cache), on-demand download through an external
 * fetch command, cache size limit, and the light-to-dark recolouring of downloaded tiles. */
#define _GNU_SOURCE
#include "sc.h"
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void mkdirs(const char *file_path) /* create every parent directory of file_path */
{
    char p[320];
    char *s;
    snprintf(p, sizeof p, "%s", file_path);
    for (s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
}

void tf_init(tilefetch_t *t, const cfg_t *c)
{
    const char *home = getenv("HOME");
    memset(t, 0, sizeof *t);
    if (c->tile_cache_dir[0]) snprintf(t->cache_dir, sizeof t->cache_dir, "%s", c->tile_cache_dir);
    else snprintf(t->cache_dir, sizeof t->cache_dir, "%s/.cache/sondechase/tiles", home ? home : "/tmp");
}

int tf_find(const tilefetch_t *t, const cfg_t *c, int z, int x, int y, char *path, size_t n)
{
    snprintf(path, n, "%s/%d/%d/%d.png", c->tile_dir, z, x, y);
    if (access(path, R_OK) == 0) return 1;
    snprintf(path, n, "%s/%d/%d/%d.png", t->cache_dir, z, x, y);
    if (access(path, R_OK) == 0) return 2;
    return 0;
}

int tf_busy(const tilefetch_t *t, int z, int x, int y)
{
    int i;
    for (i = 0; i < TF_JOBS; i++)
        if (t->job[i].pid > 0 && t->job[i].z == z && t->job[i].x == x && t->job[i].y == y) return 1;
    return 0;
}

static void subst1(char *out, size_t n, const char *tpl, const char *key, const char *val)
{
    const char *p = strstr(tpl, key);
    if (!p) snprintf(out, n, "%s", tpl);
    else snprintf(out, n, "%.*s%s%s", (int)(p - tpl), tpl, val, p + strlen(key));
}

int tf_request(tilefetch_t *t, const cfg_t *c, int z, int x, int y, double now)
{
    char url[300], a[300], b[700], cmd[1000], num[16];
    int i, slot = -1;
    pid_t pid;
    if (!c->tile_online || z < 0 || z > 19 || tf_busy(t, z, x, y)) return 0;
    for (i = 0; i < TF_FAILS; i++)
        if (t->fail[i].t_retry > now && t->fail[i].z == z && t->fail[i].x == x && t->fail[i].y == y) return 0;
    for (i = 0; i < TF_JOBS; i++)
        if (t->job[i].pid == 0) slot = i;
    if (slot < 0) return 0;

    snprintf(num, sizeof num, "%d", z); subst1(a, sizeof a, c->tile_url, "{z}", num);
    snprintf(num, sizeof num, "%d", x); subst1(url, sizeof url, a, "{x}", num);
    snprintf(num, sizeof num, "%d", y); subst1(a, sizeof a, url, "{y}", num);
    snprintf(url, sizeof url, "%s", a);
    snprintf(t->job[slot].dst, sizeof t->job[slot].dst, "%s/%d/%d/%d.png", t->cache_dir, z, x, y);
    snprintf(t->job[slot].part, sizeof t->job[slot].part, "%s/%d/%d/%d.part", t->cache_dir, z, x, y);
    mkdirs(t->job[slot].dst);
    subst1(b, sizeof b, c->tile_fetch_cmd, "{ua}", c->tile_user_agent);
    subst1(cmd, sizeof cmd, b, "{url}", url);
    subst1(b, sizeof b, cmd, "{out}", t->job[slot].part);

    pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", b, (char *)NULL);
        _exit(127);
    }
    t->job[slot].pid = pid;
    t->job[slot].z = z;
    t->job[slot].x = x;
    t->job[slot].y = y;
    return 1;
}

int tf_poll(tilefetch_t *t, const cfg_t *c, double now)
{
    int i, st, n = 0;
    for (i = 0; i < TF_JOBS; i++) {
        struct stat sb;
        if (t->job[i].pid <= 0 || waitpid(t->job[i].pid, &st, WNOHANG) != t->job[i].pid) continue;
        reap_group(t->job[i].pid);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && stat(t->job[i].part, &sb) == 0 && sb.st_size > 64 &&
            rename(t->job[i].part, t->job[i].dst) == 0) { /* rename: a reader never sees a partial file */
            t->done++;
            t->t_last_ok = now;
            n++;
            if (++t->since_sweep >= 50) {
                t->since_sweep = 0;
                tf_sweep(t, (long)c->tile_cache_mb * 1024L * 1024L);
            }
        } else {
            unlink(t->job[i].part);
            t->fail[t->nfail_slot].z = t->job[i].z;
            t->fail[t->nfail_slot].x = t->job[i].x;
            t->fail[t->nfail_slot].y = t->job[i].y;
            t->fail[t->nfail_slot].t_retry = now + TF_RETRY_S;
            t->nfail_slot = (t->nfail_slot + 1) % TF_FAILS;
            t->failed++;
            t->t_last_fail = now;
        }
        t->job[i].pid = 0;
    }
    return n;
}

void tf_close(tilefetch_t *t)
{
    int i;
    for (i = 0; i < TF_JOBS; i++)
        if (t->job[i].pid > 0) {
            kill(-t->job[i].pid, SIGTERM);
            waitpid(t->job[i].pid, NULL, 0);
            reap_group(t->job[i].pid);
            unlink(t->job[i].part);
            t->job[i].pid = 0;
        }
}

/* ---- cache size limit: remove the least recently modified tiles until the total fits ---- */
typedef struct { char path[300]; long size; time_t mtime; } tfile_t;
typedef struct { tfile_t *v; int n, cap; long total; } tlist_t;

static void walk(const char *dir, tlist_t *l, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char p[300];
        struct stat sb;
        if (e->d_name[0] == '.') continue;
        if ((size_t)snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= sizeof p || stat(p, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) {
            if (depth < 2) walk(p, l, depth + 1);
        } else if (S_ISREG(sb.st_mode)) {
            if (l->n == l->cap) {
                tfile_t *nv = realloc(l->v, sizeof *nv * (size_t)(l->cap ? l->cap * 2 : 256));
                if (!nv) continue;
                l->v = nv;
                l->cap = l->cap ? l->cap * 2 : 256;
            }
            snprintf(l->v[l->n].path, sizeof l->v[l->n].path, "%s", p);
            l->v[l->n].size = (long)sb.st_size;
            l->v[l->n].mtime = sb.st_mtime;
            l->total += (long)sb.st_size;
            l->n++;
        }
    }
    closedir(d);
}

static int by_mtime(const void *a, const void *b)
{
    time_t x = ((const tfile_t *)a)->mtime, y = ((const tfile_t *)b)->mtime;
    return (x > y) - (x < y);
}

long tf_sweep(const tilefetch_t *t, long max_bytes)
{
    tlist_t l = {NULL, 0, 0, 0};
    int i;
    walk(t->cache_dir, &l, 0);
    if (l.total > max_bytes) {
        qsort(l.v, (size_t)l.n, sizeof l.v[0], by_mtime);
        for (i = 0; i < l.n && l.total > max_bytes; i++)
            if (unlink(l.v[i].path) == 0) l.total -= l.v[i].size;
    }
    free(l.v);
    return l.total;
}

/* ---- recolouring ----
 * Detailed: the most frequent colour of the tile is taken as "land". Output brightness follows the
 * colour distance from it, so roads, labels and boundaries become light lines on the dark
 * ground; blue-dominant pixels become water; near-white pixels (road fill) are lifted.
 * Simple: only water and roads survive. The OpenStreetMap style draws main roads in light red, orange and
 * yellow and minor roads in white; labels are dark or strongly saturated, landuse and buildings pale. */
static const int G[3] = {10, 6, 20}, L[3] = {58, 36, 102}, H[3] = {167, 155, 209}, W[3] = {31, 47, 122};
static const int MINOR[3] = {40, 26, 72};

static const int *simple_colour(int r, int g, int b)
{
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double s = mx ? (double)(mx - mn) / mx : 0, h = 0;
    if (b > r + 18 && b > g + 6 && mx >= 191) return W; /* water fill (light blue); blue labels are darker */
    if (mx == r && mx > mn) h = (double)(g - b) / (mx - mn) / 6.0; /* hue 0..1 with red at 0 */
    if (h < 0) h += 1.0;
    if (mx == r && mx > 216 && s > 0.2 && s < 0.8 && (h < 0.18 || h > 0.9)) return H; /* main roads */
    if (r >= 250 && g >= 250 && b >= 250) return MINOR;
    return G;
}

void tile_recolor(unsigned char *rgb, int npix, int simple)
{
    static unsigned short hist[32768];
    int i, k, best = 0, land[3];
    if (simple) {
        for (i = 0; i < npix; i++) {
            const int *c = simple_colour(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]);
            for (k = 0; k < 3; k++) rgb[3 * i + k] = (unsigned char)c[k];
        }
        return;
    }
    memset(hist, 0, sizeof hist);
    for (i = 0; i < npix; i++) {
        int q = ((rgb[3 * i] >> 3) << 10) | ((rgb[3 * i + 1] >> 3) << 5) | (rgb[3 * i + 2] >> 3);
        if (hist[q] < 65535) hist[q]++;
        if (hist[q] > hist[best]) best = q;
    }
    land[0] = ((best >> 10) & 31) << 3 | 4;
    land[1] = ((best >> 5) & 31) << 3 | 4;
    land[2] = (best & 31) << 3 | 4;
    for (i = 0; i < npix; i++) {
        unsigned char *p = &rgb[3 * i];
        int r = p[0], g = p[1], b = p[2], d;
        const int *a, *c;
        if (b > r + 18 && b > g + 6) {
            for (k = 0; k < 3; k++) p[k] = (unsigned char)W[k];
            continue;
        }
        d = abs(r - land[0]) + abs(g - land[1]) + abs(b - land[2]);
        if (r >= 250 && g >= 250 && b >= 250 && d > 12) { /* white road fill */
            for (k = 0; k < 3; k++) p[k] = (unsigned char)(L[k] + (H[k] - L[k]) * 6 / 10);
            continue;
        }
        if (d > 150) d = 150;
        if (d < 75) { a = G; c = L; }
        else { a = L; c = H; d -= 75; }
        for (k = 0; k < 3; k++) p[k] = (unsigned char)(a[k] + (c[k] - a[k]) * d / 75);
    }
}
