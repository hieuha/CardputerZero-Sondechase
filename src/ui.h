/* LVGL user interface (320x170). Drawn in immediate mode from one draw callback. */
#ifndef UI_H
#define UI_H
#include "sc.h"

enum { K_UP = 256, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESC, K_BKSP };

void ui_init(app_t *a);
void ui_key(int key);      /* ASCII (lower case) or K_* */
int ui_should_quit(void);

int keys_open(const char *dev); /* "" = auto-detect; returns fd or -1 */
int keys_read(int fd);          /* next key press, 0 = none */
int keys_esc_held(double secs); /* Esc has been held down for at least secs */
#endif
