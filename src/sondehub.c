/* SondeHub upload: decoded RS41 frames to /sondes/telemetry (as radiosonde_auto_rx sends them) and this device's
 * position to /listeners as a chase car (as chasemapper does). The request runs in a child, sondehub_cmd (curl),
 * polled like the tile downloads, so the UI never waits on the network. */
#define _GNU_SOURCE
#include "sc.h"
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SH_SOFTWARE "SondeChase"
#define SH_BODY_MAX 65536 /* a full batch of SH_QMAX frames is about 30 kB */

void sh_init(sondehub_t *h, const cfg_t *c)
{
    memset(h, 0, sizeof *h);
    snprintf(h->dir, sizeof h->dir, "%s", c->data_dir[0] ? c->data_dir : "/tmp/sondechase");
}

int sh_add(sondehub_t *h, const cfg_t *c, const sonde_t *s, long freq_hz)
{
    struct timespec ts;
    /* the server rejects a frame without the sonde's own time; local time must never stand in for it */
    if (!c->sondehub_upload || !c->callsign[0] || !s->datetime[0] || !strcmp(s->id, "?")) return 0;
    if (h->qn == SH_QMAX) { /* realtime: the oldest frame goes */
        memmove(&h->q[0], &h->q[1], sizeof h->q[0] * (SH_QMAX - 1));
        h->qn--;
    }
    clock_gettime(CLOCK_REALTIME, &ts);
    h->q[h->qn].s = *s;
    h->q[h->qn].freq_hz = freq_hz;
    h->q[h->qn].t_unix = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    h->qn++;
    return 1;
}

