# Deployment Guide

Last updated: 2026-11-20 (version 0.6.0).

Covers building the CardputerZero `.deb`, running the tests, installing and configuring the app on the device, and
the release steps. Commands are taken from `CMakePresets.json`, `.github/workflows/` and the CMake files in `cmake/`.

## Contents
- [Build presets](#build-presets)
- [Build on the device](#build-on-the-device)
- [Cross build on a PC](#cross-build-on-a-pc)
- [Tests](#tests)
- [Install and run](#install-and-run)
- [Configuration](#configuration)
- [Map tiles](#map-tiles)
- [Demo mode](#demo-mode)
- [Screenshots](#screenshots)
- [Release procedure](#release-procedure)
- [CI workflows](#ci-workflows)
- [Device checks](#device-checks)
- [Troubleshooting](#troubleshooting)

## Build presets

| Preset (configure) | Purpose | Build preset | Package preset |
|---|---|---|---|
| `cp0-native` | Build on the CardputerZero (Debian 13 arm64) | `cp0-native-rel` | `cp0-native-deb` |
| `cp0-cross` | Cross build for `aarch64-linux-gnu` with the BSP sysroot | `cp0-cross-rel`, `cp0-cross-dbg` | `cp0-cross-deb` |
| `linux-x86-64` | Desktop SDL2 simulator (`SC_SIM=ON`; untested) | `linux-x86-64-rel` | none |
| `render` | Headless screenshot tool (`SC_RENDER=ON`) | `render-rel` | none |

Workflow presets: `cp0-native-package` (configure, build, package on the device) and `cp0-cross-package`.
Package output goes to `dist/`, named `sondechase_<version>-rev1_arm64.deb` (for example `sondechase_0.6.0-rev1_arm64.deb`).

Options in `CMakeLists.txt`:

| Option | Default | Effect |
|---|---|---|
| `SC_BUILD_UI` | `ON` | Build the LVGL application (fetches LVGL v9.5.0). `OFF` builds only the core and tests |
| `SC_SIM` | `OFF` | SDL2 window instead of the framebuffer and evdev |
| `SC_RENDER` | `OFF` | Build `sc_render` (`tools/render.c`) with `SC_HEADLESS=1` |

## Build on the device

Requires Debian 13 arm64 on the CardputerZero (or any aarch64 Debian 13 host):

```sh
sudo apt install cmake ninja-build build-essential git
cmake --workflow --preset cp0-native-package
sudo apt install --no-install-recommends ./dist/sondechase_0.6.0-rev1_arm64.deb
```

The first build fetches LVGL v9.5.0 (git) and the `radiosonde_auto_rx` v1.9.0 release tarball (SHA-256 pinned in
`cmake/radiosonde-decoders.cmake`), then compiles `fsk_demod` and `rs41mod`. Build trees go to `build/cp0-native`.

## Cross build on a PC

Requires a Debian or Ubuntu PC with `crossbuild-essential-arm64`, `ninja-build`, `cmake` 3.25 or newer, `git`
and `curl`:

```sh
cmake --workflow --preset cp0-cross-package
```

The first run downloads the CardputerZero BSP sysroot (`sdk_bsp.tar.gz`, version `CM0_SDK_VERSION`, default
`v0.0.4`) into `.cache/sdk_bsp-src`. Set `-DCM0_ALLOW_FETCH_DEPS=OFF` and `-DCM0_SDK_ROOT=<path>` to use an existing
sysroot. The cross build was not re-run for this document; the native workflow is the one CI runs.

## Tests

The core test suite (`tests/test_core.c`) needs no LVGL and no device. It prints `N checks, M failed` and exits
non-zero on any failure. CI runs it with the fake librtlsdr (`tests/fake_rtlsdr.c`) and the fake decoder
(`tests/fake_decoder.sh`) wired in by `ctest`.

```sh
cmake -S . -B build -G Ninja -DSC_BUILD_UI=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Notes:
- `keys.c` and the tests include `<linux/input.h>`. The build fails on macOS; use a Debian or Ubuntu host or a
  Linux container.
- Verified on 2026-11-20 in `debian:trixie` (GCC 14.2.0): `test_core` reports **4459 checks, 0 failed**.
- The same build prints no warnings (`-Wall -Wextra`), nor does the UI (`sc_render`, `sondechase`).
- Also on 2026-11-20: the whole suite under AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer
  (`-fsanitize=address,undefined`) and under ThreadSanitizer (`-fsanitize=thread`) reports nothing; 300 000
  mutated decoder, gpsd and config lines through `sonde_parse`, `gps_parse` and `cfg_load` under the same
  sanitizers do not crash; `gcc -fanalyzer` and `cppcheck` leave no open finding.
- What the tests cover is listed in [system architecture](./system-architecture.md) by module. Test names are
  in `tests/test_core.c`.

## Install and run

```sh
sudo apt install --no-install-recommends ./sondechase_0.6.0-rev1_arm64.deb
```

Installed files:

| Path | Contents |
|---|---|
| `/usr/bin/sondechase` | Application (LVGL, PNG and TTF decoders statically linked). `sondechase --version` prints the version |
| `/etc/sondechase.conf` | Configuration (conffile, kept on upgrade) |
| `/usr/share/sondechase/fonts/` | Rajdhani SemiBold, Orbitron Bold |
| `/usr/share/sondechase/tiles/` | Empty directory for a preloaded map pack |
| `/usr/share/APPLaunch/applications/sondechase.desktop` | Launcher entry |
| `/usr/share/APPLaunch/share/images/sondechase.png` | 100×100 icon |
| `/usr/lib/sondechase/fsk_demod`, `/usr/lib/sondechase/rs41mod` | Decoders from `radiosonde_auto_rx` v1.9.0 |
| `/usr/share/doc/sondechase/` | README, font licences, `LICENSE-radiosonde_auto_rx` |

Dependencies: `Depends: libc6, curl, librtlsdr0 (>= 2.0.2), libhackrf0`. librtlsdr and libhackrf are loaded at run
time, not linked. `Recommends: gpsd, pulseaudio-utils, coreutils`. The package installs no systemd unit: the app is started from
APPLaunch and runs as the launcher's user, which needs access to the LCD framebuffer (`/dev/fb_lcd`, `fb0` on the current image), `/dev/input/event*`, the RTL-SDR or HackRF and
PipeWire (groups `video`, `input`, `plugdev`, `audio`).

Run from a terminal with a custom config:

```sh
sondechase /path/to/sondechase.conf      # default: /etc/sondechase.conf
```

## Configuration

The app reads `/etc/sondechase.conf` (or the path given as its first argument). Every key is optional; built-in
defaults apply to anything missing.

Syntax rules, from `src/base.c`:
- One `key = value` per line.
- `#` starts a comment at the start of a line or after a space (`tile_dir = /path   # note` reads `/path`).
  A `#` inside a value with no space before it is kept (`https://host/{z}#x`).
- Unknown keys print `sondechase: unknown config key '<key>'` on stderr and are ignored.

### Keys

| Key | Default | Meaning |
|---|---|---|
| `freq_hz` | unset | Decode this frequency at start-up. Unset: open on the Radio screen, listening mid-band |
| `sdr` | `auto` | Receiver: `auto` (whichever is on USB, the RTL-SDR first when both are), `rtlsdr` or `hackrf` |
| `ppm` | `0` | Frequency correction for the receiver, ppm |
| `gain` | unset (tuner AGC) | RTL-SDR tuner gain in dB, for example `38.6` |
| `hackrf_gain` | unset (LNA 32 + VGA 24 dB) | HackRF gain in dB: the total, filled into the LNA (0–40 dB, 8 dB steps), then the VGA (0–62 dB, 2 dB steps), then the 14 dB amplifier. It has no AGC |
| `rtl_dev` | `0` | Receiver index among those of its kind |
| `bias_tee` | `0` | `1` powers an LNA through the RTL-SDR v4 bias-T, or the HackRF's antenna port power |
| `rtlsdr_lib` | `librtlsdr.so.0` | librtlsdr file to load with `dlopen` |
| `hackrf_lib` | `libhackrf.so.0` | libhackrf file to load with `dlopen` |
| `sdr_check` | `1` | `1` requires a receiver on USB: an RTL2832U (VID:PID `0bda:2838` or `0bda:2832`) or a HackRF One or rad1o (`1d50:6089`, `1d50:cc15`) |
| `scan_lo_hz`, `scan_hi_hz` | `400000000`, `406000000` | Scan band |
| `scan_step_hz` | `5000` | Step of the scan spectrum cells, at least 1000; raised so the band fits 2048 cells. A reversed band falls back to the defaults |
| `decoder_feed_cmd` | `fsk_demod … \| rs41mod …` | Decoder pipeline; `{f_hz}` is replaced with the tuned frequency |
| `gpsd_host`, `gpsd_port` | `127.0.0.1`, `2947` | gpsd address. `gpsd_port = 0` disables GPS |
| `cog_min_speed` | `1.5` | m/s above which the GPS course is used as heading |
| `mag_enable` | `1` | Use the IIO magnetometer when the GPS course is not valid |
| `mag_off_x`, `mag_off_y`, `mag_rot_deg`, `mag_flip_y` | `0`, `0`, `0`, `0` | Magnetometer calibration (hard-iron offsets, rotation, axis flip). Normally set in the app (Info, `K`), which writes them to `<data_dir>/compass.conf`; that file is read after the main config and wins |
| `mag_off_z`, `mag_tilt`, `accel_axes` | `0`, `0`, `+x+y+z` | Tilt compensation: z hard-iron offset, on/off, and how the accelerometer's axes map to the magnetometer's (`-x+y-z` on the CardputerZero, which also gets `mag_flip_y = 1`). Set by the calibration's lean step; a `compass.conf` without them keeps the flat-only compass |
| `declination` | `0` | Magnetic declination, degrees east positive |
| `ground_alt` | unset | Landing altitude AMSL. Unset: this device's GPS altitude |
| `burst_alt` | `30000` | Assumed burst altitude while the sonde ascends, m |
| `descent_v0` | `6.0` | Assumed sea-level descent rate before the burst, m/s |
| `stale_s` | `5` | Telemetry older than this is shown as stale |
| `prox_radius_m`, `prox_hyst_m` | `10000`, `500` | Proximity beeper on inside the radius, off beyond radius + hysteresis |
| `audio_enable` | `1` | Beeper, frame tick and radio audio |
| `audio_cmd` | `pacat --raw … --client-name=SondeChase` | Command that reads 16 kHz S16_LE mono PCM on stdin |
| `led_path` | `/sys/class/leds/ACT/brightness` | LED blinked on each frame on Track and Map. Empty: none |
| `tile_online` | `1` | `1` downloads the tiles the Map shows |
| `tile_url` | `https://tile.openstreetmap.org/{z}/{x}/{y}.png` | Tile template |
| `tile_user_agent` | `SondeChase/<version> (CardputerZero app; github.com/hieuha)` | User-Agent sent with each tile request |
| `tile_fetch_cmd` | `curl -fsS --max-time 20 -A '{ua}' -o '{out}' '{url}'` | Download command; must write `{out}` and exit 0 |
| `tile_cache_dir` | `<data_dir>/tiles` | Download cache (offline map) |
| `tile_cache_mb` | `1000` | Cache size limit, MB (at least 1); oldest downloaded tiles are removed, other files are left alone |
| `tile_dir` | `/usr/share/sondechase/tiles` | Preloaded tile pack (`<z>/<x>/<y>.png`), used before the cache |
| `zoom_min`, `zoom_max` | `6`, `18` | Zoom range of the Map, within 0–19 |
| `map_lat`, `map_lon` | `21.0285`, `105.8542` | Map centre while no position is known |
| `map_simple` | `1` | Start-up map style: `1` simple (water and roads), `0` detailed. `V` toggles |
| `font_dir` | `/usr/share/sondechase/fonts` | Folder with `Orbitron-Bold.ttf` and `Rajdhani-SemiBold.ttf` |
| `fb_dev` | unset | Framebuffer. Unset: `$LV_LINUX_FBDEV_DEVICE`, else `/dev/fb_lcd` (the image's udev link for the st7789v node), else the `fb_st7789v` or `panel-mipi-dbi` entry of `/proc/fb`, else `/dev/fb0` |
| `kbd_dev` | auto | evdev keyboard. Empty: first device with Q, Enter and 1 keys |
| `data_dir` | `$HOME/sondechase` | App data: `sondechase.csv`, `log/`, `tiles/`. Files from before 0.5.1 are moved in on first start |
| `demo` | `0` | `1` simulated flight, see [Demo mode](#demo-mode) |
| `demo_speed` | `30` | Simulated seconds per real second in demo mode |

The shipped `conf/sondechase.conf` leaves `freq_hz` and `ground_alt` commented out, so the app opens on the Radio
screen and uses the GPS altitude as ground level.

## Map tiles

The Map works without a pack: with `tile_online = 1` it downloads only the tiles on screen, at most two at a time,
and caches them. The tile server's usage policy forbids bulk downloading for offline use, so the app does not
prefetch.

A preloaded pack is optional. `tools/prep_tiles.py` converts tiles you are allowed to use offline into the UI's
dark palette:

```sh
tools/prep_tiles.py --mbtiles vietnam.mbtiles --bbox 105.3,20.5,106.6,21.5 --zoom 8-14 --out tiles
```

- Input: an MBTiles file (raster PNG/JPEG) or a `z/x/y.png` directory. The script does not download anything. It
  needs the Pillow package (`python3-pil`).
- Output: `<out>/<z>/<x>/<y>.png`, 256×256, recoloured. Copy it to `tile_dir`, for example
  `/usr/share/sondechase/tiles`.
- `--recolor dark` uses a colour-distance heuristic. Tune it if the source style is far from standard OSM.

Failure messages and tile styles are described in [system architecture](./system-architecture.md#map-and-tiles).

## Demo mode

`demo = 1` plays a simulated flight through the same frame path as real frames: ascent at 5 m/s to 31.2 km, burst,
parachute descent, landing, then signal loss after 90 s. No SDR, GPS or decoder is needed. The receiver is not
opened, so radio screens show the demo band overview. `demo_speed` sets the speed (default 30×).

## Screenshots

`tools/render.c` runs the real UI code on an in-memory 320×170 display with demo data and writes one 320×170 PPM
per screen. Build and run it on any Linux host (verified in `debian:trixie`):

```sh
cmake -S . -B build/render -DSC_RENDER=ON
cmake --build build/render -j
./build/render/sc_render <output dir> assets/fonts <tile dir> [download cache dir]
```

- `<tile dir>` is the cache or pack to draw map tiles from. The optional `[download cache dir]` sets
  `tile_cache_dir`. `SC_RENDER_MISSING=<file>` writes the list of tiles the shots need instead of drawing an empty
  grid.
- The images in `screenshots/` (used by `app-builder.json`) and the 2× mockups in `screenshots/mockup/` come from this
  tool. `screenshots/device/` holds framebuffer captures from the device; those cannot be regenerated by the tool.

## Release procedure

Steps used for 0.4.0 to 0.6.0. Keep the version identical in all places; the CMake configure step fails if
`CMakeLists.txt` and `app-builder.json` differ.

1. Set the version in `CMakeLists.txt` (`project(sondechase VERSION x.y.z ...)`) and in `app-builder.json`
   (`"version"`).
2. Update the version and the `.deb` file name in `README.md` and `README.vi.md`, and any changed behaviour or
   facts in both READMEs and `docs/`. Regenerate the screenshots in `screenshots/` when the UI changed (see
   [Screenshots](#screenshots)).
3. Run the checks locally: `ctest` on a Linux core build, `python3 tools/validate_app_builder.py`.
4. Commit as `chore: version x.y.z` and push to `main`.
5. The `package` workflow runs on `ubuntu-24.04-arm` in `debian:trixie`, builds the `.deb` with
   `cmake --workflow --preset cp0-native-package`, runs `ctest`, and uploads the artifact `sondechase-deb`.
6. Download the artifact, compute its checksum (`sha256sum sondechase_x.y.z-rev1_arm64.deb`), and check the
   package name and version with `dpkg-deb -I`.
7. Tag the same commit `vx.y.z` and create a GitHub release with the `.deb` attached and the SHA-256 in the notes.
8. Publish to the CardputerZero store with `czdev publish --deb <deb>` (from CardputerZero/AppBuilder). It opens a
   pull request on CardputerZero/packages; an update of an existing app merges by itself once its CI passes. The
   manifest rules are checked by `tools/validate_app_builder.py`; run the skill's `prepublish_check.py` first (see
   [`PUBLISH.md`](../PUBLISH.md)).
9. Update [`PUBLISH.md`](../PUBLISH.md): version, package path, release link, PR and deploy run, submission time.


## CI workflows

| Workflow | File | Runs on | Steps |
|---|---|---|---|
| `tests` | `.github/workflows/tests.yml` | `ubuntu-24.04` | `cmake -S . -B build -G Ninja -DSC_BUILD_UI=OFF`, build, `ctest` |
| `package` | `.github/workflows/package.yml` | `ubuntu-24.04-arm`, container `debian:trixie` | Native package workflow, `ctest` on `build/cp0-native`, artifact `sondechase-deb` |
| `validate` | `.github/workflows/validate.yml` | `ubuntu-24.04` | `python3 tools/validate_app_builder.py` |

All three run on `push` and `pull_request`. Badges for them are in both READMEs.

## Device checks

Measured by the maintainer on a CardputerZero (Debian 13, RTL-SDR Blog V4 with R828D, sonde Y0532351 at 403.000 MHz),
except the last two rows, measured on 2026-11-20 (HackRF One and the compass). The RTL-SDR rows were not re-measured
for this document.

| Check | Result |
|---|---|
| Decode, 60 s | 55 frames (first after 4.2 s, then about one per second); `rtl_fm` pipeline: 58 |
| CPU while decoding | 15.6 % of one core for the whole app (`rtl_fm` alone used 61.5 %) |
| Scan | full 400–406 MHz pass about every 0.25 s, 12.8 % CPU, sonde at +55 dB |
| Dropped samples | 0 in the IQ ring, 0 in the decoder pipe |
| Retune (R828D, I2C) | 32.7 ms. Scan → decode switch 621 ms when the dongle was reopened (it now stays open; not re-measured) |
| Listening (spectrum and FM audio, no decoder) | 22.6 % of one core; a broadcast FM station at 100.0 MHz heard; an empty 403 MHz channel squelched to a murmur |
| Display, keyboard, PipeWire audio, magnetometer node, OSM tile download | Working |
| Audio stream cost (pacat + PipeWire) | About 3.9 % of one core |
| HackRF One (libhackrf 2024.02.1), 2026-11-20 | Scan and WFM audio clear. About 20 % of one core while decoding and playing WFM, before the DSP speed-ups (see [system architecture](./system-architecture.md#decoder-front-end-dspc)). RS41 decoding not tried |
| Compass calibration with the lean step, 2026-11-20 (about 20° inclination, 40 s recording while leaning) | Tilt-compensated heading 2.7° RMS up to 35° of lean (flat formula 12.3°), 7.0° at 35–50° (flat 27.8°) |

Not yet checked on hardware: GPS fix (no module fitted at the time), the proximity beeper in the field, burst
detection and landing prediction on a real flight, AM and SSB on real signals, the simple map style and panning on
the device, drawing speed with many map tiles, HackRF RS41 decoding, HackRF sensitivity against an RTL-SDR. See
[roadmap](./project-roadmap.md#known-gaps).

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| "No SDR device found" | No RTL2832U or HackRF on USB, or it is another model. Check `lsusb` for `0bda:2838`, `0bda:2832` or `1d50:6089`. Set `sdr_check = 0` only to test without a dongle |
| Status says the device is busy or librtlsdr is missing | Another program holds the dongle (for example `rtl_fm`), or `librtlsdr0` is not installed (`apt install librtlsdr0`) |
| Status says `libhackrf0 not installed` | A HackRF is plugged in but its library is missing, which the package normally pulls in. Only a binary copied by hand gets there: `sudo apt install libhackrf0` |
| HackRF: weak or no signal | It has no AGC: raise `hackrf_gain` (for example `70`, which turns on more VGA) or set `bias_tee = 1` for a powered LNA. Its 8-bit ADC and unfiltered front end make it less sensitive at 400 MHz than an RTL-SDR |
| `GPS 0/3` in red on the top bar | The receiver is plugged in and talking, but has no fix yet. Decoding is unaffected |
| `No GPS` | gpsd is not running on `gpsd_host:gpsd_port`, or no receiver is attached. Install `gpsd` and start it with the receiver |
| Map shows `Map server blocked by DNS` | A DNS filter answers `tile.openstreetmap.org` with a loopback address. Allow the domain in the filter |
| Map shows `No map stored for this area` | `tile_online = 0` and no tile on disk. Set `tile_online = 1` or add a pack |
| No beeper or radio audio | `pacat` (`pulseaudio-utils`) missing, PipeWire not running, or `audio_enable = 0`. `M` mutes the beeper, frame tick and radio audio |
| LED does not blink | `led_path` not writable by the app's user (the `input` group), or the path does not exist on this image |
| `test_core` does not build on macOS | Needs Linux headers (`linux/input.h`). Build on Linux or in a container |
