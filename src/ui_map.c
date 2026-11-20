/* Map screen: tile cache, the map with both positions, and panning, zoom and follow. */
#define _GNU_SOURCE
#include "ui_priv.h"
#include "lvgl_private.h" /* lv_image_decoder_dsc_t layout, lv_image_cache_drop */
#include <unistd.h>

/* Decoded map tiles kept in RAM as RGB565. Pixel buffers are static, so a descriptor handed
 * to LVGL stays valid for the whole frame even when another slot is replaced. */
#define TC_N 12
static struct { int z, x, y, valid; unsigned stamp; lv_image_dsc_t dsc; } tc[TC_N];
static uint8_t tc_px[TC_N][TILE_PX * TILE_PX * 2];
static unsigned tc_stamp;

int map_follow, map_zoom = -1;

/* ---------------- 5 Map ---------------- */
static const lv_image_dsc_t *tile_get(int z, int x, int y)
{
    static unsigned char rgb[TILE_PX * TILE_PX * 3];
    char path[320], fsp[330];
    lv_image_decoder_dsc_t dec;
    int i, slot = 0, kind, ok = 0;
    for (i = 0; i < TC_N; i++)
        if (tc[i].valid && tc[i].z == z && tc[i].x == x && tc[i].y == y) {
            tc[i].stamp = ++tc_stamp;
            return &tc[i].dsc;
        }
    kind = tf_find(&A->tf, &A->cfg, z, x, y, path, sizeof path);
    if (!kind) return NULL;
    snprintf(fsp, sizeof fsp, "A:%s", path);
    if (lv_image_decoder_open(&dec, fsp, NULL) == LV_RESULT_OK) { /* LVGL's PNG decoder (lodepng) */
        const lv_draw_buf_t *b = dec.decoded;
        int cf = b ? (int)b->header.cf : -1, bpp = cf == LV_COLOR_FORMAT_RGB888 ? 3 : 4, xx, yy;
        if (b && b->header.w == TILE_PX && b->header.h == TILE_PX &&
            (cf == LV_COLOR_FORMAT_ARGB8888 || cf == LV_COLOR_FORMAT_XRGB8888 || cf == LV_COLOR_FORMAT_RGB888)) {
            for (yy = 0; yy < TILE_PX; yy++) {
                const uint8_t *row = b->data + (size_t)yy * b->header.stride; /* stored as B, G, R(, A) */
                for (xx = 0; xx < TILE_PX; xx++) {
                    rgb[3 * (yy * TILE_PX + xx)] = row[xx * bpp + 2];
                    rgb[3 * (yy * TILE_PX + xx) + 1] = row[xx * bpp + 1];
                    rgb[3 * (yy * TILE_PX + xx) + 2] = row[xx * bpp];
                }
            }
            ok = 1;
        }
        lv_image_decoder_close(&dec);
        lv_image_cache_drop(fsp); /* the RGB565 copy below is the only one kept */
    }
    if (!ok) {
        if (kind == 2) unlink(path); /* damaged download: fetch it again */
        return NULL;
    }
    if (kind == 2) /* downloaded tiles keep the server's light colours on disk */
        tile_recolor(rgb, TILE_PX * TILE_PX, A->cfg.map_simple);
    for (i = 0; i < TC_N; i++) {
        if (!tc[i].valid) { slot = i; break; }
        if (tc[i].stamp < tc[slot].stamp) slot = i;
    }
    for (i = 0; i < TILE_PX * TILE_PX; i++) {
        uint16_t v = (uint16_t)(((rgb[3 * i] >> 3) << 11) | ((rgb[3 * i + 1] >> 2) << 5) | (rgb[3 * i + 2] >> 3));
        tc_px[slot][2 * i] = (uint8_t)(v & 0xff);
        tc_px[slot][2 * i + 1] = (uint8_t)(v >> 8);
    }
    lv_image_cache_drop(&tc[slot].dsc);
    memset(&tc[slot].dsc, 0, sizeof tc[slot].dsc);
    tc[slot].dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    tc[slot].dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    tc[slot].dsc.header.w = TILE_PX;
    tc[slot].dsc.header.h = TILE_PX;
    tc[slot].dsc.header.stride = TILE_PX * 2;
    tc[slot].dsc.data = tc_px[slot];
    tc[slot].dsc.data_size = sizeof tc_px[slot];
    tc[slot].z = z;
    tc[slot].x = x;
    tc[slot].y = y;
    tc[slot].valid = 1;
    tc[slot].stamp = ++tc_stamp;
    return &tc[slot].dsc;
}

