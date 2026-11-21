/* Shared by the UI files (ui.c and ui_<screen>.c): colours, layout constants, the state every screen
 * reads, the drawing primitives and one entry point per screen. Not part of the core: includes LVGL. */
#ifndef UI_PRIV_H
#define UI_PRIV_H
#include "ui.h"
#include "lvgl.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define C_G 0x0A0614  /* ground */
#define C_P 0x150D2A  /* panel */
#define C_L 0x3A2466  /* grid line */
#define C_CY 0x19E6FF /* this device, chrome */
#define C_PK 0xFF3D9A /* sonde */
#define C_YL 0xFFE14A /* prediction */
#define C_RD 0xFF6A4D /* signal lost */
#define C_GN 0x6CFF9A /* wind */
#define C_TX 0xE9E4FF
#define C_DIM 0xA79BD1
#define C_AD 0x27407F /* dry adiabats */

enum { PG_TRACK, PG_MAP, PG_GRAPH, PG_RADIO, PG_INFO };
enum { AL_L = LV_TEXT_ALIGN_LEFT, AL_C = LV_TEXT_ALIGN_CENTER, AL_R = LV_TEXT_ALIGN_RIGHT };

extern app_t *A;
extern lv_layer_t *LY;                                    /* the layer of the frame being drawn */
extern const lv_font_t *F_S, *F_M, *F_NUM, *F_BIG, *F_TTL; /* 11, 12, Orbitron 17 / 30 / 25 */
extern int cal_saved;                                     /* 1 saved, -1 could not write compass.conf */
extern double cal_t;                                      /* when; the result is shown for a few seconds */
enum { SH_NOTE_SAVED = 1, SH_NOTE_NOCALL, SH_NOTE_SAVE_FAIL };
extern int sh_note;                                       /* Info 3/3: SH_NOTE_* after U, C or E, 0 = none */
extern double sh_note_t;
extern int map_follow, map_zoom;                          /* 0 both, 1 device, 2 sonde, 3 panned; -1 = auto */

/* ui_draw.c: primitives, helpers and the chrome */
void ln(double x1, double y1, double x2, double y2, uint32_t col, int w);
void dash(double x1, double y1, double x2, double y2, uint32_t col, int w, double on, double off);
void ln_clip(double x1, double y1, double x2, double y2, int cx0, int cy0, int cx1, int cy1,
             uint32_t col, int w);
void fill_opa(int x, int y, int w, int h, uint32_t col, lv_opa_t opa);
void fill(int x, int y, int w, int h, uint32_t col);
void txt(int x, int y, int w, const lv_font_t *f, uint32_t col, int align, const char *fmt, ...);
int text_w(const lv_font_t *f, const char *s);
void box(int x, int y, int w, int h, uint32_t col, uint32_t bg);
void kv(int x, int y, int w, const char *k, uint32_t vc, const char *fmt, ...);
void pol(double cx, double cy, double r, double deg, double *x, double *y);
void fmt_dist(char *num, size_t n, const char **unit, double m);
int fresh(void);
int land_rem_s(void);
const char *land_rem_txt(void);
void read_battery(void);
void top_bar(const char *name, uint32_t chip, const char *mid);
void nav_bar(int active);

/* ui_track.c */
void compass(double hdg, int have_rel, double rel, uint32_t col, int dashed, int have_land, double land_rel,
             uint32_t ring);
const char *phase_name(void);
void p_track(void);

/* ui_map.c */
void p_map(void);
void map_pan(int k);
void map_zoom_step(int d);
void map_follow_next(void);
void map_style_toggle(void);

/* ui_graph.c */
void p_skewt(void);
void p_flight(void);

/* ui_radio.c */
void p_scan(void);

/* ui_info.c */
void p_cal(void);
void p_info_pos(void);
void p_info_sondehub(void);
void p_info(void);
#endif
