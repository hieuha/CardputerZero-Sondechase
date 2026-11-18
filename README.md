# SondeChase

**English** | [Tiếng Việt](README.vi.md)

RS41 radiosonde receiver, decoder and chase aid for the M5Stack CardputerZero
(Raspberry Pi CM0, 320×170 LCD, 46-key keyboard) with an RTL-SDR v4 and GPS.

Version 0.3.0. Read "Test status" before use: the UI has been compiled and rendered with LVGL 9.5
on a PC; the hardware paths have not yet run on a real device.

## Architecture

```
┌────────────┐    ┌────────┐ IQ 48 kHz ┌───────────┐ bits 4800 Bd ┌─────────┐
│ RTL-SDR v4 ├─┬─►│ rtl_fm ├──────────►│ fsk_demod ├─────────────►│ rs41mod ├───┐ JSON frames
└────────────┘ │  └────────┘           └───────────┘              └─────────┘   │
               │  ┌───────────┐                                                 │
               └─►│ rtl_power ├── CSV 400–406 MHz, scan only ───────────────────┤ spectrum
                  └───────────┘                                                 │
┌────────────┐    ┌──────┐                                                      │
│ GPS        ├───►│ gpsd ├── JSON, TCP 2947 ────────────────────────────────────┤ position
└────────────┘    └──────┘                                                      │
┌────────────┐                                                                  │
│ Magnet.    ├── IIO sysfs ─────────────────────────────────────────────────────┤ heading
└────────────┘                                                                  │
    ┌───────────────────────────────────────────────────────────────────────────▼──────┐
    │  sondechase  ·  C, LVGL 9.5, /dev/fb0 + evdev                                    │
    │                                                                                  │
    │  app.c    state machine IDLE / DECODE / SCAN                                     │
    │  geo.c    distance, bearing, elevation, ISA, landing prediction                  │
    │  track.c  flight history, burst detection, beeper, audio, tiles                  │
    │  io.c     gpsd, heading, rtl_power spectrum                                      │
    │  ui.c     7 screens + alert, drawn with line / rect / label                      │
    └───────────────────────────────────────────────────────────────────────────┬──────┘
                                                                                │ PCM 16 kHz over a pipe
                                                                            ┌───▼───┐
                                                                            │ aplay │
                                                                            └───────┘
```

`radiosonde_auto_rx` does not run. The app only uses the `rs41mod` and `fsk_demod` binaries built
from that project's source tree, and does SDR control, scanning, tracking, prediction and UI itself.

The RTL-SDR serves one process at a time: while scanning (`rtl_power`) the decode pipeline is
stopped; after a peak is chosen, the decode pipeline restarts on the new frequency.

## Screens and keys

| Key | Function |
|---|---|
| `1` Track | HUD compass, distance, bearing, elevation, predicted landing point |
| `2` Map | Preloaded tiles, device position, sonde, flight track, landing point. `Up/Down` zoom, `C` cycles follow mode (both / device / sonde) |
| `3` Graph | `Left/Right` switch page: Skew-T log-P, altitude + vertical speed over time |
| `4` Scan | 400–406 MHz spectrum. `Left/Right` select peak, `Enter` tune, `S` rescan |
| `5` Info | Sonde, reception, device, one pair of target coordinates |
| `F` | Enter frequency (kHz) |
| `M` | Mute / unmute beeper |
| `R` / `X` / `Q` | Restart decoder / stop / quit |

The burst alert overlays every screen: `Enter` opens Map, `Esc` dismisses.

**Missing hardware states**

- No SDR on USB (VID:PID `0bda:2838` or `0bda:2832`): a blocking "No SDR device found" screen.
  The app rechecks USB every 2 s and resumes decoding when the device appears. Disable the check: `sdr_check = 0`.
- No GPS fix: sonde decoding is unaffected. The top bar shows `GPS lost` (had a fix before)
  or `No GPS`. The Track screen replaces distance, bearing and elevation with the absolute altitude and
  coordinates of the sonde and of the predicted landing point. Map follows the sonde. The beeper stays silent.

## Algorithms

**Landing prediction** (`geo.c`, `predict_landing`)

- Descent rate under parachute at terminal velocity: `v(h) = v_sl · sqrt(ρ0 / ρ(h))`, `ρ(h)` from ISA.
- While the sonde descends, `v_sl` is calibrated from the measured vertical speed: `v_sl = |vv| / sqrt(ρ0 / ρ(h))`.
- While the sonde ascends: assumes burst altitude `burst_alt` and `descent_v0` from the config file.
- Horizontal drift integrated in 100 m layers: `Δx = w(h) · Δh / v(h)`. `w(h)` comes from the wind profile
  the sonde itself measured during ascent (250 m bins). Unobserved layers use the current horizontal velocity.

**Burst detection** (`track.c`, `flight_update`): vertical speed < −3 m/s for 3 consecutive frames,
altitude > 1000 m, after an ascent phase has been observed. If the app starts while the sonde is already
descending: the phase changes, no alert is raised.

