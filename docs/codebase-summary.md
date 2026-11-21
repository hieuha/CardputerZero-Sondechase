# Codebase Summary

Last updated: 2026-11-20 (version 0.7.0). Derived from a repomix compaction of the
working tree and from the line counts below. Regenerate after large changes.

## Contents
- [At a glance](#at-a-glance)
- [Repository layout](#repository-layout)
- [Source modules](#source-modules)
- [Tests and tools](#tests-and-tools)
- [Build, packaging and CI files](#build-packaging-and-ci-files)
- [Assets and screenshots](#assets-and-screenshots)
- [Documentation](#documentation)
- [Generated and ignored content](#generated-and-ignored-content)
- [History](#history)

## At a glance

| Item | Value |
|---|---|
| Language | C11, one application, plus two shell and Python helper groups |
| Application | `sondechase` (CardputerZero, Debian 13 arm64), LVGL 9.5.0 UI |
| Core | `sc_core`, no LVGL, unit-tested on Linux |
| Source size | `src/` 6447 lines (`src/app.c` 630 the largest, `src/sc.h` 574) |
| Tests | `tests/test_core.c` (1690 lines), 4496 checks, all passing (GCC 14.2, Debian trixie) |
| External programs run | `fsk_demod`, `rs41mod` (built from `radiosonde_auto_rx` v1.9.0), `pacat`, `curl`, `gpsd` (socket), or a serial GPS read directly |
| Runtime libraries | `librtlsdr`, or `libhackrf` for a HackRF (loaded with `dlopen`), libc, libm, pthread |
| License | GPL-3.0-or-later (see `LICENSE`) |
| Store manifest | `app-builder.json` (version 0.7.0) |

## Repository layout

```
.
├── src/              application and core (see below)
├── tests/            core tests and fake librtlsdr / libhackrf / decoder
├── tools/            render.c, prep_tiles.py, make_icon.py, validate_app_builder.py
├── cmake/            package layout, decoder build, toolchain, Template licence
├── conf/             sondechase.conf (installed as /etc/sondechase.conf)
├── assets/           fonts (Rajdhani, Orbitron, OFL licences) and icons
├── screenshots/      store and README images (device/, mockup/ subfolders)
├── .github/workflows tests, package, validate
├── docs/             this documentation
├── plans/            plans and reports (not shipped)
├── .claude/          assistant rules and skills (not shipped)
├── CMakeLists.txt    project, options, core and UI targets
├── CMakePresets.json configure, build, package and workflow presets
├── app-builder.json  CardputerZero store manifest
├── PUBLISH.md        store publish record: package, listing, assets, last submission
├── lv_conf.h         LVGL configuration
├── README.md, README.vi.md
└── release-manifest.json  local tooling metadata, ignored by git
```

## Source modules

| File | Lines | Entry points (examples) | Role |
|---|---|---|---|
| `src/sc.h` | 574 | all | Public types (`cfg_t`, `app_t`, `sonde_t`, `wind_t`, `radio_t`, `flight_t`, `tilefetch_t`, ...) and APIs |
| `src/main.c` | 92 | `main` | Startup, signals, main loop, hold-Esc quit |
| `src/app.c` | 630 | `app_init`, `app_poll`, `app_tune`, `app_listen`, `app_scan`, `app_hunt`, `app_on_frame`, `app_free` | Application state machine, receiver ownership, decoder pipeline, sweep, Scan hunt, frame handling, CSV and auto_rx logs |
| `src/base.c` | 426 | `cfg_load`, `cfg_defaults`, `sc_subst`, `fb_pick`, `lr_readline`, `proc_start_feed`, `js_num`, `js_str` | Config, line reader, child process groups, flat-JSON reader |
| `src/sdr.c` | 332 | `sdr_open`, `sdr_tune`, `sdr_read`, `sdr_range`, `iqring_*` | IQ ring, generic receiver calls, RTL-SDR back end (librtlsdr via `dlopen`, reader thread, tuner range table) |
| `src/sdr_hackrf.c` | 318 | `sdr_hackrf_ops` | HackRF back end: libhackrf via `dlopen`, int8 to u8, half-band halving, gain split, stale-transfer drop |
| `src/sdr_priv.h` | 32 | `sdr_ops_t`, `struct sdr` | Back-end interface |
| `src/dsp.c` | 234 | `fft_spectrum`, `ddc_process`, `ddc_process_wide` | FFT with Hann window, decoder front end |
| `src/radio.c` | 207 | `radio_init`, `radio_process`, `radio_process_wfm`, `radio_next_bw` | NFM, AM, USB, LSB, WFM demodulation and channel widths |
| `src/io.c` | 292 | `gps_poll`, `gps_parse`, `sdr_present`, `spec_add_bins`, `spec_peaks` | gpsd client, USB presence, spectrum cells and peaks |
| `src/nmea.c` | 167 | `nmea_poll`, `nmea_parse` | Serial GPS (a GPS cap) read without gpsd: NMEA GGA, RMC, GSA, GSV |
| `src/compass.c` | 399 | `heading_update`, `heading_tilt`, `mag_read`, `acc_read`, `magcal_add`, `magcal_apply` | Heading sources, magnetometer and accelerometer, tilt compensation, compass calibration |
| `src/geo.c` | 223 | `geo_dist`, `geo_bearing`, `isa_rho`, `dewpoint`, `wind_add`, `sonde_parse`, `predict_landing` | Geodesy, ISA, dew point, wind profile, sonde JSON, landing prediction |
| `src/track.c` | 333 | `hist_add`, `flight_update`, `prox_update`, `audio_*`, `led_*`, `power_set`, `tile_frac` | Flight history, burst detection, proximity beeper, PCM audio stream, frame LED, EXT 5 V switch, slippy-map maths |
| `src/tiles.c` | 326 | `tf_find`, `tf_request`, `tf_poll`, `tf_sweep`, `tile_recolor`, `tf_host_blocked`, `mkdirs` | Tile lookup, `curl` downloads, cache cap, failure reasons, recolouring |
| `src/demo.c` | 139 | `demo_poll`, `demo_advance` | Simulated flight |
| `src/keys.c` | 97 | `keys_open`, `keys_read`, `keys_esc_held_s` | evdev keyboard, Sym symbol keys, held-arrow repeat, Esc timing |
| `src/ui.c` | 258 | `ui_init`, `ui_key`, `draw_cb`, `tick_cb` | UI state, overlays (frequency entry, burst alert, no SDR), LVGL glue, key dispatch |
| `src/ui_draw.c` | 254 | `ln`, `fill`, `txt`, `box`, `kv`, `top_bar`, `nav_bar` | Drawing primitives, top and nav bars, battery, shared helpers |
| `src/ui_track.c` | 227 | `p_track`, `compass`, `phase_name` | Track screen, compass, plain-language directions |
| `src/ui_map.c` | 290 | `p_map`, `map_pan`, `map_zoom_step`, `map_follow_next`, `map_style_toggle` | Map screen, decoded tile cache, pan, zoom, follow, style |
| `src/ui_graph.c` | 168 | `p_skewt`, `p_flight` | Graph screens: Skew-T and flight profile |
| `src/ui_radio.c` | 159 | `p_scan` | Radio screen: band overview, tuned spectrum, listening, decoding |
| `src/ui_info.c` | 177 | `p_info`, `p_info_pos`, `p_cal` | Info screens and compass calibration |
| `src/ui_priv.h` | 77 | all of the above | Colours, shared UI state and prototypes for the UI files |
| `src/ui.h` | 16 | `ui_init`, `ui_key`, `keys_*` | UI interface for `main.c`, `keys.c`, `tools/render.c` and the tests |

One UI file per screen: Track (`4`, `ui_track.c`), Map (`5`, `ui_map.c`), Graph (`6`, Skew-T and flight pages,
`ui_graph.c`), Radio (`7`, tuned view, Scan, `ui_radio.c`), Info (`8`, `ui_info.c`). The frequency entry (`F`), burst
alert overlay and the "No SDR device found" screen stay in `ui.c` with the draw callback and `ui_key`.

## Tests and tools

| File | Lines | Role |
|---|---|---|
| `tests/test_core.c` | 1690 | Core checks: geodesy, ISA, prediction, sonde and gpsd parsers, heading, compass calibration and tilt compensation on simulated devices, config, framebuffer choice, spectrum, IQ ring, DSP, radio, sweep and hunt, tiles, flight, beeper, LED, child lifecycle |
| `tests/fake_rtlsdr.c` | 60 | Stand-in `librtlsdr` shared library: 403.000 MHz carrier wherever it is tuned |
| `tests/fake_hackrf.c` | 131 | Stand-in `libhackrf`: int8 transfers in real time, each with the frequency it began at; getters for rate, filter and gains |
| `tests/fake_decoder.sh` | 11 | Stand-in decoder: prints rs41mod-style JSON lines and a non-JSON line |
| `tools/render.c` | 226 | `sc_render`: headless UI on demo data, writes 320×170 PPM per screen |
| `tools/validate_app_builder.py` | 77 | Store manifest check (stdlib only), run in CI |
| `tools/prep_tiles.py` | 114 | Builds a preloaded tile pack from MBTiles or `z/x/y.png` (Pillow) |
| `tools/make_icon.py` | 78 | Draws the app icon (Pillow) |

## Build, packaging and CI files

| File | Lines | Role |
|---|---|---|
| `CMakeLists.txt` | 92 | Project `sondechase` 0.7.0, options `SC_BUILD_UI`, `SC_SIM`, `SC_RENDER`, targets |
| `CMakePresets.json` | 90 | Presets `cp0-native`, `cp0-cross`, `linux-x86-64`, `render`; packages `cp0-native-deb`, `cp0-cross-deb`; workflows |
| `cmake/cm0-package.cmake` | 94 | Install layout, `.deb` settings (CPack), desktop entry, conffile |
| `cmake/radiosonde-decoders.cmake` | 45 | Downloads `radiosonde_auto_rx` v1.9.0 (SHA-256 pinned) and builds `rs41mod` and `fsk_demod` |
| `cmake/toolchains/cp0-aarch64-linux-gnu.cmake` | - | Cross toolchain and BSP sysroot download |
| `cmake/templates/app.desktop.in`, `conffiles.in` | - | Launcher entry and conffile templates |
| `conf/sondechase.conf` | 70 | Shipped configuration |
| `lv_conf.h` | 43 | LVGL build configuration |
| `.github/workflows/tests.yml` | 15 | Core build and `ctest` on Ubuntu |
| `.github/workflows/package.yml` | 18 | Native arm64 package on Debian 13, `ctest`, artifact |
| `.github/workflows/validate.yml` | 11 | Store manifest check |

## Assets and screenshots

| Path | Contents |
|---|---|
| `assets/fonts/` | `Rajdhani-SemiBold.ttf`, `Orbitron-Bold.ttf`, SIL OFL licence texts |
| `assets/images/` | `sondechase.png` (100×100, APPLaunch), `sondechase_256.png` (store icon) |
| `screenshots/*.png` | 19 images at 320×170 from `sc_render` plus `all_screens_2x.png` (1280×1360); six are listed in the store manifest (`app-builder.json`) |
| `screenshots/replay-y0310371.gif` | Replay of sonde Y0310371 (Track and Skew-T), shown in the READMEs; made once, not regenerated by `sc_render` |
| `screenshots/mockup/` | 13 scaled mockups (2×); 12 are shown in the READMEs, `radio-listening.png` is not referenced |
| `screenshots/device/` | 4 framebuffer captures from the CardputerZero |

## Documentation

| File | Purpose |
|---|---|
| `README.md`, `README.vi.md` | User-facing overview, install, keys, screens, quick config (English and Vietnamese) |
| `docs/project-overview-pdr.md` | Product overview and requirements |
| `docs/system-architecture.md` | Modules, processes, data flows, peripherals, algorithms |
| `docs/code-standards.md` | Conventions for C, naming, tests, commits |
| `docs/deployment-guide.md` | Builds, tests, install, configuration reference, release steps |
| `docs/project-roadmap.md` | Released versions, known gaps, declined ideas |
| `docs/codebase-summary.md` | This file |

## Generated and ignored content

Ignored by `.gitignore`: `build/` (the default and `cp0-*` build tree), `dist/` (packages), `.cache/` (FetchContent
and BSP sysroot), `repomix-output.xml`, `release-manifest.json`, `.repomixignore`, `__pycache__/`. The local `.repomixignore` excludes
`docs/`, `plans/`, `assets/`, `dist/`, `tests/` and tool folders such as `.github/` and `.claude/` from the compaction.



## History

Tags and GitHub releases: `v0.4.0`, `v0.4.1`, `v0.5.0`, `v0.5.1`, `v0.6.0`, `v0.7.0`.
Version bumps are `chore: version x.y.z` commits (see
[project roadmap](./project-roadmap.md#released-versions)).
