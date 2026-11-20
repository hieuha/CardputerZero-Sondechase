# Project Roadmap

Last updated: 2026-11-20 (version 0.6.0). Versions come from `chore: version` commits and GitHub releases. The
"Known gaps" list comes from the test status and limitations in the README and from the code. Nothing here is a
commitment to a future feature.

## Contents
- [Released versions](#released-versions)
- [Known gaps](#known-gaps)
- [Dropped or declined](#dropped-or-declined)

## Released versions

| Version | Released (GitHub, UTC) | Commit range | Main changes |
|---|---|---|---|
| 0.3.0 | not on GitHub Releases | `f64e6fa` | First source and README |
| 0.4.0 | 2026-11-19 03:57 | `f64e6fa`..`539c908` | In-process librtlsdr receiver (one open dongle); FFT spectrum and rtl_fm-equivalent front end; in-process sweep; decoding in process with live spectrum; manual or automatic scan with peak marks; pacat beeper; keys 4–8; hold `Esc` to quit; telemetry before the GPS fix; map before a position is known; decoders and store metadata in the package; GPL-3.0-or-later licence |
| 0.4.1 | 2026-11-19 04:08 | `539c908`..`59e89c7` | Tune and listen like an SDR, `Enter` sets decode; NFM, AM, USB, LSB audio with selectable width (Scan tab becomes Radio); held arrow keys keep tuning; simple map style; map pan and zoom; DNS block messages for the map; CI (tests, arm64 package, store checks); launcher icon with a radiosonde; config fix for long `rtlsdr_lib` paths |
| 0.5.0 | 2026-11-19 14:12 | `59e89c7`..`6f6f036` | `Tab` switches screens; SDR-style start on the Radio screen; WFM audio; automatic Scan that tries peaks; tuner-range frequency entry; frame tick and ACT LED on Track and Map; GPS satellite counts; auto_rx-format log per sonde; green compass ring within 15° |
| 0.5.1 | 2026-11-20 | `6f6f036`..`v0.5.1` | `Z` zooms the map in; zoom range 6–18; app data grouped in `data_dir` (`~/sondechase`: `sondechase.csv`, `log/`, `tiles/`), older files moved in; map tile cache 1000 MB for offline use; one CSV header per file; auto_rx log timed by the sonde's GPS time; inline `#` comments in the config; versioned tile User-Agent; live spectrum only on the Radio screen |
| 0.6.0 | 2026-11-20 | `v0.5.1`..`v0.6.0` | `Sym` + `R` (`-`) zooms out and `Sym` + `T` (`+`) zooms in (the driver sends them as keypad 7 and 8, which the app ignored); `Z` no longer zooms; `Q` no longer quits (hold `Esc` 3 s only, so a stray key cannot close the app); Track in plain words (distance labelled, `Go: Bear right, East`, height, drift; no degrees) and kept going from the last device fix when GPS drops out; Info 2/2 with the exact numbers; compass calibration in the app (Info, `K`, saved to `<data_dir>/compass.conf`); page number in the top-left label (`Graph 1/2`, `Info 1/2`); altitude label no longer cut off |
| Unreleased | | `v0.6.0`.. | The LCD framebuffer is found at start (`/dev/fb_lcd`, else `/proc/fb`) instead of a fixed `/dev/fb0`, so the app stays on the LCD when HDMI is attached; a map download whose command would not fit its buffer is skipped instead of running cut; no compiler warnings under GCC 14.2; UI split into one file per screen (no visible change) |

Notes:
- Version bumps touch `CMakeLists.txt`, `app-builder.json` and both READMEs (see
  [release procedure](./deployment-guide.md#release-procedure)).

## Known gaps

Grouped by kind. Each item is something the code or the test status says is missing or unverified.

### Not yet verified on hardware

| Gap | Source |
|---|---|
| GPS fix and chasing in the field (no GPS module was fitted when the device checks were run) | README test status |
| Proximity beeper in the field | README test status |
| Burst detection and landing prediction on a real flight | README test status |
| `cp0-cross` cross build (the native build was checked) | README test status |
| Drawing speed with many map tiles | README test status |
| AM and SSB audio on real signals (checked with the fake receiver and rendered screenshots only) | README test status |
| Simple map style and map panning on the device | README 0.4.0 notes |
| Scan → decode switch time after the dongle stays open (the 621 ms figure predates that change) | README test status |

### Functional limitations

| Limitation | Effect |
|---|---|
| RS41 only | Other sonde types need their own decoder and `fsk_demod` parameters |
| Weak spurs near a strong sonde (402.88 and 404.6 MHz seen with the sonde a few cm from the antenna) | The overview shows them; tuning to them shows nothing to decode |
| No tilt compensation; in-app calibration (Info, `K`) not yet tried on the device's magnetometer | Hold the device flat; heading from GPS course above 5 km/h is unaffected |
| Skew-T stops at 100 hPa; no moist adiabats or mixing-ratio lines | Upper part of the diagram is not shown |
| Landing prediction during ascent depends on `burst_alt`; no wind above the current altitude | Estimate changes as the balloon climbs |
| Flight history keeps 2048 samples (decimated) and is not restored after an app restart | Graphs start empty after a restart |
| The CSV log has no SNR or frequency error | Those fields are `-1` in the auto_rx-format log |

### Code and configuration issues

None known. The core and the UI build without warnings under GCC 14.2 (`-Wall -Wextra`).

## Dropped or declined

These were considered and are **not planned**:

| Item | Status |
|---|---|
| SNR and frequency-error measurement | Dropped by maintainer decision. The logs write `-1` for these fields |
| Download of GFS wind data for the landing prediction | Declined by maintainer decision |
