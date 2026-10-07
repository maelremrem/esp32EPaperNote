# Carnet web interfaces — browser evidence

These screenshots show rendered implementation files, not design mockups. All
note content and device state shown are explicitly synthetic browser-test
fixtures. No real credentials, recordings or deployed service data were used.

The earlier four-image baseline was regenerated from the English HTML/JS with actual Chrome
browser tests. UI labels are English; repeated accented characters are deliberate
UTF-8 wrapping fixtures, not untranslated interface text. `Carnet` is the product
name and is unchanged.

- `esp32-mobile.png`: current embedded device console with server configuration at 375 px.
- `esp32-desktop.png`: current embedded device console with server configuration at 1440 px.
- `lxc-desktop.png`: library with selected transcript at 1440 px.
- `lxc-mobile.png`: library at 320 px, including long-word/XSS fixtures.
- `lxc-locked.png`: private-library access screen at 1440 px.

## Verified

English-localization checks rerun successfully: embedded web policy/HTTP-handler
host tests, live-preview helper tests, the embedded Node UI test, all three LXC
Node UI tests, and 52 Python API/deployment tests (2 real-inference opt-ins skipped).
Chrome checks passed at 320/375/414/768 px on ESP32 and additionally 1440 px on LXC,
including authentication, queued commands, inert transcript text and layout bounds.
The firmware size/build figures below describe the earlier baseline, not a new
compile or hardware run for this documentation and web-label change.

- ESP32 policies, real HTTP handlers with host transport adapters, browser state
  and live-preview helpers passed. Existing e-paper render suite: 13 passed.
- Firmware compiled successfully for `waveshare_epaper_154_v2` using only public
  nonfunctional build credentials. RAM: 62,804 bytes; flash: 1,207,348 bytes.
  Do not flash this public-fixture binary.
- LXC Python suite: 52 passed, 2 real-inference opt-in tests skipped. Node UI
  contracts: 3 passed.
- LXC Chrome against a real uvicorn process and disposable SQLite database:
  correct/incorrect token, empty archive, note selection, inert HTML text,
  authenticated WAV decoding, transcript export, search, status filter, missing
  audio, lock clearing and keyboard focus passed. No page JavaScript errors.
- No overflow or clipped visible buttons at 320, 375, 414 and 768 px on both
  interfaces; LXC also checked at 1440 px. Screenshots visually inspected.
- No browser localStorage/sessionStorage token persistence.

## Current ESP32 server-configuration evidence

The ESP32 mobile image was refreshed and desktop image added from the actual
Carnet HTML/CSS/JS after configuring a synthetic `http://192.168.1.21:8001` target.
LXC images and their earlier evidence above are unchanged. Chrome passed at
320/375/414/768/1440px with no overflow/clipped controls, including authenticated
config load, preservation of edits, queued save, NVS-failure UI, completed exact
readback and 401 clearing. Synthetic API fixtures do not prove real NVS or HTTP.

Real firmware handlers/main/ApiClient host tests and their ASan/UBSan checks passed;
existing navigation/storage/Wi-Fi/button/live-preview/e-paper tests also passed,
including 26 actual-framebuffer UI checks. A **clean** public-fixture PlatformIO
build succeeded: RAM **68,156 bytes**, flash **1,238,500 bytes**. All four generated
embedded asset byte streams match their current source files. Incremental builds
can otherwise retain stale web data; use the clean-build verification in `WEB.md`.

## Scope and limitations

Nothing was committed, flashed or deployed. Real ESP32 microphone/SD/Wi-Fi
runtime behavior and the target Proxmox LXC remain deployment checks. The ESP32
uses trusted-LAN HTTP and the existing API token: do not expose port 80 publicly.
Its latest note is session RAM state, not restored history after a reboot.
Browser tests do not measure real transcription accuracy.

Reproduce via `firmware/tests/WEB.md` and `lxc/README.md`. Screenshots are regenerated
by the corresponding browser tests under `$TMPDIR`, then copied here for review.
