# Code Standards

Last updated: 2026-11-20 (version 0.5.1). Describes the conventions the code already follows, so new code matches.

## Contents
- [Language and build](#language-and-build)
- [Layout](#layout)
- [Public header](#public-header)
- [Naming](#naming)
- [Comments](#comments)
- [Error handling and resource rules](#error-handling-and-resource-rules)
- [UI rules](#ui-rules)
- [Configuration keys](#configuration-keys)
- [Tests](#tests)
- [Tools and scripts](#tools-and-scripts)
- [File size](#file-size)
- [Documentation with every change](#documentation-with-every-change)
- [Commits and branches](#commits-and-branches)
- [Known warnings](#known-warnings)

## Language and build

- C11 (`CMAKE_C_STANDARD 11`). No C++ and no compiler extensions beyond `_GNU_SOURCE` for POSIX APIs.
- Warnings: `-Wall -Wextra` on `sc_core`, `test_core`, `sondechase` and `sc_render`. Keep new code free of warnings
  under GCC 14 (see [known warnings](#known-warnings)).
- Default build type is `Release` (set in `CMakeLists.txt`).
- The core (`sc_core`, everything in `src/` except `main.c`, `ui.c`, `keys.c`) must not include `lvgl.h`. That keeps
  it testable on any Linux host with `-DSC_BUILD_UI=OFF`.
- Linux-only APIs are allowed (`dlopen`, evdev, sysfs, `prctl`, process groups). `keys.c` includes `<linux/input.h>`.

## Layout

```
src/
  sc.h          public header: every type, constant and module API of the core
  ui.h          UI entry points (ui_init, ui_key, ui_should_quit)
  main.c        entry point and main loop
  app.c         application state, receiver, decoder pipeline, frames, logs
  base.c        config, line reader, child processes, JSON fields
  sdr.c         librtlsdr loaded at run time, IQ ring
  dsp.c         FFT and decoder front end
  radio.c       audio demodulators
  io.c          gpsd, magnetometer, USB presence, spectrum cells
  geo.c         geodesy, sonde JSON, wind, landing prediction
  track.c       history, burst, beeper, audio stream, LED
  tiles.c       tile lookup, download, recolour
  demo.c        simulated flight
  keys.c        evdev keyboard
  ui.c          LVGL screens
tests/          test_core.c, fake_rtlsdr.c, fake_decoder.sh
tools/          render.c, prep_tiles.py, make_icon.py, validate_app_builder.py
cmake/          package layout, decoders, toolchain (from the CardputerZero Template, MIT)
conf/           sondechase.conf, installed as /etc/sondechase.conf
```

- One module per `.c` file, named after its job in one short lowercase word (`app.c`, `geo.c`). This repo's
  convention is short module names, so new files follow it rather than the longer kebab-case used elsewhere.
- Add a file only for a real boundary: a new module with its own state, or a separate program.

## Public header

- `src/sc.h` is the only header shared by the core modules and the tests. Every type, constant and function a
  module exposes is declared there with a short comment stating units and ranges (for example `/* m, haversine */`,
  `/* deg true, 0..360 */`).
- `ui.h` is the only header for the UI.
- Internal helpers are `static` in their `.c` file and are not declared in `sc.h`.

## Naming

| Kind | Rule | Examples |
|---|---|---|
| Functions | `module_verb`, lowercase snake case | `sonde_parse`, `predict_landing`, `gps_poll`, `tf_request`, `app_on_frame` |
| Public types | `name_t`, lowercase | `cfg_t`, `app_t`, `sonde_t`, `radio_t`, `iqring_t` |
| Structs, enums | Enum constants with a module prefix | `ST_DECODE`, `PH_ASCENT`, `DEMOD_NFM`, `HDG_GPS`, `TF_WHY_DNS_BLOCKED` |
| Macros and constants | `UPPER_SNAKE` with a module prefix and unit in the name where useful | `DDC_RATE`, `SWEEP_KEEP`, `HUNT_TRY_S`, `TF_RETRY_S`, `RS41_TUNE_OFFSET_HZ`, `ESC_QUIT_S` |
| Frequencies and times | Suffix `_hz`, `_s`, `_m`, `_db` on the field or macro | `freq_hz`, `scan_lo_hz`, `stale_s`, `prox_radius_m`, `db` |
| Config fields | Same name as the config key | `cfg.tile_cache_mb` ↔ `tile_cache_mb = 1000` |
| Local variables | Short lowercase names are normal in the DSP and geodesy code (`lat`, `lon`, `dh`) | `dt`, `xe`, `xn` |

- Function and variable names keep the case of the protocol they read: JSON keys in `sonde_parse` are the
  lowercase keys of `rs41mod` (`"lat"`, `"vel_v"`), and sysfs and gpsd names are kept as written.
- `LVGL` identifiers keep their `lv_` / `LV_` prefixes.

## Comments

- C block comments only (`/* ... */`). The code has no `//` comments. Multi-line comments use the `/*` on its own
  line and ` * ` for continuation lines.
- Explain the invariant, the unit or the reason (`/* Sweep keeps the central 80 % of each FFT: flat floor */`).
  Do not describe what the next line obviously does.
- Do not put plan IDs, phase numbers, audit labels or finding codes in code, tests, test names, migration names or
  commit messages.
- No `TODO`, `FIXME` or `XXX` markers in the tree. Record open items in [the roadmap](./project-roadmap.md#known-gaps)
  instead.

## Error handling and resource rules

- Functions that can fail return `0` or `-1`, or `NULL` with a reason in an `err` buffer (`sdr_open`). Callers either
  show the reason on the status line (`set_status`) or retry after a delay (`t_restart`).
- Parsers never trust input length: `snprintf` into sized buffers, `js_str` takes a buffer size, and `lr_readline`
  bounds lines at `LR_MAX`.
- Child processes run through `sh -c` in their own process group (`proc_start`, `proc_start_feed`). Stop them with
  `proc_stop` or `reap_group`. The app is a child subreaper, so orphaned members are collected.
- The USB thread never blocks the producer: `iqring_write` drops and counts instead of waiting.
- Keep no unbounded buffers: histories are capped (`HIST_MAX` 2048 points, decimated), tile caches are capped
  (`tile_cache_mb`), the audio queue is fixed size (`AUDIO_RADIO_MAX`).
- Signal handling: `SIGINT`/`SIGTERM` set a flag; `SIGPIPE` is ignored. Holding `Esc` for `ESC_QUIT_S` quits.
- Do not write to the console from the core except on startup errors (`fprintf(stderr, ...)`, for example unknown
  config keys).

## UI rules

- Immediate-mode: the screen is painted in one `LV_EVENT_DRAW_MAIN` callback (`draw_cb`) from `app_t`. A 200 ms
  timer invalidates the root object (`tick_cb`). Do not create per-value LVGL objects for live data.
- Drawing helpers (`fill`, `ln`, `txt`, `kv`) and colours (`C_*` constants) are shared by all screens. Use them.
- Key handling is centralised in `ui_key`. A new key binding goes there and into the table in the
  [README](../README.md#keys) (and `README.vi.md`).
- The screen is 320×170. Text must fit its box; long text is truncated, not wrapped.
- Keep the chrome consistent: top bar (name, frequency, state, GPS, mute, battery) and bottom navigation bar.

## Configuration keys

Adding a key takes four edits, all in the same change:

1. Add the field to `cfg_t` in `src/sc.h` with a unit comment.
2. Set its default in `cfg_defaults` (`src/base.c`).
3. Add an entry to the `E(key, type)` table in `src/base.c` (`l` long, `i` int, `d` double, `s` string).
4. Document it in `docs/deployment-guide.md` and the sample `conf/sondechase.conf`.

Unknown keys are reported on stderr and ignored. `#` starts a comment at the start of a line or after a space.

## Tests

- `tests/test_core.c` is a single program with a `t_<module>()` function per area (`t_geo`, `t_isa`, `t_predict`,
  `t_parse`, `t_heading`, `t_subst`, `t_cfg`, `t_spec`, `t_ring`, `t_dsp`, `t_radio`, and more). `main()` runs them all
  and prints `N checks, M failed`.
- Assertions:
  - `CHECK(expr)`: boolean, reports the file, line and expression on failure.
  - `NEAR(a, b, tol)`: `|a − b| ≤ tol`, for floating-point results. Give the tolerance that the physics or the
    rounding justifies (for example `NEAR(geo_dist(0, 0, 1, 0), 111195.1, 1.0)`).
- Add tests in the matching `t_` function. A new module gets its own `t_` function.
- Test the behaviour with the fake receiver and decoder where the path crosses processes: `tests/fake_rtlsdr.c`
  (a 403.000 MHz carrier wherever it is tuned) and `tests/fake_decoder.sh` (prints rs41mod-style JSON). Do not add
  mocks that change production behaviour.
- Test names describe the behaviour (`CHECK(gps_sky(&g, 5.0))`), not the plan or the ticket.
- Verify with `ctest --test-dir build --output-on-failure` after a change in the core. Run the full package build
  (see [deployment guide](./deployment-guide.md#build-on-the-device)) for changes that touch packaging or the UI.

## Tools and scripts

- `tools/validate_app_builder.py`: standard library only. Checks `app-builder.json` against the store rules.
- `tools/make_icon.py`, `tools/prep_tiles.py`: need Pillow. Not run by CI.
- `tools/render.c`: same UI code, headless. Use it for screenshots (see [deployment guide](./deployment-guide.md#screenshots)).
- Shell commands in `audio_cmd`, `tile_fetch_cmd` and `decoder_feed_cmd` are configuration, not code. They run through
  `sh -c`, so quote paths.

## File size

- The project guideline is 200 lines per module as a split candidate and 800 lines per documentation file.
- Current sizes are listed in the [codebase summary](./codebase-summary.md). `src/ui.c` (1271 lines) is the largest
  and is the first candidate for splitting by screen (Track, Map, Graph, Radio, Info, overlays) if it keeps growing.
  Split only where it removes real complexity, and keep `ui_key` and the draw helpers shared.

## Documentation with every change

A change that touches behaviour, keys, screens, config keys, files on disk or the build updates, in the same commit:

| What changed | Update |
|---|---|
| Any key, or what a key does on a screen | [`docs/keyboard-cheatsheet.md`](./keyboard-cheatsheet.md) and the Keys table in both READMEs |
| User-visible behaviour, screens, logs, data folder | `README.md` and `README.vi.md` (same content, Vietnamese written with natural vi-VN terms) |
| Config keys or defaults | `conf/sondechase.conf`, [deployment guide](./deployment-guide.md#configuration) |
| Modules, data flow, peripherals, algorithms | [system architecture](./system-architecture.md), [codebase summary](./codebase-summary.md) |
| A release | [roadmap](./project-roadmap.md), version in the READMEs and in each doc's "Last updated" line |

Check the docs against the code before committing; do not describe behaviour that is planned but not built.

## Commits and branches

- Conventional commits, lowercase type and scope: `feat(radio): ...`, `fix(log): ...`, `docs: ...`, `chore: version 0.5.0`,
  `refactor: ...`, `ci: ...`. Imperative subject, no trailing period.
- No AI or assistant references in commit messages or PR text.
- Version bumps are their own `chore: version x.y.z` commit (see [release procedure](./deployment-guide.md#release-procedure)).
- Work on `main` in this repository. Keep commits focused: one behaviour per commit.
- Never commit `dist/`, `build/`, `.cache/`, dotenv files, tokens or personal data (see `.gitignore`).

## Known warnings

The core builds with `-Wall -Wextra` under GCC 14.2 (Debian 13) with these warnings, which are not yet fixed:

| File | Warning | Why it is not a bug today |
|---|---|---|
| `src/app.c:10` (`set_status`) | `-Wformat-truncation`: 95-byte source into the 80-byte `status` | Status texts are short; the compiler assumes the longest possible argument |
| `src/app.c:333` (`known_set`) | 19-byte ID into the 16-byte `known[].id` | RS41 serials seen so far are 8 characters; a longer ID would be cut |
| `src/app.c:355` (`log_autorx`) | `%03ld` into the tail of the 32-byte timestamp | The value is a millisecond count, 0 to 999 |
| `src/tiles.c:57` | Path built with `snprintf` into a 700-byte buffer | Paths come from config and cache names; the compiler assumes longer input |

Fix by sizing the buffers to the worst case or by truncating explicitly. The warnings are tracked in the
[roadmap](./project-roadmap.md#known-gaps).
