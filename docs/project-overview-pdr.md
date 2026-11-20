# Project Overview and PDR

Last updated: 2026-11-20 (version 0.6.0).

## Contents
- [Overview](#overview)
- [Users and use](#users-and-use)
- [Scope](#scope)
- [Requirements](#requirements)
- [Non-functional requirements](#non-functional)
- [Acceptance criteria](#acceptance-criteria)
- [Constraints and dependencies](#constraints-and-dependencies)
- [Decisions](#decisions)
- [Status](#status)

## Overview

SondeChase turns the M5Stack CardputerZero (Raspberry Pi CM0, Debian 13 arm64, 320×170 LCD, 46-key keyboard) into a
handheld radiosonde receiver and chase display for the Vaisala RS41. It receives the 400 MHz telemetry with an
RTL-SDR, decodes it with the bundled `fsk_demod` and `rs41mod`, and shows the balloon's position relative to the
device. The device position comes from an optional USB GPS through gpsd, and heading from the GPS course or a
magnetometer.

Product form: one `.deb` package, installed from APPLaunch, distributed through the CardputerZero app store and as
GitHub releases. Source is GPL-3.0-or-later.

## Users and use

| User | Need |
|---|---|
| Radiosonde enthusiast with a CardputerZero and an RTL-SDR | Receive a launched RS41 without a laptop, find it, and see where it lands |
| Chaser in the field | Direction, distance, elevation, landing estimate, a beeper that gets faster as the balloon gets closer, a burst alert |
| Developer or maintainer | A small C codebase, reproducible `.deb` build, tests that run without hardware |

Typical session: plug in the RTL-SDR, open SondeChase, press `S` to scan 400–406 MHz. The app tries the strongest
peak with the decoder and stays on the first RS41 it finds. The Track screen then shows the sonde, and the Map and
Graph screens show its track and profile.

## Scope

In scope (current):
- RS41 reception and decoding with the in-process RTL-SDR receiver (librtlsdr, loaded at run time).
- Spectrum scan of the band, tuning, listening with NFM, AM, USB, LSB and broadcast FM audio.
- Position display, compass, distance and elevation, predicted landing, burst alert, proximity beeper, frame tick and
  LED blink.
- Map from OpenStreetMap tiles (on-demand download, cache, optional preloaded pack), Skew-T and flight graphs.
- Local CSV log and a radiosonde_auto_rx-format log per sonde.
- Demo mode for testing without hardware.

Out of scope (decided or not planned):
- Sonde types other than RS41. Another type needs its own decoder and `fsk_demod` parameters.
- Running `radiosonde_auto_rx` itself, or a network service. The app runs alone.
- Downloading weather model data (a GFS wind download was declined).
- SNR and frequency-error measurement (dropped; the logs write `-1` for these fields).
- Offline bulk map download (the tile server's policy forbids it; the app does not prefetch).
- Systemd service or background operation. The app runs when opened from APPLaunch.
- Storage of flight history across restarts.

## Requirements

### Functional

| ID | Requirement | Where |
|---|---|---|
| FR-01 | Open the RTL-SDR (VID:PID `0bda:2838` or `0bda:2832`) and report a missing dongle on a blocking screen, rechecked every 2 s | `app.c`, `io.c`, `ui.c` |
| FR-02 | Sweep 400–406 MHz continuously with peak hold; mark peaks at least 10 dB above the floor | `app.c`, `io.c`, `ui.c` |
| FR-03 | Scan (`S`): after two passes, try the strongest untried peak with the decoder for 10 s; stop on an RS41 frame; skip a failed peak for 2 min | `app.c` (`app_hunt`) |
| FR-04 | Tune with arrow keys in 10 kHz (Left/Right) and 100 kHz (Up/Down) steps; hold to keep tuning | `keys.c`, `ui.c` |
| FR-05 | Listen to the tuned frequency with NFM (default 16 kHz), AM, USB, LSB and WFM; select the channel width with `B` and the mode with `D` | `radio.c`, `ui.c` |
| FR-06 | `Enter` decodes the tuned frequency; `F` enters a frequency in kHz within the tuner's range | `ui.c`, `app.c` |
| FR-07 | Decode RS41 frames with `fsk_demod` and `rs41mod` in a child pipeline and restart it when it exits | `app.c` |
| FR-08 | Show distance, bearing and elevation to the sonde, the relative arrow, and the predicted landing point, when a GPS fix exists | `app.c`, `geo.c`, `ui.c` |
| FR-09 | Show serial, frame and battery of a sonde without a GPS fix; keep its position out of the track | `geo.c`, `ui.c` |
| FR-10 | Show the satellite count (`GPS 0/3` while plugged in without a fix, `3D 9/12` with a fix) | `ui.c` |
| FR-11 | Compass ring turns green while the sonde is within 15° of straight ahead | `ui.c` |
| FR-12 | Map: tiles from the preloaded pack, the cache, then download of visible tiles only; simple and detailed styles; pan, zoom, follow modes | `tiles.c`, `ui.c` |
| FR-13 | Skew-T log-P and altitude and vertical-speed graphs of the flight | `ui.c` |
| FR-14 | Burst alert when vertical speed is below −3 m/s on three frames above 1000 m after an ascent; Enter opens the Map | `track.c`, `ui.c` |
| FR-15 | Proximity beeper inside 10 km, faster as the distance falls; silent without a position; mutable with `M` | `track.c` |
| FR-16 | Each valid frame plays a tick and blinks the ACT LED on Track and Map | `app.c`, `track.c` |
| FR-17 | Write `sondechase.csv` and one radiosonde_auto_rx-format log per sonde, timed by the sonde's GPS time | `app.c` |
| FR-18 | Read configuration from `/etc/sondechase.conf`; every key optional | `base.c` |
| FR-19 | Simulated flight with `demo = 1`, through the same frame path | `demo.c` |
| FR-20 | Quit by holding `Esc` for 3 s (no single-key quit) | `keys.c`, `ui.c`, `main.c` |

### Non-functional

| ID | Requirement | Current evidence |
|---|---|---|
| NFR-01 | Runs on the CardputerZero (4 cores, arm64) within its CPU budget while decoding | 15.6 % of one core for the whole app while decoding (device measurement, [deployment guide](./deployment-guide.md#device-checks)) |
| NFR-02 | No dropped samples between the dongle and the decoder under normal load | 0 dropped in the IQ ring and the decoder pipe (device measurement) |
| NFR-03 | Core logic is testable without hardware | `test_core`: 4363 checks, 0 failed |
| NFR-04 | Package installs with no build tools, only `libc6`, `curl`, `librtlsdr0 (>= 2.0.2)` | `cmake/cm0-package.cmake` |
| NFR-05 | Works offline for everything except new map tiles | Cache and preloaded pack are used first |
| NFR-06 | Does not stop on a missing optional peripheral (GPS, LED, audio) | Each has a disabled or retry state; see [system architecture](./system-architecture.md#failure-handling) |
| NFR-07 | Stays within the screen: 320×170, text fits its box | Screenshots from `sc_render`, see [deployment guide](./deployment-guide.md#screenshots) |
| NFR-08 | Respects the tile server's policy: download only what is on screen, identify the app in the User-Agent | `tiles.c`, `tile_user_agent` |
| NFR-09 | Memory and storage are bounded: history 2048 points, tile cache 1000 MB by default, IQ ring 4 MiB | `sc.h`, `base.c` |

## Acceptance criteria

A release is accepted when all of these hold:

1. `ctest` passes on a Linux core build (`-DSC_BUILD_UI=OFF`). Current: 4363 checks, 0 failed.
2. The `package` workflow builds `sondechase_<version>-rev1_arm64.deb` on Debian 13 arm64 and its `ctest` passes.
3. `tools/validate_app_builder.py` passes; `CMakeLists.txt` and `app-builder.json` have the same version.
4. Both READMEs state the same version, keys, screens and facts.
5. On a CardputerZero with an RTL-SDR and a live sonde, the app scans, decodes, and shows the sonde's frames and
   tick (manual check; see [device checks](./deployment-guide.md#device-checks) for the last recorded run).

## Constraints and dependencies

| Item | Constraint |
|---|---|
| Hardware | CardputerZero: 320×170 LCD, 46 keys, ES8389 audio codec under PipeWire, no RTC. RTL-SDR tested: Blog V4 (R828D) |
| Software | Debian 13 arm64; LVGL 9.5.0 (fetched at build time); `radiosonde_auto_rx` v1.9.0 decoders (fetched and SHA-256 checked) |
| Runtime | `librtlsdr0` (loaded with `dlopen`), `curl` for tiles, optional `gpsd` and `pulseaudio-utils` (`pacat`) |
| Licence | GPL-3.0-or-later; bundled decoders are GPL-3.0 and LGPL-2.1; LVGL MIT; fonts SIL OFL 1.1; map data ODbL attribution |
| Store | CardputerZero store manifest (`app-builder.json`): seven permission flags, 320×170 screenshots, share code `SNDE` |
| Time | No RTC: log timestamps come from the sonde's GPS time when it has one |

## Decisions

| Decision | Source |
|---|---|
| In-process librtlsdr receiver; `rtl_fm`, `rtl_power` not used | Commit history from `ef63dbc` (librtlsdr only, one open dongle) |
| Decoder chain `fsk_demod` and `rs41mod` shipped in the package | `cmake/radiosonde-decoders.cmake` |
| Scan takes the strongest peak and tries it; the operator can pick by hand | `app_hunt`, README |
| Map uses only on-demand tile download, no prefetch | Tile server usage policy, `tiles.c` |
| Simple map style is the default; the detailed style is one key away | `map_simple = 1`, `V` |
| Frame cue on Track and Map only | `frame_cue` set in `ui_key` |
| SNR and frequency-error measurement dropped | Maintainer decision (logs write `-1`) |
| GFS wind download declined | Maintainer decision (not planned) |

## Status

- Version 0.6.0 (tag `v0.6.0`, GitHub release published, in the CardputerZero store).
- Tested on hardware: scanning and decoding with a live RS41 (Y0532351), display, keyboard, PipeWire audio, magnetometer
  node, OSM tile download. Not yet tested in the field: GPS (no module fitted when this was written), the beeper, burst
  detection and landing on a real flight. Full list in the [roadmap](./project-roadmap.md#known-gaps).
- Version history and per-version changes are in the [roadmap](./project-roadmap.md#released-versions).
