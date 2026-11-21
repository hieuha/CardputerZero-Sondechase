#define _GNU_SOURCE
#include "sc.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

double sc_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ---------------- config ---------------- */
void cfg_defaults(cfg_t *c)
{
    memset(c, 0, sizeof *c);
    /* the decoders shipped in the package come first; a build elsewhere on PATH still works */
    snprintf(c->decoder_feed_cmd, sizeof c->decoder_feed_cmd,
             "PATH=" SC_DECODER_DIR ":$PATH; fsk_demod --cs16 -b 1200 -u 22800 -s 2 48000 4800 - - 2>/dev/null"
             " | stdbuf -oL rs41mod --ptu2 --json --softin -i --jsn_cfq {f_hz} 2>/dev/null");
    c->scan_lo_hz = 400000000L;
    c->scan_hi_hz = 406000000L;
    c->scan_step_hz = 5000;
    snprintf(c->gpsd_host, sizeof c->gpsd_host, "127.0.0.1");
    c->gpsd_port = 2947;
    c->ground_alt = -1000.0;
    c->burst_alt = 30000.0;
    c->descent_v0 = 6.0;
    c->cog_min_speed = 1.5;
    c->mag_enable = 1;
    c->stale_s = 5.0;
    c->fb_dev[0] = 0; /* found at start by fb_pick */
    c->prox_radius_m = 10000.0;
    c->prox_hyst_m = 500.0;
    c->audio_enable = 1;
    /* PipeWire owns the speaker codec on the CardputerZero image, so plain ALSA aplay cannot open it. */
    snprintf(c->audio_cmd, sizeof c->audio_cmd,
             "pacat --raw --format=s16le --rate=16000 --channels=1 --latency-msec=120"
             " --client-name=SondeChase 2>/dev/null");
    snprintf(c->tile_dir, sizeof c->tile_dir, "/usr/share/sondechase/tiles");
    c->map_lat = 21.0285; /* Hanoi */
    c->map_simple = 1;
    c->map_lon = 105.8542;
    c->zoom_min = 6;
    c->zoom_max = 18;
    snprintf(c->font_dir, sizeof c->font_dir, "/usr/share/sondechase/fonts");
    c->sdr_check = 1;
    snprintf(c->sdr, sizeof c->sdr, "auto");
    snprintf(c->led_path, sizeof c->led_path, "/sys/class/leds/ACT/brightness");
    snprintf(c->ext_power_path, sizeof c->ext_power_path, "/sys/class/leds/ext_5v_out/brightness");
    snprintf(c->gps_serial, sizeof c->gps_serial, "/dev/ttyS0");
    c->tile_online = 1;
    snprintf(c->tile_url, sizeof c->tile_url, "https://tile.openstreetmap.org/{z}/{x}/{y}.png");
    snprintf(c->tile_user_agent, sizeof c->tile_user_agent, "SondeChase/" SC_VERSION " (CardputerZero app; github.com/hieuha)");
    snprintf(c->tile_fetch_cmd, sizeof c->tile_fetch_cmd, "curl -fsS --max-time 20 -A '{ua}' -o '{out}' '{url}'");
    c->tile_cache_mb = 1000;
    c->demo_speed = 30.0;
    c->sondehub_rate_s = 30; /* auto_rx and chasemapper both upload every 30 s */
    snprintf(c->sondehub_url, sizeof c->sondehub_url, "https://api.v2.sondehub.org");
    snprintf(c->sondehub_cmd, sizeof c->sondehub_cmd,
             "curl -sS -o '{resp}' -w '%%{http_code}' --max-time 20 -X PUT -H 'Content-Type: application/json' {enc}"
             " -H 'Date: {date}' -A '{ua}' --data-binary @- '{url}' 2>/dev/null");
}