**Proximity beeper** (`track.c`): horizontal distance from the device to the sonde (or to the last fix
after signal loss).

- On when d ≤ 10 km, off when d > 10.5 km.
- Period `T = 0.12 + 1.88 · (d / 10 km)^0.6` s; each beep 60 ms.
- Volume `V = 1 − 0.35 · log10(d / 100 m)`, clamped to 0.3–1.0.
- 2200 Hz while telemetry is updating, 1100 Hz after signal loss.

**Skew-T log-P** (`ui.c`): `y ∝ ln(1050 / p)` from 1050 to 100 hPa, isotherms skewed 45°,
dry adiabats `T = θ · (p/1000)^0.2857`. Dew point by Magnus (a = 17.62; b = 243.12 °C).
For RS41-SG (no pressure sensor), P is derived from altitude via ISA and shown with a `~` prefix.

**Spectrum scan** (`io.c`): a peak is a contiguous run of bins above `median + scan_snr_db`; the center
frequency is the power-weighted centroid, rounded to a 10 kHz grid.

## Map

The Map screen looks up each tile in this order:

1. Preloaded pack in `tile_dir` (PNGs already recolored to the UI palette, made with `tools/prep_tiles.py`).
2. Download cache in `~/.cache/sondechase/tiles` (original server PNGs; recolored when loaded into RAM).
3. If missing and `tile_online = 1`: fetch that tile over the network with `curl`, at most 2 concurrent
   connections. A failed tile is not retried for 30 s.

- Only visible tiles are fetched; the app never prefetches an area. This is the usage
  `tile.openstreetmap.org` allows; bulk downloading for offline use is forbidden.
- Every request sends a User-Agent identifying the app (`tile_user_agent`). The Map screen shows
  "© OpenStreetMap" when tiles are present.
- The cache is capped by `tile_cache_mb` (default 200 MB); when exceeded, the oldest tiles are deleted.
- Missing tiles show an empty grid with a status: `Loading map`, `No network: stored map only`,
  or `No map stored for this area` (when `tile_online = 0`).
- Recoloring (`tile_recolor` in `src/tiles.c`): the most frequent color of a tile is treated as land background;
  output brightness is proportional to color distance from that background, so roads, labels and borders become
  bright strokes on a dark background; bluish pixels become water.
- `tools/prep_tiles.py` can still preload an area from an MBTiles file or a `z/x/y.png` directory that you are
  allowed to use offline. It does not download tiles.

## Build and packaging

Install layout and `.deb` packaging follow the CardputerZero Template contract (`cmake/cm0-package.cmake`,
`cmake/toolchains/cp0-aarch64-linux-gnu.cmake`, same preset names as the Template):

```sh
# Cross build on a Debian/Ubuntu PC (needs crossbuild-essential-arm64, ninja-build, cmake >= 3.25).
# The first run downloads the CardputerZero BSP sysroot and LVGL v9.5.0.
cmake --workflow --preset cp0-cross-package
# -> dist/SondeChase_0.3.0_rev1_arm64.deb

# Or build on the device itself:
cmake --workflow --preset cp0-native-package

# Install on the CardputerZero:
sudo apt install --no-install-recommends ./SondeChase_0.3.0_rev1_arm64.deb
```

Package contents:

| Path | Contents |
|---|---|
| `/usr/bin/sondechase` | binary (LVGL, PNG and TTF decoders statically linked; depends only on `libc6`) |
| `/etc/sondechase.conf` | configuration (conffile) |
| `/usr/share/sondechase/fonts/` | Rajdhani SemiBold, Orbitron Bold |
| `/usr/share/sondechase/tiles/` | directory for a preloaded map pack (empty) |
| `/usr/share/APPLaunch/applications/sondechase.desktop` | launcher entry |
| `/usr/share/APPLaunch/share/images/sondechase.png` | 100×100 icon |

`Depends: libc6, curl`. Child processes run at use time and are **not in the package**: `rtl_fm`, `rtl_power`
(RTL-SDR Blog fork of rtl-sdr, for the v4), `fsk_demod`, `rs41mod`, `gpsd`, `aplay`. Install them with:

```sh
./scripts/install_decoders.sh
```

The package installs no systemd service: the app is opened from APPLaunch and runs as the launcher's user.
That user needs read/write access to `/dev/fb0`, `/dev/input/event*`, the ALSA device and the RTL-SDR (groups
`video`, `input`, `audio`, `plugdev`).

`app-builder.json` is the manifest for `czdev publish`; the Template's check script
(`validate_app_builder.cmake`) accepts it.

### Demo mode and screenshots

- `demo = 1` in the config file: the app plays a simulated flight (ascent at 5 m/s to 31.2 km, burst, parachute
  descent, landing, signal loss after 90 s) through the same processing path as real frames. No SDR, GPS or
  decoder needed. `demo_speed` is simulated seconds per real second.
- `tools/render.c` runs the real UI code on an in-memory display and writes a 320×170 image of each screen:

