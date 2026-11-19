# SondeChase keyboard cheatsheet

Every key the app reacts to, screen by screen (version 0.5.1). Keys are not case sensitive.

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
| `Q` | Quit |
| Hold `Esc` 3 s | Quit, with a countdown on screen. A short `Esc` does not quit |

## Track (`4`)

No keys of its own. While a sonde is decoded, each frame ticks and blinks the red LED. The compass ring turns green
when the arrow to the sonde is within 15° of straight ahead.

## Map (`5`)

| Key | Action |
|---|---|
| `←` `→` `↑` `↓` | Pan; hold to keep moving. The map stops following (`free`) |
| `Z` (`Sym` + `Z`, printed `+`) | Zoom in (up to `zoom_max`, default 18) |
| `-` | Zoom out (down to `zoom_min`, default 6) |
| `C` | After panning: follow again as before. Otherwise cycle: both → device → sonde |
| `V` | Simple / detailed map style |

Each frame ticks and blinks the red LED here too.

## Graph (`6`)

| Key | Action |
|---|---|
| `←` / `→` | Page 1/2: Skew-T diagram ↔ page 2/2: altitude and vertical speed over time |

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

No keys of its own.

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

Shown while no RTL-SDR is plugged in. Only `Q` (quit) works; the app resumes by itself once the dongle appears.