typedef struct {
    const char *k;
    char t;
    size_t off, sz;
} cfg_ent;
#define E(n, t) {#n, t, offsetof(cfg_t, n), sizeof(((cfg_t *)0)->n)}
static const cfg_ent ents[] = {
    E(freq_hz, 'l'),      E(ppm, 'i'),          E(gain, 's'),         E(rtl_dev, 'i'),
    E(bias_tee, 'i'),     E(rtlsdr_lib, 's'),   E(scan_lo_hz, 'l'),   E(scan_hi_hz, 'l'),
    E(sdr, 's'),          E(hackrf_lib, 's'),   E(hackrf_gain, 's'),
    E(scan_step_hz, 'l'), E(gpsd_host, 's'),    E(gpsd_port, 'i'),    E(ground_alt, 'd'),
    E(burst_alt, 'd'),    E(descent_v0, 'd'),   E(cog_min_speed, 'd'), E(mag_enable, 'i'),
    E(mag_off_x, 'd'),    E(mag_off_y, 'd'),    E(mag_rot_deg, 'd'),  E(mag_flip_y, 'i'),
    E(mag_off_z, 'd'),    E(mag_tilt, 'i'),     E(accel_axes, 's'),
    E(declination, 'd'),  E(stale_s, 'd'),      E(data_dir, 's'),     E(kbd_dev, 's'),
    E(fb_dev, 's'),       E(prox_radius_m, 'd'), E(prox_hyst_m, 'd'), E(audio_enable, 'i'),
    E(led_path, 's'),     E(ext_power_path, 's'), E(gps_serial, 's'),
    E(audio_cmd, 's'),    E(tile_dir, 's'),     E(zoom_min, 'i'),     E(zoom_max, 'i'),
    E(map_lat, 'd'),      E(map_lon, 'd'),      E(map_simple, 'i'),
    E(font_dir, 's'),     E(sdr_check, 'i'),    E(demo, 'i'),         E(demo_speed, 'd'),
    E(decoder_feed_cmd, 's'),
    E(tile_online, 'i'),  E(tile_url, 's'),     E(tile_user_agent, 's'), E(tile_fetch_cmd, 's'),
    E(tile_cache_dir, 's'), E(tile_cache_mb, 'i'),
    E(callsign, 's'),     E(sondehub_upload, 'i'), E(sondehub_chase, 'i'), E(sondehub_dev, 'i'),
    E(sondehub_rate_s, 'i'), E(sondehub_url, 's'), E(sondehub_cmd, 's'),
};

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* Values the app could not work with, from a hand-edited config: put back to something usable, with a warning. */
static void cfg_check(cfg_t *c)
{
    cfg_t d;
    cfg_defaults(&d);
    if (c->scan_lo_hz <= 0 || c->scan_hi_hz <= c->scan_lo_hz) {
        fprintf(stderr, "sondechase: scan_lo_hz must be below scan_hi_hz; using %ld..%ld\n", d.scan_lo_hz, d.scan_hi_hz);
        c->scan_lo_hz = d.scan_lo_hz;
        c->scan_hi_hz = d.scan_hi_hz;
    }
    if (c->scan_step_hz < 1000) c->scan_step_hz = 1000;
    if ((c->scan_hi_hz - c->scan_lo_hz) / c->scan_step_hz > SPEC_MAX) /* the overview holds SPEC_MAX cells */
        c->scan_step_hz = (c->scan_hi_hz - c->scan_lo_hz + SPEC_MAX - 1) / SPEC_MAX;
    if (c->ppm < -1000 || c->ppm > 1000) {
        fprintf(stderr, "sondechase: ppm %d is out of range; using 0\n", c->ppm);
        c->ppm = 0;
    }
    if (c->zoom_min < 0) c->zoom_min = 0;
    if (c->zoom_max > 19) c->zoom_max = 19;
    if (c->zoom_min > c->zoom_max) c->zoom_min = c->zoom_max;
    if (c->tile_cache_mb < 1) c->tile_cache_mb = 1;
    if (c->sondehub_rate_s < 10) c->sondehub_rate_s = 10;
    if (c->sondehub_rate_s > 300) c->sondehub_rate_s = 300;
    callsign_clean(c->callsign);
}

void callsign_clean(char *s)
{
    size_t i, o = 0;
    for (i = 0; s[i] && o < 16; i++) {
        char ch = (char)toupper((unsigned char)s[i]);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-') s[o++] = ch;
    }
    s[o] = 0;
}

int cfg_load(cfg_t *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s;
        size_t len = strlen(line);
        if (len == sizeof line - 1 && line[len - 1] != '\n') { /* too long: skip it, not parse its tail as a line */
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {}
            fprintf(stderr, "sondechase: config line over %zu characters ignored\n", sizeof line - 2);
            continue;
        }
        s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = eq + 1, *h;
        size_t i, n = sizeof ents / sizeof ents[0];
        for (h = v; (h = strchr(h, '#')) != NULL; h++) /* "value   # comment": a # after a space ends the value */
            if (h > v && isspace((unsigned char)h[-1])) {
                *h = 0;
                break;
            }
        v = trim(v);
        for (i = 0; i < n; i++) {
            if (strcmp(k, ents[i].k)) continue;
            char *p = (char *)c + ents[i].off;
            switch (ents[i].t) {
            case 'l': *(long *)(void *)p = strtol(v, NULL, 10); break;
            case 'i': *(int *)(void *)p = (int)strtol(v, NULL, 10); break;
            case 'd': *(double *)(void *)p = strtod(v, NULL); break;
            default: /* a cut command line would run something else: keep the default */
                if (strlen(v) >= ents[i].sz) fprintf(stderr, "sondechase: %s is longer than %zu characters, ignored\n", k, ents[i].sz - 1);
                else snprintf(p, ents[i].sz, "%s", v);
                break;
            }
            break;
        }
        if (i == n) fprintf(stderr, "sondechase: unknown config key '%s'\n", k);
    }
    fclose(f);
    cfg_check(c);
    return 0;
}

