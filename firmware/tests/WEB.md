# Carnet: local ESP32 web console

Open `http://<station-IPv4>/` from the same trusted Wi-Fi network. The address is
available in the ESP32 serial log after association (`Got IP`). Use the literal
IPv4 address, not a hostname or reverse proxy: API requests validate Host and
Origin against the device's current station address (optional `:80`). No access
point, mDNS, credential editor or remote control service is introduced.

Enter the existing `VOICE_NOTES_API_TOKEN` in the password field. The page keeps
it only in memory/the open form; it does not use cookies, localStorage or
sessionStorage and never includes a token in assets or status. Maximum supported
length is 248 bytes (Authorization header is bounded to 255 bytes). An empty
configured token fails closed. Status and all commands require Bearer auth.

**HTTP is not encrypted.** Only use this on a trusted private LAN. Do not port
forward port 80 or expose it publicly. Sharing the API token means this interface
uses the same authority as the transcription service; a dedicated local token
and HTTPS can be added if separate access control is needed. No Wi-Fi passwords,
SSID or API token are served. The transcription base URL is available only after authentication.

## Transcription server address

After connecting, edit **Server base URL (protocol, IPv4 and port)** and select
**Save server address**. Example: `http://192.168.1.20:8000`. This is the server,
not the ESP32 address used to open this portal. Only literal IPv4 with `http://`
or `https://` and an optional decimal port (1–65535) is supported. Without a port,
HTTP uses 80 and HTTPS uses 443. No hostname, IPv6, trailing slash/base path,
credentials, query, fragment, whitespace/control character or leading-zero
address/port is accepted. Loopback, unspecified, multicast/reserved, link-local
and the ESP32’s current address are rejected. The URL is bounded to 63 bytes.
HTTPS requires a trusted certificate valid for the server IP; certificate checks
are not disabled. HTTP sends audio and the shared token in plaintext: trusted LAN only.

The server must still accept the existing **firmware-compiled
`VOICE_NOTES_API_TOKEN`**. Changing the URL does not change authentication or edit
Wi-Fi credentials. The same new address is used for both live PCM16 previews and
final WAV uploads. Preview windows remain nonoverlapping 4.096 seconds; audio
formats and backend contracts are unchanged. HTTP redirects are disabled on both
upload paths so the configured target cannot redirect the credential elsewhere.

- GET `/api/config/server`: authenticated, `Cache-Control: no-store`; returns
  `base_url`, `command_id`, `command_result`, `command_busy`, never the token.
- POST same path: `Content-Type: application/json`, exactly one `base_url` string;
  body at most 256 bytes, with no JSON escapes/control characters. Partial or
  malformed bodies return 400, oversized bodies 413. Host/Origin/Bearer protection
  is identical to the other API routes; an authenticated editor can choose another
  unicast IP, so access tokens must only be shared with trusted administrators.
- Changes use the existing one-slot mailbox; HTTP never mutates ApiClient or NVS.
  Allowed in idle or menu, even without SD/audio; recording, finalizing, syncing,
  reconnecting or an occupied mailbox returns 409. Main revalidates the state and
  device-self address after dequeue. Menu-triggered sync also publishes `syncing`.
- 202 is **queued, not saved**. Main stores NVS string `server/base_url`; only after
  successful open/write/commit does it activate and publish the address. Failure
  reports `failed` and retains the prior current address; state changes report
  `rejected`. Config edits run only after recording workers have joined and outside
  synchronization. ApiClient additionally takes mutex-protected immutable URL
  copies per request so string changes cannot race a preview request.
- Boot loads after NVS init, before preview/final requests. Missing, wrong-type,
  unreadable, oversized or invalid saved values use `VOICE_NOTES_API_BASE_URL`
  without rewriting flash. Once Wi-Fi reveals the station IP, an idle/menu check
  rejects a saved endpoint that is now device-self (for example after DHCP changes).
  The compile-time fallback remains the existing trusted build configuration.
- The form loads after unlock, never overwrites edits during status polls, and
  waits for completion then reads back the exact requested URL and matching command
  result before showing “saved”. Failure keeps edits for retry. Another tab replacing
  a result is not success. Polls begun before enqueue cannot cancel the pending save.
  Disconnect or any 401 clears the token, URL and note data; late JSON responses
  from an earlier access session cannot repopulate it. Nothing is browser-persisted.

## Behavior

- English, framework-free HTML/CSS/JS, local assets embedded in flash. `Carnet`
  uses a light monochrome Workbench layout; no CDN, font downloads or animation.
