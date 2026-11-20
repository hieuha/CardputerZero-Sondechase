# SondeChase keyboard cheatsheet

Every key the app reacts to, screen by screen (version 0.5.1). Keys are not case sensitive.

`Sym` works like `Shift`: hold it and press the key with the symbol (`-` on `R`, `+` on `T`). The same key without
`Sym` keeps its letter action, so plain `R` restarts the decoder.

## Everywhere

| Key | Action |
|---|---|
| `4` `5` `6` `7` `8` | Track, Map, Graph, Radio, Info (the digits sit under the labels of the bottom bar) |
| `Tab` | Next screen: Track → Map → Graph → Radio → Info → Track |
| `S` | Scan 400–406 MHz and try each signal peak on the decoder; opens the Radio screen |
| `F` | Type a frequency in kHz (see [Frequency entry](#frequency-entry)) |
| `R` | Restart the decoder on the current frequency |
| `X` | Stop the receiver |
| `M` | Mute / unmute: proximity beeper, frame tick and radio audio. The red LED still blinks |
| Hold `Esc` 3 s | Quit, with a countdown on screen. A short `Esc` does not quit |

## Track (`4`)

No keys of its own. The big number is the distance to the sonde; `Go` says which way to walk or drive in words
(`Bear right, East`). The compass ring and `Go` turn green when the arrow to the sonde is within 15° of straight ahead.
While a sonde is decoded, each frame ticks and blinks the red LED. If GPS drops out, distance and direction are
measured from the device's last fix (yellow, `from last GPS`). Exact angles are on Info 2/2.

## Map (`5`)

| Key | Action |
|---|---|
| `←` `→` `↑` `↓` | Pan; hold to keep moving. The map stops following (`free`) |
| `Sym` + `T` (printed `+`) | Zoom in (up to `zoom_max`, default 18) |
| `Sym` + `R` (printed `-`) | Zoom out (down to `zoom_min`, default 6) |
| `C` | After panning: follow again as before. Otherwise cycle: both → device → sonde |
| `V` | Simple / detailed map style |

Each frame ticks and blinks the red LED here too.

## Graph (`6`)

| Key | Action |
|---|---|
| `←` / `→` | `Graph 1/2`: Skew-T diagram ↔ `Graph 2/2`: altitude and vertical speed over time |

## Radio (`7`)

Tuned view (listening or decoding one frequency):

| Key | Action |
|---|---|
| `←` / `→` | Tune −10 / +10 kHz; hold to keep tuning (only listens) |
| `↓` / `↑` | Tune −100 / +100 kHz; hold to keep tuning |
| `Enter` | Decode the frequency being listened to |
| `D` | Mode: NFM → AM → USB → LSB → WFM (broadcast FM) |
| `B` | Channel width for the mode (NFM 16 / 25 / 6 / 12 kHz, AM 6 / 10 kHz, USB and LSB 2.4 / 3 kHz) |

Scan (after `S`):

| Key | Action |
|---|---|
| (none) | Sweeps the band, tries the strongest peak for 10 s, stays on the first RS41 it decodes |
| `←` `→` `↑` `↓` | Move the cursor by hand (±10 / ±100 kHz); this stops the automatic choice |
| `Enter` | Tune to the cursor: listen and decode there |
| `Esc` | Back to the frequency used before Scan |

## Info (`8`)

| Key | Action |
|---|---|
| `←` / `→` | `Info 1/2`: sonde, reception, device ↔ `Info 2/2`: exact numbers (bearing, elevation, coordinates, weather) |
| `K` | Calibrate the compass (see below) |

### Compass calibration (`K` on Info)

Do it once, outdoors, away from cars and metal. Hold the device flat.

| Step | What to do |
|---|---|
| 1 | Turn slowly to your right on the spot, a full circle, until the ring is all green. Step 2 starts by itself |
| 2 | Point the top of the device north (use a phone compass), press `Enter`. Saved to `<data_dir>/compass.conf` |
| Any time | `Esc` cancels; nothing is changed |

On screens with pages, the page number is in the top-left label.

## Frequency entry

Opened by `F`.

| Key | Action |
|---|---|
| `0`–`9` | Digits, in kHz (up to 7 digits, for example `403500`) |
| `Backspace` | Delete the last digit |
| `Enter` | Tune and decode, if the frequency is inside the dongle's range (shown on screen) |
| `Esc` | Cancel |

## Burst alert

Shown over every screen when the balloon bursts.

| Key | Action |
|---|---|
| `Enter` | Open the Map on the sonde and the landing point |
| `Esc` | Dismiss the alert |

## No SDR screen

Shown while no RTL-SDR is plugged in. Only holding `Esc` (quit) works; the app resumes by itself once the dongle appears.