/* Appends to a bounded buffer; any overflow sticks, so the caller checks once at the end. */
typedef struct { char *p; size_t n, o; int over; } jb_t;
static void jb(jb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb(jb_t *b, const char *fmt, ...)
{
    va_list ap;
    int w;
    if (b->over) return;
    va_start(ap, fmt);
    w = vsnprintf(b->p + b->o, b->n - b->o, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= b->n - b->o) b->over = 1;
    else b->o += (size_t)w;
}

/* Text from the decoder in a JSON string: anything but letters, digits and - _ . : becomes _ */
static void safe(char *out, size_t n, const char *in)
{
    size_t o = 0;
    for (; *in && o + 1 < n; in++)
        out[o++] = (isalnum((unsigned char)*in) || strchr("-_.:", *in)) ? *in : '_';
    out[o] = 0;
}

static void iso_utc(char *out, size_t n, double t_unix)
{
    long long ms = (long long)(t_unix * 1000.0 + 0.5); /* rounded: 05.099 must not come out as 05.098 */
    time_t s = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&s, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(out + strlen(out), n - strlen(out), ".%03dZ", (int)(ms % 1000));
}

int sh_json_tel(const sondehub_t *h, const cfg_t *c, const gps_t *g, double now, char *out, size_t n)
{
    jb_t b = {out, n, 0, 0};
    int i, pos = c->sondehub_chase && gps_ok(g, now);
    jb(&b, "[");
    for (i = 0; i < h->qn; i++) {
        const sonde_t *s = &h->q[i].s;
        char rx[32], dt[32], id[20], sub[16];
        /* the sonde's GPS time exactly as the decoder gave it: SondeHub asks for it unchanged */
        safe(dt, sizeof dt, s->datetime);
        iso_utc(rx, sizeof rx, h->q[i].t_unix);
        safe(id, sizeof id, s->id);
        jb(&b, "%s{\"software_name\":\"" SH_SOFTWARE "\",\"software_version\":\"" SC_VERSION "\","
               "\"uploader_callsign\":\"%s\",\"time_received\":\"%s\",\"manufacturer\":\"Vaisala\",\"type\":\"RS41\","
               "\"serial\":\"%s\",\"frame\":%d,\"datetime\":\"%s\",\"lat\":%.5f,\"lon\":%.5f,\"alt\":%.1f,"
               "\"frequency\":%.3f",
           i ? "," : "", c->callsign, rx, id, s->frame, dt, s->lat, s->lon, s->alt, h->q[i].freq_hz / 1e6);
        if (s->subtype[0]) {
            safe(sub, sizeof sub, s->subtype);
            jb(&b, ",\"subtype\":\"%s\"", sub);
        }
        /* optional fields only when the sonde measured them: the ISA estimate is not a pressure reading */
        if (s->temp > -273.0) jb(&b, ",\"temp\":%.1f", s->temp);
        if (s->humidity >= 0.0) jb(&b, ",\"humidity\":%.1f", s->humidity);
        if (!s->pres_est && s->pressure > 0.0) jb(&b, ",\"pressure\":%.1f", s->pressure);
        jb(&b, ",\"vel_h\":%.1f,\"vel_v\":%.1f,\"heading\":%.1f,\"sats\":%d", s->vel_h, s->vel_v, s->heading, s->sats);
        if (s->batt >= 0.0) jb(&b, ",\"batt\":%.1f", s->batt);
        if (s->bt >= 0 && s->bt < 65535) jb(&b, ",\"burst_timer\":%d", s->bt);
        if (pos) jb(&b, ",\"uploader_position\":[%.5f,%.5f,%.1f]", g->lat, g->lon, g->alt);
        if (c->sondehub_dev) jb(&b, ",\"dev\":true");
        jb(&b, "}");
    }
    jb(&b, "]");
    return b.over ? -1 : (int)b.o;
}

int sh_json_lis(const cfg_t *c, const gps_t *g, char *out, size_t n)
{
    int w = snprintf(out, n,
                     "{\"software_name\":\"" SH_SOFTWARE "\",\"software_version\":\"" SC_VERSION "\","
                     "\"uploader_callsign\":\"%s_chase\",\"uploader_position\":[%.5f,%.5f,%.1f],"
                     "\"uploader_antenna\":\"Chase Car\",\"mobile\":true}",
                     c->callsign, g->lat, g->lon, g->alt);
    return w < 0 || (size_t)w >= n ? -1 : w;
}

static void fail(sh_result_t *r, double now, const char *why)
{
    r->t = now;
    r->ok = 0;
    r->http = 0;
    snprintf(r->why, sizeof r->why, "%s", why);
}

/* Write the body, then run sondehub_cmd with it on stdin (gzipped first for telemetry) and its stdout, the HTTP
 * code, in sondehub-<kind>.code. The child's pid, -1 = not started. */
static pid_t start(const sondehub_t *h, const cfg_t *c, const char *kind, const char *path, int gz, const char *body,
                   size_t len)
{
    char in[260], code[260], resp[260], url[200], date[40], a[1024], b[1024];
    int w;
    time_t t = time(NULL);
    struct tm tm;
    FILE *f;
    pid_t pid;
    snprintf(in, sizeof in, "%s/sondehub-%s.json", h->dir, kind);
    snprintf(code, sizeof code, "%s/sondehub-%s.code", h->dir, kind);
    snprintf(resp, sizeof resp, "%s/sondehub-%s.resp", h->dir, kind);
    snprintf(url, sizeof url, "%s%s", c->sondehub_url, path);
    gmtime_r(&t, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm); /* RFC 7231 */
    /* a cut command would send the wrong thing: skip the upload instead */
    if (!str_subst(a, sizeof a, c->sondehub_cmd, "{enc}", gz ? "-H 'Content-Encoding: gzip'" : "") ||
        !str_subst(b, sizeof b, a, "{date}", date) || !str_subst(a, sizeof a, b, "{ua}", SH_SOFTWARE "-" SC_VERSION) ||
        !str_subst(b, sizeof b, a, "{url}", url) || !str_subst(a, sizeof a, b, "{resp}", resp))
        return -1;
    if ((w = snprintf(b, sizeof b, "%s%s", gz ? "gzip -c | " : "", a)) < 0 || (size_t)w >= sizeof b) return -1;
    mkdirs(in);
    unlink(code);
    unlink(resp);
    if (!(f = fopen(in, "w"))) return -1;
    if (fwrite(body, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) return -1;
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fi, fo;
        sc_child_setup();
        fi = open(in, O_RDONLY);
        fo = open(code, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fi < 0 || fo < 0) _exit(126);
        dup2(fi, 0);
        dup2(fo, 1);
        execl("/bin/sh", "sh", "-c", b, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    return pid;
}

/* The child's exit status and the HTTP code it printed, as a result. curl: 6 = name not resolved, 7 = no
 * connection, 28 = timed out. Only 200 is a clean upload: SondeHub answers 201/202 when it rejected some or all of
 * the frames, with the reason in the body (radiosonde_auto_rx logs it the same way). */
static void finish(const sondehub_t *h, const char *kind, int st, double now, sh_result_t *r)
{
    char p[260], body[512] = "", msg[40];
    FILE *f;
    int code = 0, k;
    r->t = now;
    r->ok = 0;
    r->http = 0;
    snprintf(p, sizeof p, "%s/sondehub-%s.code", h->dir, kind);
    if ((f = fopen(p, "r"))) {
        if (fscanf(f, "%d", &code) != 1) code = 0;
        fclose(f);
    }
    k = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (k == 0 && code == 200) {
        r->ok = 1;
        r->http = code;
        r->why[0] = 0;
        return;
    }
    if (k == 0 && code > 0) { /* the server's own words, for the 201/202/4xx/5xx it sends back with a reason */
        size_t i, n;
        snprintf(p, sizeof p, "%s/sondehub-%s.resp", h->dir, kind);
        if ((f = fopen(p, "r"))) {
            n = fread(body, 1, sizeof body - 1, f);
            body[n] = 0;
            for (i = 0; i < n; i++)
                if (!isprint((unsigned char)body[i])) body[i] = ' ';
            fclose(f);
        }
        r->http = code;
        /* SondeHub lists why each frame was refused, e.g. "This software is unknown to us!" */
        if (js_str(body, "error_message", msg, sizeof msg)) snprintf(r->why, sizeof r->why, "HTTP %d %.30s", code, msg);
        else snprintf(r->why, sizeof r->why, "HTTP %d %s", code, body);
    } else if (k == 6 || k == 7) snprintf(r->why, sizeof r->why, "No network");
    else if (k == 28) snprintf(r->why, sizeof r->why, "Timeout");
    else if (k == 127) snprintf(r->why, sizeof r->why, "curl not found");
    else if (k == 0) snprintf(r->why, sizeof r->why, "No answer");
    else snprintf(r->why, sizeof r->why, "Error %d", k);
}

static int reap(pid_t *pid, int *st)
{
    if (*pid <= 0 || waitpid(*pid, st, WNOHANG) != *pid) return 0;
    reap_group(*pid);
    *pid = 0;
    return 1;
}

void sh_poll(sondehub_t *h, const cfg_t *c, const gps_t *g, double now)
{
    static char body[SH_BODY_MAX];
    int st, len;
    if (reap(&h->tel_pid, &st)) {
        finish(h, "tel", st, now, &h->last_tel);
        h->last_tel.n = h->tel_n;
    }
    if (reap(&h->lis_pid, &st)) finish(h, "lis", st, now, &h->last_lis);
    if (!c->sondehub_upload || !c->callsign[0]) h->qn = 0; /* switched off: nothing left to go later */
    if (h->tel_pid <= 0 && h->qn > 0 && now >= h->t_next_tel) {
        h->t_next_tel = now + c->sondehub_rate_s;
        h->tel_n = h->qn;
        if ((len = sh_json_tel(h, c, g, now, body, sizeof body)) < 0 ||
            (h->tel_pid = start(h, c, "tel", "/sondes/telemetry", 1, body, (size_t)len)) < 0) {
            h->tel_pid = 0;
            fail(&h->last_tel, now, "Could not start");
            h->last_tel.n = h->tel_n;
        }
        h->qn = 0; /* sent or dropped: realtime only */
    }
    if (c->sondehub_chase && c->callsign[0] && h->lis_pid <= 0 && now >= h->t_next_lis && gps_ok(g, now)) {
        h->t_next_lis = now + SH_CHASE_S;
        if ((len = sh_json_lis(c, g, body, sizeof body)) < 0 ||
            (h->lis_pid = start(h, c, "lis", "/listeners", 0, body, (size_t)len)) < 0) {
            h->lis_pid = 0;
            fail(&h->last_lis, now, "Could not start");
        }
    }
}

void sh_close(sondehub_t *h)
{
    pid_t *p[2] = {&h->tel_pid, &h->lis_pid};
    int i;
    for (i = 0; i < 2; i++)
        if (*p[i] > 0) {
            kill(-*p[i], SIGTERM);
            waitpid(*p[i], NULL, 0);
            reap_group(*p[i]);
            *p[i] = 0;
        }
}
