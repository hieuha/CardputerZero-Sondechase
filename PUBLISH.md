# Application Store Publish Record

The record of what SondeChase sends to the CardputerZero Application Store, kept in the format of the
`cardputer-zero-application` skill (<https://github.com/CardputerZero/skill>). Update it with every release: the
version, the package path and the submission result. The listing text itself lives in `app-builder.json`, which
`czdev publish` reads; this file points at it instead of copying it.

Publish with: `czdev publish --deb <deb>` (CLI, the way every release so far went). Web portal, not used:
<https://dev.cardputer.cc/#/upload>. Steps: [release procedure](docs/deployment-guide.md#release-procedure).

## Package

- Debian package path: `dist/sondechase_0.8.0-rev1_arm64.deb` (local build); released file attached to
  [v0.8.0](https://github.com/hieuha/CardputerZero-Sondechase/releases/tag/v0.8.0), built by the `package` workflow
- Package: `sondechase`
- Version: `0.8.0-rev1`
- Architecture: `arm64`
- Maintainer: `hieuha <hieuha@users.noreply.github.com>` (`APP_MAINTAINER` in `cmake/cm0-package.cmake`)
- APPLaunch desktop path: `/usr/share/APPLaunch/applications/sondechase.desktop`
  (from `cmake/templates/app.desktop.in`)
- Executable path: `/usr/bin/sondechase`

## Source and Ownership

- Public source repository: <https://github.com/hieuha/CardputerZero-Sondechase>
- Expected GitHub uploader/owner: `hieuha`
- Hide uploader email: `true` (Maintainer uses the GitHub noreply address)

## Listing

All in `app-builder.json` under `store`:

- Application name: `SondeChase`
- One-line summary (maximum 80 characters): `store.summary` (71 characters); Vietnamese in `store.locales.vi`
- Description: `store.description`
- Categories (maximum 6): `Radio & Comms`, `Hardware & IoT`
- Licence: `GPL-3.0-or-later`
- Permissions: IMU, network, additional hardware (RTL-SDR or, experimental, HackRF; GPS)

## Assets

- 256 x 256 source icon: `assets/images/sondechase_256.png`
- Icon packaged in `.deb`: `/usr/share/APPLaunch/share/images/sondechase.png` (100 x 100, from
  `assets/images/sondechase.png`)
- Screenshot 1 (320 x 170): `screenshots/01_track.png`
- Screenshot 2 (320 x 170): `screenshots/02_map.png`
- Screenshot 3 (320 x 170): `screenshots/04_radio_decode.png`
- Screenshot 4 (320 x 170): `screenshots/03_graph_skewt.png`
- Optional screenshot 5 (portal supports up to 6): `screenshots/03_graph_flight.png`
- Optional screenshot 6 (portal supports up to 6): `screenshots/07_burst.png`

The screenshots are drawn by the app's own UI code (`tools/render.c`); see
[Screenshots](docs/deployment-guide.md#screenshots).

## Preflight

Checked for 0.8.0:

- [x] `.deb` exists and can be parsed.
- [x] Control fields contain Package, Version, Architecture, and Maintainer.
- [x] Architecture is appropriate for the package (`arm64` for native binaries).
- [x] APPLaunch `.desktop`, executable, and PNG icon exist inside the package.
- [x] Package name ownership and update version are valid (`czdev` preflight: newer than 0.7.0-rev1).
- [x] Listing copy and categories have been reviewed.
- [x] Four clean 320 x 170 baseline screenshots exist (six are submitted).
- [x] No credentials, tokens, or private server details are embedded.
- [x] The user explicitly approved external submission.

Automated: `python3 ~/.claude/skills/cardputer-app-publish/scripts/prepublish_check.py --deb <deb> --app-dir .`
(passed for 0.8.0) and `python3 tools/validate_app_builder.py`.

## Submission Result

- Status: `published` (0.8.0-rev1)
- Submitted at: 2026-11-21T12:58:42Z (`czdev publish`, after syncing the fork `hieuha/packages` with upstream;
  without the sync creating the branch fails with HTTP 404)
- Portal message: none (CLI); `czdev` preflight passed
- Actions URL: <https://github.com/CardputerZero/packages/actions/runs/44906569404> (Deploy APT Repository to
  Pages, success)
- Tracking or pull-request URL: <https://github.com/CardputerZero/packages/pull/617> (merged 2026-11-21T12:59:12Z)

Earlier: 0.6.0-rev1 via <https://github.com/CardputerZero/packages/pull/544>, 0.5.1-rev1 via
<https://github.com/CardputerZero/packages/pull/516>.
