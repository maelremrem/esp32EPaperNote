# Carnet web interfaces — browser evidence

These screenshots show rendered implementation files, not design mockups. All
note content and device state shown are explicitly synthetic browser-test
fixtures. No real credentials, recordings or deployed service data were used.

The earlier four-image baseline was regenerated from the English HTML/JS with actual Chrome
browser tests. UI labels are English; repeated accented characters are deliberate
UTF-8 wrapping fixtures, not untranslated interface text. `Carnet` is the product
name and is unchanged.

- `esp32-mobile.png`: current Notebook view at 375 px (synthetic SD-unavailable state).
- `esp32-desktop.png`: current Notebook view at 1440 px.
- `esp32-settings-mobile.png`: Settings navigation, Wi-Fi/server/display/storage at 320 px.
- `esp32-settings-desktop.png`: same Settings implementation at 1440 px.
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

## Earlier ESP32 server-configuration baseline

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

## Current ESP32 Settings evidence

Screenshots above were regenerated from real Chrome HTML/CSS/JS rendering with
explicit synthetic API fixtures. The Settings images show the two network editors,
observed IP and saved SSIDs, transcription target, full-refresh selector, and an
injected SD mount error (not a physical diagnosis). Chrome passed 320/375/414/768/1440px
without overflow or clipped visible buttons, with 44px touch targets, keyboard focus,
authentication, poll/edit preservation, immediate password clearing, exact Wi-Fi/
display readback, Cancel-focused format preparation, non-destructive mount retry,
queued recording, inert live text and 401/private-data clearing. Screenshots at
320 and 1440px were visually inspected; no browser storage is used.

Real main, real HTTP/cJSON handlers and real Wi-Fi/NVS adapter tests passed, including
worker/state guards, strict body bounds, commit failures, retained blank passwords,
explicit open networks, format challenge expiry/replay/command invalidation and cache
clearing. New main/HTTP/Wi-Fi and existing server-setting harnesses passed ASan/UBSan.
Existing navigation, physical submenu/portal, refresh, boot storage/Wi-Fi, URL/API,
web policy, live helper and three embedded Node UI suites passed.

A clean secret-excluding scratch fixture build succeeded for
`waveshare_epaper_154_v2`: RAM **69,484 bytes**, flash **1,289,924 bytes**.
All **40** copied production-source hashes and four embedded web asset byte streams
match current repository sources, including current SD diagnostics and warning-icon
changes. Build only: never flash the nonfunctional public fixture. Existing unused
button TAG and codec Kconfig environment warnings remain; PlatformIO also warns
about multiple installed Core versions. Shared repository `.pio` was not cleaned.

## Pending cancellation / station setup / local downloads

Current production browser assets were exercised in Chrome at 320/375/414/768/1440px.
Settings can be drafted before pairing; protected operations require a physically opened
10-minute local access session, never the LXC token. URL/token edits, revision readback,
secret clearing, byte-exact local WAV/Markdown downloads and safe note rendering pass.
Storage usage shown in captures is an explicitly synthetic FAT capacity sample.
Real main/storage/HTTP/ApiClient host tests and the 42-test framebuffer suite pass;
physical cancellation retains WAV audio with no fabricated transcription.
Current Settings captures were copied to `esp32-settings-mobile.png` and
`esp32-settings-desktop.png`; all API data is synthetic.
Earlier firmware build numbers above are historical, not measurements of these changes.

## Six-digit physical access modal

`esp32-pin-modal-mobile.png` (375 px) and `esp32-pin-modal-desktop.png`
(1440 px) show the actual Chrome-rendered access dialog with an empty PIN field.
Notebook and Settings captures were regenerated with the same production assets.
All device/API data remains synthetic; no real PIN or server credential was used.

The permanent access form is replaced by **Connect / unlock settings**. Selecting
Settings while locked opens the same native `<dialog>`; Cancel leaves the public
server URL/token draft available. One password field accepts exactly six ASCII
digits, including leading zeros and clipboard paste. The PIN is RAM-only and sent
only in the Authorization header, never in a URL or browser storage. The dialog
closes after authenticated settings/config/history loading, not merely on submit.
A `401` with `pin_attempts_exhausted` tells the user to reopen web settings physically.
The backend, not the browser fixture, enforces the cumulative five-attempt limit.

Verification: four Node suites pass, including stale-login rejection, PIN
validation, exhaustion messaging, command completion/exact readback and download
contracts. Real Chrome passes at 320/375/414/768/1440 px, including initial PIN focus,
forward/reverse Tab containment, Escape/Cancel/×/backdrop dismissal and trigger
focus restoration, draft preservation, real clipboard paste of a leading-zero
fixture, no horizontal overflow and 44 px visible button targets. Dialog bounds
also pass with a 360 px viewport height (a reduced-height keyboard proxy, not a
real phone keyboard test). Existing Wi-Fi/display/storage, queued recording,
inert text and byte-exact WAV/Markdown download checks still pass. No JavaScript
page errors or localStorage/sessionStorage entries were observed. Both modal
captures were visually inspected. No new firmware build or hardware verification
is claimed by this frontend-only evidence.

## Scope and limitations

Nothing was committed, flashed or deployed. Real ESP32 microphone/SD/Wi-Fi
runtime behavior and the target Proxmox LXC remain deployment checks. The ESP32
uses trusted-LAN HTTP and a physically authorized, expiring local session independent
of the transcription token: do not expose port 80 publicly.
Its latest note is session RAM state, not restored history after a reboot.
Browser tests do not measure real transcription accuracy.

Reproduce via `firmware/tests/WEB.md` and `lxc/README.md`. Screenshots are regenerated
by the corresponding browser tests under `$TMPDIR`, then copied here for review.
