# Project Roadmap

Last updated: 2026-11-20 (version 0.5.1). Versions come from `chore: version` commits and GitHub releases. The
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
| 0.5.1 | 2026-11-20 | `6f6f036`..`v0.5.1` | `Z` (Sym+Z, printed +) zooms the map in; zoom range 6–18; app data grouped in `data_dir` (`~/sondechase`: `sondechase.csv`, `log/`, `tiles/`), older files moved in; map tile cache 1000 MB for offline use; one CSV header per file; auto_rx log timed by the sonde's GPS time; inline `#` comments in the config; versioned tile User-Agent; live spectrum only on the Radio screen |

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
| Compass not calibrated; hard-iron offset on this board; no tilt compensation | `mag_off_x`, `mag_off_y`, `mag_rot_deg`, `mag_flip_y` must be set by hand |
| Skew-T stops at 100 hPa; no moist adiabats or mixing-ratio lines | Upper part of the diagram is not shown |
| Landing prediction during ascent depends on `burst_alt`; no wind above the current altitude | Estimate changes as the balloon climbs |
| Flight history keeps 2048 samples (decimated) and is not restored after an app restart | Graphs start empty after a restart |
| The CSV log has no SNR or frequency error | Those fields are `-1` in the auto_rx-format log |

### Code and configuration issues

| Issue | Where | Note |
|---|---|---|
| Four `-Wformat-truncation` sites under GCC 14.2 | `src/app.c` (lines 10, 333, 355), `src/tiles.c` (line 57) | See [code standards](./code-standards.md#known-warnings) |
| `ui.c` is 1271 lines | `src/ui.c` | Candidate for a split by screen |

## Dropped or declined

These were considered and are **not planned**:

| Item | Status |
|---|---|
| SNR and frequency-error measurement | Dropped by maintainer decision. The logs write `-1` for these fields |
| Download of GFS wind data for the landing prediction | Declined by maintainer decision |