void fb_pick(char *out, size_t n, const char *cfg_dev, const char *env, const char *lcd_link, const char *procfb)
{
    char line[128], name[64];
    int idx;
    FILE *f;
    if (cfg_dev && cfg_dev[0]) { snprintf(out, n, "%s", cfg_dev); return; }
    if (env && env[0]) { snprintf(out, n, "%s", env); return; }
    if (access(lcd_link, F_OK) == 0) { snprintf(out, n, "%s", lcd_link); return; }
    snprintf(out, n, "/dev/fb0");
    if (!(f = fopen(procfb, "r"))) return;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%d %63s", &idx, name) == 2 && (strstr(name, "st7789") || strstr(name, "mipi-dbi"))) {
            snprintf(out, n, "/dev/fb%d", idx);
            break;
        }
    fclose(f);
}

int str_subst(char *out, size_t n, const char *tpl, const char *key, const char *val)
{
    const char *p = strstr(tpl, key);
    int len = p ? snprintf(out, n, "%.*s%s%s", (int)(p - tpl), tpl, val, p + strlen(key))
                : snprintf(out, n, "%s", tpl);
    return len >= 0 && (size_t)len < n;
}

int sc_subst(char *out, size_t n, const char *tpl, long f_hz)
{
    size_t o = 0;
    while (*tpl) {
        char val[64];
        const char *end = (*tpl == '{') ? strchr(tpl, '}') : NULL;
        int w = -1;
        if (end) {
            size_t kl = (size_t)(end - tpl - 1);
            const char *k = tpl + 1;
#define IS(name) (kl == strlen(name) && !strncmp(k, name, kl))
            if (IS("f_hz")) w = snprintf(val, sizeof val, "%ld", f_hz);
#undef IS
        }
        if (w >= 0) {
            if (o + (size_t)w >= n) return -1;
            memcpy(out + o, val, (size_t)w);
            o += (size_t)w;
            tpl = end + 1;
        } else {
            if (o + 1 >= n) return -1;
            out[o++] = *tpl++;
        }
    }
    out[o] = 0;
    return 0;
}

/* ---------------- line reader ---------------- */
int lr_readline(lr_t *r, char *out, size_t n)
{
    for (;;) {
        char *nl = memchr(r->buf, '\n', r->len);
        if (nl && r->skip) { /* the tail of an over-long line: not a line of its own */
            size_t l = (size_t)(nl - r->buf);
            memmove(r->buf, nl + 1, r->len - l - 1);
            r->len -= l + 1;
            r->skip = 0;
            continue;
        }
        if (nl) {
            size_t l = (size_t)(nl - r->buf);
            size_t cp = l < n - 1 ? l : n - 1;
            memcpy(out, r->buf, cp);
            out[cp] = 0;
            memmove(r->buf, nl + 1, r->len - l - 1);
            r->len -= l + 1;
            return 1;
        }
        if (r->len == sizeof r->buf) { /* over-long line: drop it */
            r->len = 0;
            r->skip = 1;
        }
        if (r->fd < 0) return -1;
        ssize_t k = read(r->fd, r->buf + r->len, sizeof r->buf - r->len);
        if (k > 0) {
            r->len += (size_t)k;
            continue;
        }
        if (k == 0) return -1;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }
}

