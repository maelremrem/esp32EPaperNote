# Firmware live-preview verification

## Staged boot and cooperative synchronization stop

```sh
python3 firmware/tests/test_boot_wifi.py
python3 firmware/tests/test_api_runtime.py
python3 firmware/tests/test_sync_cancel.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/test_api_runtime.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/test_sync_cancel.py
```

Boot executes real main and real WifiManager with hardware adapters, including
mount-before-result ordering, server Checking-before-health ordering, missing
SD/audio, Wi-Fi association without GOT_IP, fallback/disabled/skipped profiles,
server failure/unconfigured target, and continued menus. ApiClient executes its
actual HTTP implementation with installed cJSON and transport adapters: public
GET /health, bounded timeout/body, exact service/status, lazy model readiness,
malformed/trailing JSON, redirect/error/init/read/timeout failure, atomic runtime
targets, cooperative cancellation before/open/upload/headers/read, EAGAIN retry
and total deadline. Remote bodies/tokens are never returned as diagnostics.

The real-main sync test includes a genuinely separate host HTTP thread: main
receives LongPress while the worker is blocked, retains the cancelled and later
WAVs, preserves committed prefix notes, rejects formatting/recording while joining,
handles cancellation simultaneous with completion and between notes, handles task/
semaphore allocation failure, pauses the real idle auto-retry loop, permits manual
retry and completes a web sync as cancelled. FreeRTOS/HTTP adapters are not actual
radio/socket/storage/e-paper timing evidence. Only the worker owns HTTP cleanup;
installed ESP-IDF cancel_request reconnects the socket and provides no cross-thread
safety guarantee, so it is deliberately not used by main.

Actual framebuffer tests verify empty/half/full ring arcs, clamping, status
truncation/collisions, all per-profile states, Long Cancel and truthful Wait-only
stopping controls. Run the complete renderer (mandatory QR decode included) and
export just owned frames with `--preview-boot` as described in `tests/ui/README.md`.

## FatFs long filenames

Run `python3 firmware/tests/test_fatfs_paths.py` with the PlatformIO ESP-IDF package
installed (or `IDF_PATH` set). This checks both defaults and active sdkconfig, then
executes real IDF FatFs on a disposable RAM FAT32 disk: the production `recording`
directory fails with `FR_INVALID_NAME` in 8.3-only mode; heap LFN permits directories,
long WAV names and `index.jsonl`, preserving data after remount. POSIX-based storage
harnesses alone do not catch this configuration failure. See [SD.md](SD.md).

## Authenticated web Settings

Run `python3 firmware/tests/test_web_settings.py`,
`python3 firmware/tests/run_web_server_tests.py`,
`node firmware/tests/web_settings_test.mjs` and
`node firmware/tests/web_server_settings_test.mjs`.
The real-main harness covers display/NVS failures, worker/state guards, retry,
format TTL/replay/command invalidation/cache clearing and save-before-reconnect.
Real HTTP handlers use installed IDF cJSON with host HTTP/mutex adapters, including
strict bounded settings bodies and the shared mailbox. Real Wi-Fi manager tests
also cover unchanged-password retention, changed-SSID rejection and explicit open.
See [WEB.md](WEB.md) for Chrome fixture checks and API schemas. Run
`python3 firmware/tests/build_public_fixture.py` for a clean scratch build excluding
all private secrets and shared `.pio`, with production hashes and embedded bytes
verified against the current sources. Never flash its public fixture.

## Settings, provisioning and destructive-format safety

```sh
python3 firmware/tests/test_settings_submenus.py
python3 firmware/tests/test_wifi_profiles.py
python3 firmware/tests/test_wifi_provisioning.py
python3 firmware/tests/test_boot_storage_mount.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/test_settings_submenus.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/test_wifi_profiles.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/test_wifi_provisioning.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/test_boot_storage_mount.py
```

