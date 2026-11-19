# Codebase Summary

Last updated: 2026-11-19 (version 0.5.0, HEAD e83d0d7, branch `main`). Derived from a repomix compaction of the
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
| Source size | `src/` 4766 lines (`src/sc.h` 496, `src/ui.c` 1271 the largest) |
| Tests | `tests/test_core.c` (1052 lines), 4315 checks, all passing (GCC 14.2, Debian trixie) |
| External programs run | `fsk_demod`, `rs41mod` (built from `radiosonde_auto_rx` v1.9.0), `pacat`, `curl`, `gpsd` (socket) |
| Runtime libraries | `librtlsdr` (loaded with `dlopen`), libc, libm, pthread |
| License | GPL-3.0-or-later (see `LICENSE`) |
| Store manifest | `app-builder.json` (version 0.5.0) |

## Repository layout

```
.
├── src/              application and core (see below)
├── tests/            core tests and fake librtlsdr / decoder
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
├── lv_conf.h         LVGL configuration
├── README.md, README.vi.md
└── release-manifest.json  tooling metadata; not used by the build
```

## Source modules

| File | Lines | Entry points (examples) | Role |
|---|---|---|---|
| `src/sc.h` | 496 | all | Public types (`cfg_t`, `app_t`, `sonde_t`, `wind_t`, `radio_t`, `flight_t`, `tilefetch_t`, ...) and APIs |
| `src/main.c` | 90 | `main` | Startup, signals, main loop, hold-Esc quit |
| `src/app.c` | 568 | `app_init`, `app_poll`, `app_tune`, `app_listen`, `app_scan`, `app_hunt`, `app_on_frame`, `app_free` | Application state machine, receiver ownership, decoder pipeline, sweep, Scan hunt, frame handling, CSV and auto_rx logs |
| `src/base.c` | 340 | `cfg_load`, `cfg_defaults`, `sc_subst`, `lr_readline`, `proc_start_feed`, `js_num`, `js_str` | Config, line reader, child process groups, flat-JSON reader |
| `src/sdr.c` | 273 | `sdr_open`, `sdr_tune`, `sdr_read`, `sdr_range`, `iqring_*` | librtlsdr via `dlopen`, reader thread, IQ ring, tuner range table |
| `src/dsp.c` | 215 | `fft_spectrum`, `ddc_process`, `ddc_process_wide` | FFT with Hann window, decoder front end |
| `src/radio.c` | 174 | `radio_init`, `radio_process`, `radio_process_wfm`, `radio_next_bw` | NFM, AM, USB, LSB, WFM demodulation and channel widths |
| `src/io.c` | 291 | `gps_poll`, `gps_parse`, `heading_update`, `sdr_present`, `spec_add_bins`, `spec_peaks` | gpsd client, heading sources, USB presence, spectrum cells and peaks |
| `src/geo.c` | 202 | `geo_dist`, `geo_bearing`, `isa_rho`, `dewpoint`, `wind_add`, `sonde_parse`, `predict_landing` | Geodesy, ISA, dew point, wind profile, sonde JSON, landing prediction |
| `src/track.c` | 310 | `hist_add`, `flight_update`, `prox_update`, `audio_*`, `led_*`, `tile_frac` | Flight history, burst detection, proximity beeper, PCM audio stream, frame LED, slippy-map maths |
| `src/tiles.c` | 291 | `tf_find`, `tf_request`, `tf_poll`, `tf_sweep`, `tile_recolor`, `tf_host_blocked`, `mkdirs` | Tile lookup, `curl` downloads, cache cap, failure reasons, recolouring |
| `src/demo.c` | 139 | `demo_poll`, `demo_advance` | Simulated flight |
| `src/keys.c` | 89 | `keys_open`, `keys_read`, `keys_esc_held_s` | evdev keyboard, held-arrow repeat, Esc timing |
| `src/ui.c` | 1271 | `ui_init`, `ui_key`, `ui_should_quit`, `draw_cb`, `tick_cb` | All LVGL screens, chrome, key dispatch |
| `src/ui.h` | 17 | `ui_init`, `ui_key`, `ui_should_quit`, `keys_*` | UI interface for `main.c` and the tests |