```sh
cmake -S . -B build-render -DSC_RENDER=ON && cmake --build build-render -j
./build-render/sc_render <output dir> assets/fonts <tile dir>     # writes .ppm files
```

The images in `screenshots/` come from this tool. The map background in the Map images is synthetic tiles used
to test the PNG loading path, not real map data.

PC build for interacting with the UI: `cmake -S . -B build-sim -DSC_SIM=ON` (needs `libsdl2-dev`; untested).
Core only, with tests: `cmake -S . -B build -DSC_BUILD_UI=OFF && cmake --build build && ctest --test-dir build`.

### Tile sources

`prep_tiles.py` does not download tiles. Its input is an existing raster MBTiles file or `z/x/y.png` directory:
tiles you rendered yourself from OSM data, or a provider's tile pack licensed for offline use.
The `tile.openstreetmap.org` server does not allow bulk downloading for offline use.
The `--recolor dark` filter converts light tiles to the UI's dark palette with a heuristic based on color
distance from the dominant background color; it needs tuning if the tile style differs much from standard OSM.

## Test status

Checked on an x86-64 host (gcc 13, `-Wall -Wextra -Werror`):

- Core: 4165 checks in `tests/test_core.c` pass — geodesy, ISA, dew point, landing prediction,
  rs41mod JSON and gpsd TPV parsers, heading, spectrum scan, burst detection, beeper, PCM sample generation,
  flight history, tile math, USB SDR detection, child pipeline lifecycle (start, read, stop the whole process
  group, auto-restart) with a fake decoder.
- `prep_tiles.py`: reads a synthetic MBTiles file, TMS axis flip, recoloring.
- `ui.c`: compiles and links against real LVGL v9.5.0, renders all 17 screen states headless with demo-mode
  data; TTF fonts (tiny_ttf) and PNG tiles (lodepng) load. Layout tuned from the rendered images.
- Demo mode: landing prediction error at 2976 m versus the simulated touchdown point is 79 m.
- `main.c` (framebuffer and SDL branches), `keys.c`: compile against real LVGL headers; not yet run.
- Tiles: download queue, de-duplication, atomic rename, failure backoff, cache cap and recoloring have unit tests
  (the download command is replaced by a fake script). The UI tile loading path runs clean under
  AddressSanitizer with both preloaded and "downloaded" tiles.
- Packaging: a trial `.deb` (amd64) built with CPack; contents and control checked with `dpkg-deb`.

Not yet checked:

- Drawing speed on the CM0 (each frame redraws the full screen; PNG tiles are decoded once, then held in a 4 MB cache).
- Real tile downloads from `tile.openstreetmap.org` (the build environment had no network access to it); the
  recolorer has only been tried on synthetic tiles, not real OSM tiles.
- The `cp0-cross` cross build (no aarch64 toolchain in the build environment) and installing the arm64 package on a device.
- The `rtl_fm | fsk_demod | rs41mod` pipeline with an RTL-SDR v4 and a real sonde; parameters taken from the auto_rx wiki.
- `scripts/install_decoders.sh`.
- Framebuffer, TCA8418 keyboard evdev, audio path (`aplay`), battery sysfs on the CardputerZero.
- Magnetometer: BMM150 behind the BMI270; the app reads `in_magn_*_raw` via IIO sysfs. Not verified that the
  Linux driver exposes these nodes. Axis orientation needs calibration via `mag_off_x/y`, `mag_rot_deg`,
  `mag_flip_y`. No tilt compensation yet.

## Known limitations

- RS41 only. Other sonde types need their own decoder and `fsk_demod` parameters.
- Skew-T stops at 100 hPa; moist adiabats and mixing ratio lines are not drawn yet.
- Landing prediction during ascent depends on the assumed `burst_alt` and has no wind above the current altitude.
- Flight history keeps 2048 samples (5 s/sample, downsampled automatically when full); not restored after an app
  restart. The full CSV log is written to `~/sondechase.csv`.

## License

SondeChase is released under the GNU General Public License, version 3 or (at your option) any later version (`GPL-3.0-or-later`), see [`LICENSE`](LICENSE).
This license allows bundling the upstream decoders in the repository or install package.

Third-party components:

| Component | License |
|---|---|
| `rs41mod` (zilog80, [rs1729/RS](https://github.com/rs1729/RS), via [radiosonde_auto_rx](https://github.com/projecthorus/radiosonde_auto_rx)) | GPL-3.0 |
| `fsk_demod` ([codec2](https://github.com/drowe67/codec2), David Rowe) | LGPL-2.1 |
| [LVGL](https://github.com/lvgl/lvgl) v9.5 | MIT |
| Files in `cmake/` taken from the CardputerZero Template | MIT, see `cmake/LICENSE-CardputerZero-Template.txt` |
| Rajdhani, Orbitron fonts | SIL OFL 1.1, see `assets/fonts/OFL-*.txt` |
| Map data | © OpenStreetMap contributors, ODbL |