- Wi-Fi/IP, application state, pending WAV count, session's last note and
  provisional live text. The latest note is RAM session state, not SD history
  reloaded after reboot. Text previews are capped at 4096 bytes, at UTF-8 boundaries.
- Record uses the ESP32 microphone. Start is blocked in the menu, recording,
  syncing, reconnecting and SD-recovery states. Stop is offered only during a
  recording and never claims that the WAV has already been finalized.
- Sync is offered only when idle, connected and there are pending WAV files.
  Existing automatic transcription and physical-button controls are unchanged.
- POST `/api/command/{start,stop,sync}` has no request body. 202 means **queued**,
  not executed. A one-slot mutex-protected mailbox stays reserved until main
  completes the command. Duplicate/busy/inapplicable commands return 409.
- Main revalidates every dequeued command against the current application state
  (including a physical button changing it after enqueue). Only main touches the
  recorder, note store and application state. HTTP only reads bounded snapshots
  or reserves the mailbox. Failed/rejected commands are reported in status.
- GET `/api/status` is authenticated; fields include `command_id`,
  `command_result` (`pending`, `ok`, `stopping`, `failed`, `rejected`) and
  `command_busy`. Polling continues during synchronous transcription.
- The browser disables commands on failed requests, handles 401/403/409, preserves
  command errors and reconnects. It handles another tab replacing the last result
  without claiming its own command succeeded. Transcript rendering uses textContent.
- UI focus rings, labelled form, live status, 44px targets, no motion, responsive
  at 320/375/414/768/1440px. The e-paper driver, renderers and refresh waveforms are
  untouched. Main publishes status without repeated SD scans during capture.

## Reproducible checks (repository root)

```sh
python3 firmware/tests/run_web_tests.py
python3 firmware/tests/run_web_server_tests.py
node firmware/tests/web_ui_test.mjs
node firmware/tests/web_server_settings_test.mjs
python3 firmware/tests/test_server_url.py
python3 firmware/tests/test_server_settings.py
python3 firmware/tests/test_api_runtime.py
python3 firmware/tests/run_live_preview_tests.py
python3 tests/ui/render_test.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined' python3 firmware/tests/run_web_tests.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined' python3 firmware/tests/run_web_server_tests.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined' python3 firmware/tests/run_live_preview_tests.py
CXX=clang++ UI_TEST_CXXFLAGS='-fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all' python3 tests/ui/render_test.py
pio run -e waveshare_epaper_154_v2 -t clean
PLATFORMIO_BUILD_FLAGS="-I$PWD/firmware/tests/build_fixture" pio run -e waveshare_epaper_154_v2
python3 firmware/tests/test_web_build_assets.py
```

Host HTTP checks compile the **real** `web_server.cpp` and installed IDF cJSON.
Only FreeRTOS mutex and HTTP transport are replaced with host adapters. They
exercise auth, hostile Host/Origin, bounded headers, served assets, JSON escaping,
queue reservation, command results and concurrent snapshot publication. They do
not simulate actual sockets or the recorder/SD.

Browser checks require Playwright and a local Chrome binary, installed outside
the repository:

```sh
python3 -m venv "$TMPDIR/carnet-browser-venv"
"$TMPDIR/carnet-browser-venv/bin/pip" install playwright
"$TMPDIR/carnet-browser-venv/bin/python" firmware/tests/web_browser_test.py
```

`CHROME` can override `/usr/bin/google-chrome`. The test serves real assets on a
loopback ephemeral port and injects clearly synthetic API fixtures; it checks
mobile overflow, button extents, input contrast, focus, 202/pending, recording,
live text, authorization errors and absence of browser persistence. Screenshots
are written to `$TMPDIR/carnet-esp32-{width}.png`.

The CMake integration uses IDF's own `data_file_embed_asm.cmake` at configure time:
PlatformIO 6.11's SCons path does not execute component `EMBED_FILES` custom
commands. `CMAKE_CONFIGURE_DEPENDS` tracks native CMake inputs, but PlatformIO’s incremental
SCons build can reuse stale assembly even after editing assets. Clean before the
fixture build above; `test_web_build_assets.py` compares all generated assembly bytes
to current source assets. A green incremental compile alone is not sufficient.

**Do not flash the nonfunctional public compile fixture.** No device was flashed
or real credentials read for these checks. Physical Wi-Fi reconnect, real HTTP
requests, recorder/SD timing, simultaneous capture and browsing, HTTP task stack
high-water mark and runtime heap headroom remain hardware checks. Browser fixtures
and host mutex stress are not proof of real-device behavior.
