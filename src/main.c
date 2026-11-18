/* sondechase: radiosonde receiver and chase display for the M5Stack CardputerZero. */
#define _GNU_SOURCE
#include "lvgl.h"
#include "ui.h"
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static app_t app;

static void on_sig(int s)
{
    (void)s;
    stop = 1;
}

static uint32_t tick_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

#ifdef SC_SIM
static void sim_key_cb(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    int m = k == LV_KEY_UP ? K_UP : k == LV_KEY_DOWN ? K_DOWN : k == LV_KEY_LEFT ? K_LEFT : k == LV_KEY_RIGHT ? K_RIGHT
          : k == LV_KEY_ENTER ? K_ENTER : k == LV_KEY_ESC ? K_ESC : k == LV_KEY_BACKSPACE ? K_BKSP
          : (k >= 'A' && k <= 'Z') ? (int)k + 32 : (int)k;
    ui_key(m);
}
#endif

int main(int argc, char **argv)
{
    const char *cfg = argc > 1 ? argv[1] : "/etc/sondechase.conf";
    int kfd = -1;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);
    app_init(&app, cfg);

    lv_init();
    lv_tick_set_cb(tick_ms);
#ifdef SC_SIM
    {
        lv_indev_t *kb;
        lv_group_t *g = lv_group_create();
        lv_sdl_window_create(320, 170);
        kb = lv_sdl_keyboard_create();
        lv_indev_set_group(kb, g);
        ui_init(&app);
        lv_group_add_obj(g, lv_screen_active());
        lv_obj_add_event_cb(lv_screen_active(), sim_key_cb, LV_EVENT_KEY, NULL);
    }
#else
    {
        lv_display_t *d = lv_linux_fbdev_create();
        lv_linux_fbdev_set_file(d, app.cfg.fb_dev);
        kfd = keys_open(app.cfg.kbd_dev);
        ui_init(&app);
    }
#endif
    while (!stop && !ui_should_quit()) {
        int k;
        uint32_t wait;
        while ((k = keys_read(kfd)) != 0) ui_key(k);
        if (keys_esc_held(3.0)) break; /* hold Esc 3 s: quit back to the launcher */
        app_poll(&app);
        wait = lv_timer_handler();
        usleep((wait > 30 ? 30 : (wait < 5 ? 5 : wait)) * 1000);
    }
    app_free(&app);
    if (kfd >= 0) close(kfd);
    return 0;
}
