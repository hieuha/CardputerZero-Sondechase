/* Minimal LVGL 9.x configuration: every option not set here keeps LVGL's default. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_DEF_REFR_PERIOD 33

/* libc allocator: decoded map tiles do not fit LVGL's built-in 64 kB pool */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* files: map tiles (PNG) and TTF fonts, addressed as "A:/absolute/path" */
#define LV_USE_FS_POSIX 1
#define LV_FS_POSIX_LETTER 'A'
#define LV_FS_POSIX_PATH ""
#define LV_USE_LODEPNG 1
#define LV_CACHE_DEF_SIZE (4 * 1024 * 1024) /* ~12 decoded 256x256 tiles */
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 32
#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 1

/* fallback fonts when the TTF files are not installed */
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_12

#if defined(SC_HEADLESS)
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
/* screenshot tool: renders into a memory buffer, no display driver */
#elif defined(SC_SIM)
#define LV_USE_SDL 1
#else
#define LV_USE_LINUX_FBDEV 1
#endif

#endif