The submenu harness executes actual `main.cpp` with hardware adapters: logical
Settings order, offline/real station IP, SSID-only profile information, physical
portal activation/close/timeout, save failure, Cancel-default formatting, worker
joins, queued web-command rejection, failed-format readiness and cache invalidation.
The Wi-Fi profile harness executes actual `WifiManager` with fake NVS/AP transport:
bounded two-slot blobs, defaults, disabled slots, validation, 32-byte SSID preservation,
lost IP, corrupt blobs, open/write/commit failure and repeated AP-netif reuse.
Commit failure is injected even with visible staged NVS data; active profiles must
remain unchanged until a successful commit.

The provisioning harness executes the real HTTP handlers and DNS worker using a
threaded host task/semaphore and simulated socket/HTTP transports. It verifies
actual DNS A response bytes and the complete preserved question, malformed/truncated/
unsupported/oversized packets, multipart receives of a URL-encoded form, random
CSRF, literal Host/Origin restrictions, detection redirect, controls/duplicates,
bounded bodies, mailbox reservation through completion and no HTTP-task NVS writes.
HTTP/socket/bind/task/AP-IP/cleanup failures must not claim portal readiness.
These are not real network socket, captive-popup, WPA radio or physical NVS tests.

Storage tests call the real `NoteStore` through fake IDF format/card-status APIs
and scratch directory fixtures. Normal init is always non-destructive. Formatting
requires a mounted FAT card; success recreates directories, and absent-card,
format/directory/unmount failures make storage unavailable without reusing freed
handles. No real card is formatted or written. Verify physical FAT formatting,
AP/DNS/DHCP/phone behavior and e-paper/button timing on the ESP32 before deployment.

RED → GREEN tracer runs covered missing real-store formatting, missing main submenu/
confirmation APIs, the new portal handlers/form, active-profile retention on failed
commit, 32-byte SSID handling and the physical portal lifetime label.

From the repository root:

```sh
python firmware/tests/run_live_preview_tests.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined' python firmware/tests/run_live_preview_tests.py
```

The pure C++ tests cover real endpoint IDs, unsafe path rejection, even-length raw PCM and
30-second limits, exact little-endian byte preservation, fixed-capacity windows and queue-gap
reset, response ID/status/model/type checks, bounded UTF-8 text accumulation, change-only
2-second display gating, and retention of short WAVs when recording failed. Tests were run
RED (missing helpers), then GREEN during implementation. They do not simulate FreeRTOS,
Wi-Fi, codec, SD, or panel hardware.

## E-paper full/partial transport

```sh
python3 firmware/tests/test_epaper_refresh.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/test_epaper_refresh.py
```

These tests compile the real display driver with GPIO/SPI/time adapters. They cover
vendor LUT bytes and command order, first-frame RAM seeding, changed-frame partial
updates, identical-frame skipping, configurable cleaning (0/full only, 1, 5, 10,
20, 50, 100), default-10 cleaning, budget preservation after changes, forced full,
sleep/wake and reference invalidation after SPI/BUSY failures. They do not measure
physical refresh latency, temperature response or ghosting.

## Persistent refresh interval / real main

```sh
python3 firmware/tests/test_refresh_settings.py
python3 firmware/tests/run_navigation_tests.py
python3 firmware/tests/test_boot_storage.py
python3 firmware/tests/test_boot_storage_mount.py
python3 firmware/tests/test_boot_wifi.py
python3 firmware/tests/test_boot_button.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/test_refresh_settings.py
```

The interval tests execute actual `main.cpp` with NVS/hardware adapters. They cover
BOOT navigation without SD, all seven menu actions, short/double cycle and wrap,
long save/return, reboot loads, all allowed values, missing/invalid/unreadable values,
and open/write/commit failure messages with no phantom active-setting change.
Driver transport, main navigation and actual framebuffer are separate host harnesses;
these do not prove physical NVS durability or panel ghosting.

TDD tracer bullets were exercised RED → GREEN for the missing configurable driver
API, missing main setting navigation, and missing interval UI/scrolling. Additional
regressions retain LUT/reference/recovery, storage, Wi-Fi and button behavior.

## Read-only saved-note history

