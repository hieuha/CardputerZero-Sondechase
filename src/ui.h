/* LVGL user interface (320x170). Drawn in immediate mode from one draw callback. */
#ifndef UI_H
#define UI_H
#include "sc.h"

enum { K_UP = 256, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESC, K_BKSP, K_TAB };
#define K_REPEAT 0x1000 /* or-ed into an arrow key repeated while it is held */

void ui_init(app_t *a);
void ui_key(int key);      /* ASCII (lower case) or K_* */

int keys_open(const char *dev); /* "" = auto-detect; returns fd or -1 */
int keys_read(int fd);          /* next key press or held-arrow repeat, 0 = none */
double keys_esc_held_s(void);   /* seconds Esc has been held down, 0 while it is up */
#define ESC_QUIT_S 3.0          /* holding Esc this long quits the app */
#endif