Screens inside `ui.c`: Track (`4`), Map (`5`), Graph (`6`, Skew-T and flight pages), Radio (`7`, tuned view, Scan),
Info (`8`), frequency entry (`F`), burst alert overlay and the "No SDR device found" screen.

## Tests and tools

| File | Lines | Role |
|---|---|---|
| `tests/test_core.c` | 1052 | Core checks: geodesy, ISA, prediction, sonde and gpsd parsers, heading, config, spectrum, IQ ring, DSP, radio, sweep and hunt, tiles, flight, beeper, LED, child lifecycle |
| `tests/fake_rtlsdr.c` | 54 | Stand-in `librtlsdr` shared library: 403.000 MHz carrier wherever it is tuned |
| `tests/fake_decoder.sh` | 11 | Stand-in decoder: prints rs41mod-style JSON lines and a non-JSON line |
| `tools/render.c` | 196 | `sc_render`: headless UI on demo data, writes 320×170 PPM per screen |
| `tools/validate_app_builder.py` | 77 | Store manifest check (stdlib only), run in CI |
| `tools/prep_tiles.py` | 114 | Builds a preloaded tile pack from MBTiles or `z/x/y.png` (Pillow) |
| `tools/make_icon.py` | 78 | Draws the app icon (Pillow) |

## Build, packaging and CI files

| File | Lines | Role |
|---|---|---|
| `CMakeLists.txt` | 86 | Project `sondechase` 0.5.0, options `SC_BUILD_UI`, `SC_SIM`, `SC_RENDER`, targets |
| `CMakePresets.json` | 90 | Presets `cp0-native`, `cp0-cross`, `linux-x86-64`, `render`; packages `cp0-native-deb`, `cp0-cross-deb`; workflows |
| `cmake/cm0-package.cmake` | 92 | Install layout, `.deb` settings (CPack), desktop entry, conffile |
| `cmake/radiosonde-decoders.cmake` | 45 | Downloads `radiosonde_auto_rx` v1.9.0 (SHA-256 pinned) and builds `rs41mod` and `fsk_demod` |
| `cmake/toolchains/cp0-aarch64-linux-gnu.cmake` | - | Cross toolchain and BSP sysroot download |
| `cmake/templates/app.desktop.in`, `conffiles.in` | - | Launcher entry and conffile templates |
| `conf/sondechase.conf` | 64 | Shipped configuration |
| `lv_conf.h` | 43 | LVGL build configuration |
| `.github/workflows/tests.yml` | 15 | Core build and `ctest` on Ubuntu |
| `.github/workflows/package.yml` | 18 | Native arm64 package on Debian 13, `ctest`, artifact |
| `.github/workflows/validate.yml` | 11 | Store manifest check |

## Assets and screenshots

| Path | Contents |
|---|---|
| `assets/fonts/` | `Rajdhani-SemiBold.ttf`, `Orbitron-Bold.ttf`, SIL OFL licence texts |
| `assets/images/` | `sondechase.png` (100×100, APPLaunch), `sondechase_256.png` (store icon) |
| `screenshots/*.png` | 20 images at 320×170 from `sc_render`; six are listed in the store manifest (`app-builder.json`) |
| `screenshots/mockup/` | 10 scaled mockups for the READMEs (2×) |
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
and BSP sysroot), `repomix-output.xml`, `__pycache__/`. `.repomixignore` excludes `docs/`, `plans/`, `assets/`,
`dist/` and `tests/` from the compaction.


Working copy at the time of writing: no uncommitted changes to tracked files; `dist/sondechase_0.5.0-rev1_arm64.deb`
is present locally and ignored.

## History

Tags: `v0.5.0` is the only tag in the working copy. GitHub releases exist for `v0.4.0`, `v0.4.1` and `v0.5.0`.
The repository has 46 commits. Version bumps are `chore: version x.y.z` commits (see
[project roadmap](./project-roadmap.md#released-versions)).