```sh
python3 firmware/tests/run_note_history_tests.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/run_note_history_tests.py
```

These tests compile the real `note_store.cpp` and use scratch filesystem fixtures,
substituting only ESP hardware services and the SD mount path. They cover bounded
listing, union/deduplication/order, regular-file and safe-ID checks, generated
Markdown round-trips, empty text, embedded separators/metadata, UTF-8 truncation,
unreadable/missing paths and malformed files. Run as a non-root user for the
permission-denied checks. No real SD card is mounted or written.

The UI requests at most 100 entries. Transcript reads use a 16 KiB budget; large
files need the generated metadata footer within the final 1 KiB. Truncated text
is explicitly marked; the source Markdown is never modified by browsing.

## Persistent transcription server / real code tests

```sh
python3 firmware/tests/test_server_url.py
python3 firmware/tests/test_server_settings.py
python3 firmware/tests/test_api_runtime.py
python3 firmware/tests/run_web_server_tests.py
node firmware/tests/web_server_settings_test.mjs
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/test_server_settings.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/test_api_runtime.py
```

TDD slices were exercised RED → GREEN for URL policy, real-main NVS commands,
actual ApiClient live/final runtime targets, real authenticated HTTP routes and
browser unlock/queue/readback. Additional RED → GREEN regressions cover saved
self-address after DHCP, menu sync publishing its busy state, disabled upload
redirects, late JSON after logout and a poll begun before enqueue.

These execute real main, ApiClient and web handlers in separate host harnesses
with NVS/hardware/HTTP adapters, not real network sockets. They cover no-SD/menu
writes, reboot load, missing/invalid/oversized values, open/write/commit failure,
state revalidation, bounded JSON, auth/Host/Origin, and exact URL confirmation.
Sanitizer checks are host-only. Playwright checks real embedded assets in Chrome
at 320/375/414/768/1440px using synthetic API fixtures, not firmware on a device.
See [WEB.md](WEB.md) for the contract and trusted-LAN limitation.

After web asset changes, use a **clean** fixture build and then run
`python3 firmware/tests/test_web_build_assets.py`: PlatformIO can leave stale
embedded assembly on an incremental build despite CMake configure dependencies.
The assertion checks actual generated asset bytes against all current sources.

## Compile without credentials

If the local `firmware/include/secrets.h` is absent, compile using the explicit **public,
nonfunctional** fixture. This does not create a production secrets header:

```sh
PLATFORMIO_BUILD_FLAGS="-I$PWD/firmware/tests/build_fixture" pio run -e waveshare_epaper_154_v2
```

**Never flash the fixture firmware.** Production builds must use the user's real local
configuration. No credentials need to be read to run these checks.

## Runtime behavior / hardware checks still needed

- PCM copied to a 16-chunk queue with zero enqueue wait; full queue drops preview only.
- Dedicated worker sends non-overlapping 4.096-second mono 16 kHz s16le windows. Capture
  during HTTP work can be dropped; the complete local WAV remains authoritative.
- Provisional text is RAM-only and capped at 4096 bytes. After the cap it stops growing;
  full final transcription is still requested from the saved WAV.
- Network operations use a 2-second timeout, cooperative cancellation between operations,
  and an 8-second request budget. An in-flight DNS/TLS/socket operation cannot be interrupted
  by this task; main polls completion without blocking or force-deleting the worker.
- Stop joins recorder first, then worker, before releasing queue/buffers or final sync.
- On SD/header/flush/commit failure the temp WAV is kept and new recordings are blocked
  for that boot to avoid overwriting it. Manual recovery is needed; recovery across reboot
  is outside this change (existing storage behavior).
- Window uses PSRAM if available, then internal heap. Allocation/task failure disables
  preview without disabling WAV capture. Runtime heap headroom, task stack high-water marks,
  offline/slow-server stop, rapid restart, and real button/display timing need device tests.
- Partial refresh does not change windowed inference into word streaming; main calls `showLiveRecording` only on
  changed text/Wi-Fi status, no sooner than 2 seconds after the previous refresh completes.