static double m_cx, m_cy; /* map centre in world pixels at the current zoom */
static double m_clat, m_clon, pan_lat, pan_lon; /* centre drawn last; centre while panned (map_follow == 3) */
static int follow_before_pan; /* the follow mode C returns to */
static int m_z;

/* Screen position; across the date line the nearer copy of the world, so a sonde at 179.9 E seen from 179.9 W is
 * just next to the device, not a world-width away. */
static void m_xy(double lat, double lon, double *x, double *y)
{
    double tx, ty, dx, world = TILE_PX * (double)(1 << m_z);
    tile_frac(lat, lon, m_z, &tx, &ty);
    dx = tx * TILE_PX - m_cx;
    *x = dx - world * floor(dx / world + 0.5) + 160.0;
    *y = ty * TILE_PX - m_cy + 86.0;
}

void p_map(void)
{
    const gps_t *g = &A->gps;
    const sonde_t *s = &A->sonde;
    int have_me = gps_ok(g, sc_now()), have_s = A->have_sonde, i, tx, ty, any = 0, missing = 0, loading = 0;
    double clat, clon, x, y, x2, y2, tfx, tfy;
    static const char *fm[] = {"both", "device", "sonde", "free"};

    if (map_follow == 3) { clat = pan_lat; clon = pan_lon; } /* panned by hand */
    else if (have_me && (map_follow == 1 || !have_s)) { clat = g->lat; clon = g->lon; }
    else if (have_s && (map_follow == 2 || !have_me)) { clat = s->lat; clon = s->lon; }
    else if (have_me && have_s) { clat = (g->lat + s->lat) / 2; clon = norm180(g->lon + norm180(s->lon - g->lon) / 2); }
    else { clat = A->cfg.map_lat; clon = A->cfg.map_lon; } /* no position yet: still show the map */
    m_z = map_zoom >= 0 ? map_zoom
        : (have_me && have_s && map_follow == 0)
              ? tile_fit_zoom(g->lat, g->lon, s->lat, g->lon + norm180(s->lon - g->lon), 260, 100, A->cfg.zoom_min,
                              A->cfg.zoom_max) /* the short way round, also across the date line */
              : (A->cfg.zoom_max < 13 ? A->cfg.zoom_max : 13);
    m_clat = clat;
    m_clon = clon;
    tile_frac(clat, clon, m_z, &tfx, &tfy);
    m_cx = tfx * TILE_PX;
    m_cy = tfy * TILE_PX;

    /* tiles: preloaded pack first, then the download cache; a missing tile is requested from the
     * network only now, while the map is showing it */
    for (ty = (int)floor((m_cy - 70) / TILE_PX); ty <= (int)floor((m_cy + 70) / TILE_PX); ty++)
        for (tx = (int)floor((m_cx - 160) / TILE_PX); tx <= (int)floor((m_cx + 160) / TILE_PX); tx++) {
            const lv_image_dsc_t *img;
            int x0 = (int)floor(tx * TILE_PX - m_cx + 160.0), y0 = (int)floor(ty * TILE_PX - m_cy + 86.0);
            int wx = ((tx % (1 << m_z)) + (1 << m_z)) % (1 << m_z); /* the world repeats east and west */
            if (ty < 0 || ty >= (1 << m_z)) continue;
            img = tile_get(m_z, wx, ty);
            if (img) {
                lv_draw_image_dsc_t d;
                lv_area_t a = {x0, y0, x0 + TILE_PX - 1, y0 + TILE_PX - 1};
                lv_draw_image_dsc_init(&d);
                d.src = img;
                lv_draw_image(LY, &d, &a);
                any = 1;
            } else {
                missing++;
                tf_request(&A->tf, &A->cfg, m_z, wx, ty, sc_now());
                loading += tf_busy(&A->tf, m_z, wx, ty);
                for (i = 0; i < TILE_PX; i += 32) { /* empty cell: plain graticule */
                    ln_clip(x0 + i, y0, x0 + i, y0 + TILE_PX - 1, 0, 16, 319, 155, C_P, 1);
                    ln_clip(x0, y0 + i, x0 + TILE_PX - 1, y0 + i, 0, 16, 319, 155, C_P, 1);
                }
            }
        }

    for (i = 1; i < A->hist.n; i++) { /* received track */
        m_xy(A->hist.p[i - 1].lat, A->hist.p[i - 1].lon, &x, &y);
        m_xy(A->hist.p[i].lat, A->hist.p[i].lon, &x2, &y2);
        ln_clip(x, y, x2, y2, 0, 16, 319, 155, C_PK, 2);
    }
    if (have_s && A->pred.valid && fresh()) {
        m_xy(s->lat, s->lon, &x, &y);
        m_xy(A->pred.lat, A->pred.lon, &x2, &y2);
        dash(x, y, x2, y2, C_YL, 2, 3, 3);
        ln(x2 - 5, y2 - 5, x2 + 5, y2 + 5, C_YL, 2);
        ln(x2 - 5, y2 + 5, x2 + 5, y2 - 5, C_YL, 2);
        {
            double lx2 = x2 < -1000 ? -1000 : (x2 > 1000 ? 1000 : x2), ly2 = y2 < -1000 ? -1000 : (y2 > 1000 ? 1000 : y2);
            int lx = (int)lx2 + 9 + 86 > 318 ? (int)lx2 - 9 - 86 : (int)lx2 + 9, ly = (int)ly2 + 5;
            if (lx < 2) lx = 2;
            fill(lx, ly, 86, 13, C_G);
            txt(lx, ly, 86, F_S, C_YL, AL_C, "Landing T-%s", land_rem_txt());
        }
    }
    if (have_s) {
        uint32_t col = fresh() ? C_PK : C_RD;
        m_xy(s->lat, s->lon, &x, &y);
        for (i = 0; i < 7; i++) { /* filled diamond */
            ln(x - i, y - 7 + i, x + i, y - 7 + i, col, 1);
            ln(x - i, y + 7 - i, x + i, y + 7 - i, col, 1);
        }
        ln(x - 7, y, x + 7, y, col, 1);
        fill((int)x - 21, (int)y - 24, 42, 13, C_G);
        txt((int)x - 21, (int)y - 24, 42, F_S, C_TX, AL_C, "%.0f m", s->alt);
    }
    if (have_me) {
        double h = A->hdg.src != HDG_NONE ? A->hdg.deg : 0, ax, ay, bx, by, cx2, cy2, dx, dy;
        double mpp = tile_mpp(g->lat, m_z), r = 5000.0 / mpp;
        m_xy(g->lat, g->lon, &x, &y);
        if (r > 20 && r < 200)
            for (i = 0; i < 48; i += 2) { /* 5 km range ring */
                pol(x, y, r, i * 7.5, &ax, &ay);
                pol(x, y, r, i * 7.5 + 7.5, &bx, &by);
                ln_clip(ax, ay, bx, by, 0, 16, 319, 155, C_DIM, 1);
            }
        pol(x, y, 8, h, &ax, &ay);
        pol(x, y, 8, h + 143, &bx, &by);
        pol(x, y, 2, h + 180, &cx2, &cy2);
        pol(x, y, 8, h - 143, &dx, &dy);
        ln(ax, ay, bx, by, C_CY, 2);
        ln(bx, by, cx2, cy2, C_CY, 2);
        ln(cx2, cy2, dx, dy, C_CY, 2);
        ln(dx, dy, ax, ay, C_CY, 2);
    }

    top_bar("Map", C_CY, have_me || have_s ? NULL : "No position yet: default area");
    box(4, 20, 112, 16, C_L, C_P); /* "Zoom 14*" needs ~50 px */
    txt(9, 21, 56, F_S, C_TX, AL_L, "Zoom %d%s", m_z, map_zoom < 0 ? "" : "*");
    txt(66, 21, 46, F_S, C_DIM, AL_R, "%s", fm[map_follow]);
    box(120, 20, 56, 16, C_L, C_P);
    txt(124, 21, 48, F_S, C_DIM, AL_C, "%s", A->cfg.map_simple ? "simple" : "detailed");
    if (missing) { /* say why part of the map is empty */
        int failing = !loading && A->cfg.tile_online && A->tf.failed && A->tf.t_last_fail >= A->tf.t_last_ok;
        const char *why = loading ? "Loading map" : !A->cfg.tile_online ? "No map stored for this area"
                        : !failing ? "Loading map"
                        : A->tf.why == TF_WHY_DNS_BLOCKED ? "Map server blocked by DNS"
                        : A->tf.why == TF_WHY_DNS_FAIL ? "Map server name not found (DNS)"
                        : A->tf.why == TF_WHY_HTTP ? "Map server refused the download"
                                                   : "No network: stored map only";
        fill(4, 39, 200, 14, C_G);
        txt(9, 39, 194, F_S, loading ? C_CY : C_RD, AL_L, "%s", why);
        if (failing && (A->tf.why == TF_WHY_DNS_BLOCKED || A->tf.why == TF_WHY_DNS_FAIL)) {
            fill(4, 53, 312, 26, C_G); /* what to do: the domain to allow, e.g. in Pi-hole / AdGuard / the router */
            txt(9, 53, 304, F_S, C_TX, AL_L, "%s %s", A->tf.why == TF_WHY_DNS_BLOCKED ? "Allow" : "Check DNS for", A->tf.host);
            txt(9, 66, 304, F_S, C_DIM, AL_L, "%s", A->tf.why == TF_WHY_DNS_BLOCKED ? "in the DNS filter (router, Pi-hole, AdGuard, NextDNS)"
                                                                                  : "Wi-Fi is up, but this name does not resolve");
        }
    }
    if (any) { /* attribution required for OpenStreetMap-derived tiles */
        fill(112, 141, 98, 12, C_G);
        txt(112, 140, 98, F_S, C_DIM, AL_C, "© OpenStreetMap");
    }
    { /* scale bar: largest of 0.5/1/2/5/10/20/50 km that fits in 60 px */
        static const double st[] = {50000, 20000, 10000, 5000, 2000, 1000, 500, 200, 100};
        double mpp = tile_mpp(clat, m_z);
        for (i = 0; i < 8 && st[i] / mpp > 60; i++) {}
        fill(4, 138, (int)(st[i] / mpp) + 44, 15, C_G);
        ln(8, 147, 8, 150, C_TX, 2);
        ln(8, 150, 8 + st[i] / mpp, 150, C_TX, 2);
        ln(8 + st[i] / mpp, 150, 8 + st[i] / mpp, 147, C_TX, 2);
        if (st[i] >= 1000) txt((int)(12 + st[i] / mpp), 140, 40, F_S, C_TX, AL_L, "%.0f km", st[i] / 1000);
        else txt((int)(12 + st[i] / mpp), 140, 40, F_S, C_TX, AL_L, "%.0f m", st[i]);
    }
    if (A->have_geo) {
        char num[16];
        const char *unit;
        fmt_dist(num, sizeof num, &unit, A->dist);
        box(214, 136, 102, 17, C_CY, C_P);
        txt(220, 138, 60, F_S, C_TX, AL_L, "%s %s", num, unit);
        txt(280, 138, 30, F_S, C_TX, AL_R, "%03.0f°", A->brg);
    }
    nav_bar(PG_MAP);
}


