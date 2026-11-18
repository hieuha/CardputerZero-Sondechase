/* Keyboard input straight from evdev: LVGL's own keypad mapping drops letter keys. */
#define _GNU_SOURCE
#include "ui.h"
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define BIT(arr, n) ((arr[(n) / 8] >> ((n) % 8)) & 1)

static int is_keyboard(int fd)
{
    unsigned char bits[KEY_MAX / 8 + 1];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return 0;
    return BIT(bits, KEY_Q) && BIT(bits, KEY_ENTER) && BIT(bits, KEY_1);
}

int keys_open(const char *dev)
{
    char p[64];
    int i, fd;
    if (dev && dev[0]) return open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    for (i = 0; i < 16; i++) {
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (is_keyboard(fd)) {
            ioctl(fd, EVIOCGRAB, 1); /* keystrokes must not reach the console underneath */
            return fd;
        }
        close(fd);
    }
    return -1;
}

static int xlate(int c)
{
    static const struct { unsigned short code; short key; } km[] = {
        {KEY_1, '1'}, {KEY_2, '2'}, {KEY_3, '3'}, {KEY_4, '4'}, {KEY_5, '5'}, {KEY_6, '6'}, {KEY_7, '7'},
        {KEY_8, '8'}, {KEY_9, '9'}, {KEY_0, '0'}, {KEY_S, 's'}, {KEY_F, 'f'}, {KEY_R, 'r'}, {KEY_X, 'x'},
        {KEY_Q, 'q'}, {KEY_M, 'm'}, {KEY_C, 'c'}, {KEY_MINUS, '-'}, {KEY_EQUAL, '='}, {KEY_KPPLUS, '+'},
        {KEY_KPMINUS, '-'}, {KEY_UP, K_UP}, {KEY_DOWN, K_DOWN}, {KEY_LEFT, K_LEFT}, {KEY_RIGHT, K_RIGHT},
        {KEY_ENTER, K_ENTER}, {KEY_KPENTER, K_ENTER}, {KEY_ESC, K_ESC}, {KEY_BACKSPACE, K_BKSP},
    };
    size_t i;
    for (i = 0; i < sizeof km / sizeof km[0]; i++)
        if (km[i].code == c) return km[i].key;
    return 0;
}

static double esc_down; /* monotonic time Esc went down, 0 while it is up */

static double mono_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int keys_esc_held(double secs)
{
    return esc_down > 0 && mono_s() - esc_down >= secs;
}

int keys_read(int fd)
{
    struct input_event ev;
    if (fd < 0) return 0;
    while (read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type == EV_KEY && ev.code == KEY_ESC && ev.value != 2) esc_down = ev.value ? mono_s() : 0;
        if (ev.type == EV_KEY && ev.value == 1) { /* press only, no auto-repeat */
            int k = xlate(ev.code);
            if (k) return k;
        }
    }
    return 0;
}