/* ---------------- child process ---------------- */
void sc_child_setup(void)
{
    sigset_t none;
    setsid();
    signal(SIGPIPE, SIG_DFL);
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

void proc_init(proc_t *p)
{
    p->pid = 0;
    p->lr.fd = -1;
    p->lr.len = 0;
    p->lr.skip = 0;
    p->wfd = -1;
    p->wdrop = 0;
}

static int proc_spawn(proc_t *p, const char *cmd, int feed)
{
    int pf[2], in[2] = {-1, -1};
    if (pipe(pf) < 0) return -1;
    if (feed && pipe(in) < 0) {
        close(pf[0]);
        close(pf[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pf[0]);
        close(pf[1]);
        if (feed) {
            close(in[0]);
            close(in[1]);
        }
        return -1;
    }
    if (pid == 0) {
        sc_child_setup();
        dup2(pf[1], 1);
        close(pf[0]);
        close(pf[1]);
        if (feed) {
            dup2(in[0], 0);
            close(in[0]);
            close(in[1]);
        } else {
            int nul = open("/dev/null", O_RDONLY);
            if (nul >= 0) dup2(nul, 0);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid); /* also here: a proc_stop right after the fork must reach the group */
    close(pf[1]);
    fcntl(pf[0], F_SETFL, O_NONBLOCK);
    fcntl(pf[0], F_SETFD, FD_CLOEXEC);
    p->pid = pid;
    p->lr.fd = pf[0];
    p->lr.len = 0;
    p->lr.skip = 0;
    p->wdrop = 0;
    if (feed) {
        close(in[0]);
        fcntl(in[1], F_SETFL, O_NONBLOCK);
        fcntl(in[1], F_SETFD, FD_CLOEXEC);
        p->wfd = in[1];
    }
    return 0;
}

int proc_start(proc_t *p, const char *cmd)
{
    return proc_spawn(p, cmd, 0);
}

int proc_start_feed(proc_t *p, const char *cmd)
{
    return proc_spawn(p, cmd, 1);
}

/* Never blocks the UI loop. Pipe writes of up to PIPE_BUF bytes are atomic, so a chunk the child has
 * no room for is dropped whole (and counted) and the stream keeps its sample alignment. */
void proc_feed(proc_t *p, const void *buf, size_t n)
{
    const char *b = buf;
    while (n > 0 && p->wfd >= 0) {
        size_t k = n < PIPE_BUF ? n : PIPE_BUF;
        if (write(p->wfd, b, k) != (ssize_t)k) p->wdrop += k; /* EAGAIN: full; EPIPE: child gone */
        b += k;
        n -= k;
    }
}

/* Pipeline members outlive the shell that led their group; as a child subreaper (main.c) this app
 * inherits them and must collect them, or they stay zombies under the launcher. */
void reap_group(pid_t pg)
{
    int i;
    for (i = 0; i < 50; i++) { /* up to 0.5 s for members still exiting */
        pid_t r = waitpid(-pg, NULL, WNOHANG);
        if (r < 0) return; /* none of our children left in the group */
        if (r > 0) continue;
        if (i == 25) kill(-pg, SIGKILL);
        usleep(10000);
    }
}

void proc_stop(proc_t *p)
{
    if (p->wfd >= 0) close(p->wfd); /* EOF first: a feed child can finish cleanly */
    if (p->pid > 0) {
        int i, done = 0;
        kill(-p->pid, SIGTERM);
        for (i = 0; i < 20 && !done; i++) {
            if (waitpid(p->pid, NULL, WNOHANG) == p->pid) done = 1;
            else usleep(50000);
        }
        if (!done) {
            kill(-p->pid, SIGKILL);
            waitpid(p->pid, NULL, 0);
        } else {
            kill(-p->pid, SIGKILL); /* remaining pipeline members, if any */
        }
        reap_group(p->pid);
    }
    if (p->lr.fd >= 0) close(p->lr.fd);
    proc_init(p);
}

/* ---------------- flat JSON ---------------- */
static const char *js_find(const char *s, const char *key)
{
    size_t kl = strlen(key);
    const char *p = s;
    while ((p = strchr(p, '"')) != NULL) {
        if (!strncmp(p + 1, key, kl) && p[1 + kl] == '"') {
            const char *q = p + kl + 2;
            while (*q == ' ') q++;
            if (*q == ':') {
                q++;
                while (*q == ' ') q++;
                return q;
            }
        }
        p++;
    }
    return NULL;
}

int js_num(const char *s, const char *key, double *out)
{
    const char *v = js_find(s, key);
    char *e;
    if (!v) return 0;
    double d = strtod(v, &e);
    if (e == v) return 0;
    *out = d;
    return 1;
}

int js_str(const char *s, const char *key, char *out, size_t n)
{
    const char *v = js_find(s, key);
    size_t o = 0;
    if (!v || *v != '"') return 0;
    for (v++; *v && *v != '"' && o + 1 < n; v++) out[o++] = *v;
    out[o] = 0;
    return 1;
}
