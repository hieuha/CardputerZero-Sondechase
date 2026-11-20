/* Keyboard input straight from evdev: LVGL's own keypad mapping drops letter keys. */
#define _GNU_SOURCE
#include "ui.h"
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
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
        {KEY_M, 'm'}, {KEY_C, 'c'}, {KEY_B, 'b'}, {KEY_D, 'd'}, {KEY_V, 'v'}, {KEY_K, 'k'}, {KEY_MINUS, '-'}, {KEY_EQUAL, '='}, {KEY_KPPLUS, '+'},
        /* Sym is held like Shift (KEY_CD) and the driver sends its own codes for the symbol layer: Sym + R, printed -,
         * arrives as KEY_KP7 and Sym + T, printed +, as KEY_KP8 (logged on the device, kernel 6.18.39) */
        {KEY_KP7, '-'}, {KEY_KP8, '+'},
        {KEY_KPMINUS, '-'}, {KEY_UP, K_UP}, {KEY_DOWN, K_DOWN}, {KEY_LEFT, K_LEFT}, {KEY_RIGHT, K_RIGHT},
        {KEY_ENTER, K_ENTER}, {KEY_KPENTER, K_ENTER}, {KEY_ESC, K_ESC}, {KEY_BACKSPACE, K_BKSP}, {KEY_TAB, K_TAB},
    };
    size_t i;
    for (i = 0; i < sizeof km / sizeof km[0]; i++)
        if (km[i].code == c) return km[i].key;
    return 0;
}

static double esc_down; /* monotonic time Esc went down, 0 while it is up */
/* Held arrow key, repeated by the app itself: the keypad driver may not auto-repeat, and the delay and rate
 * are kept below the tuner's retune time (~33 ms over I2C) so tuning never falls behind the key. */
#define REPEAT_DELAY_S 0.4
#define REPEAT_EVERY_S 0.08
static int held;          /* K_* of the arrow held down, 0 = none */
static unsigned short held_code;
static double held_next;  /* next repeat */

double keys_esc_held_s(void)
{
    return esc_down > 0 ? sc_now() - esc_down : 0.0;
}

int keys_read(int fd)
{
    struct input_event ev;
    if (fd < 0) return 0;
    while (read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type == EV_KEY && ev.code == KEY_ESC && ev.value != 2) esc_down = ev.value ? sc_now() : 0;
        if (ev.type == EV_KEY && ev.value == 0 && ev.code == held_code) held = 0;
        if (ev.type == EV_KEY && ev.value == 1) { /* press only; the kernel's auto-repeat is ignored */
            int k = xlate(ev.code);
            if (k == K_UP || k == K_DOWN || k == K_LEFT || k == K_RIGHT) {
                held = k;
                held_code = ev.code;
                held_next = sc_now() + REPEAT_DELAY_S;
            }
            if (k) return k;
        }
    }
    if (held && sc_now() >= held_next) {
        held_next = sc_now() + REPEAT_EVERY_S;
        return held | K_REPEAT;
    }
    return 0;
}