/* Arrow keys on the Map: move the centre a quarter of the screen, and stop following (C follows again).
 * The zoom stays where it was, so the view only slides. */
void map_pan(int k)
{
    double n = TILE_PX * (double)(1 << m_z), px, py;
    if (map_follow != 3) {
        follow_before_pan = map_follow;
        pan_lat = m_clat;
        pan_lon = m_clon;
        map_follow = 3;
    }
    if (map_zoom < 0) map_zoom = m_z;
    tile_frac(pan_lat, pan_lon, m_z, &px, &py);
    px = px * TILE_PX + (k == K_RIGHT ? 80 : (k == K_LEFT ? -80 : 0));
    py = py * TILE_PX + (k == K_DOWN ? 40 : (k == K_UP ? -40 : 0));
    if (py < 0) py = 0;
    if (py > n) py = n;
    pan_lon = fmod(px / n * 360.0 + 360.0, 360.0) - 180.0; /* inverse Web Mercator, wrapping at 180 degrees */
    pan_lat = atan(sinh(M_PI * (1.0 - 2.0 * py / n))) * 180.0 / M_PI;
}

/* +/-: one zoom level, starting from the zoom drawn last when it was automatic. */
void map_zoom_step(int d)
{
    if (map_zoom < 0) map_zoom = m_z;
    if (d > 0 && map_zoom < A->cfg.zoom_max) map_zoom++;
    if (d < 0 && map_zoom > A->cfg.zoom_min) map_zoom--;
}

/* C: free -> follow again as before panning; else both -> device -> sonde. The zoom goes back to automatic. */
void map_follow_next(void)
{
    map_follow = map_follow == 3 ? follow_before_pan : (map_follow + 1) % 3;
    map_zoom = -1;
}

/* V: simple <-> detailed; the cached tiles are recoloured again as they are drawn. */
void map_style_toggle(void)
{
    int i;
    A->cfg.map_simple = !A->cfg.map_simple;
    for (i = 0; i < TC_N; i++) tc[i].valid = 0;
}
