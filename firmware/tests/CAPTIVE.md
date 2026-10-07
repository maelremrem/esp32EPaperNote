# Captive Wi-Fi setup

The physical Wi-Fi menu opens a ten-minute, WPA2-protected setup AP at
`192.168.4.1`. The station console stays paused. The eight-character AP password
and separate physical Wi-Fi QR are unchanged. The browser uses only inline
assets protected by a fresh random session nonce; no internet assets are needed.

## Main-loop integration (required)

Call `portal.pollScan()` from the existing main-owned `pollPortal()` routine,
after its inactive/expiry checks and **before** `takeProfiles()` can return early:

```cpp
// Main task only; never call this from an HTTP handler.
portal.pollScan();
network::WifiProfiles profiles;
if (!portal.takeProfiles(profiles)) return;
```

`pollScan()` starts or polls a native asynchronous `esp_wifi_scan_start(...,
false)` through `WifiManager`. It never associates the station, calls reconnect,
or writes NVS. All portal start/stop and profile commits remain main-owned.
`WifiManager::beginPortalScan()` and `pollPortalScan(results,error)` are also
main-only APIs; normal production callers should use the portal wrapper.
No new production translation unit or CMake change is needed.

The radio uses **AP+STA**, not AP-only, because ESP-IDF requires station capability
for scanning. STA is scan-only during provisioning: `connectPreferred()` refuses
and station IP events are ignored until the portal ends. A generation-tagged FIFO
event-loop barrier drains queued old scan events after synchronous radio stop,
including timeout cancellation, before a subsequent scan is armed. HTTP never
holds the mailbox mutex while calling the manager. Stop invalidates scan state,
clears driver results, and leaves no scanner worker or auto-restart resources.

## Browser and API contract

- Two slots, Home first and optional Hotspot second, each have a nearby-network
  selector and an always-available manual SSID field for hidden networks.
- Scan results come only from the IDF radio. Extract at most the 20 strongest AP
  records reported by IDF, deduplicate SSIDs keeping strongest signal, and sort
  by RSSI then SSID. Multiple BSSIDs can therefore yield fewer than 20 names.
  Preserve complete 32-byte names. Skip empty/hidden names, control bytes,
  malformed UTF-8 and channels outside the ESP32-S3 2.4 GHz range.
- Show actual RSSI in dBm and native authentication mode. Unsupported enterprise,
  WEP/WAPI/OWE modes are not silently described as ordinary open networks.
  Authentication labels do not guarantee a successful connection.
- Scanning can temporarily increase AP/page latency because one radio serves
  both jobs. Queue/radio deadlines are bounded at 15 seconds; browser retry
  control recovers after errors, and completed server results expire in 30 seconds.
- `POST /scan`: exact URLencoded `csrf=<session token>` body, literal AP Host,
  same-origin Origin. Returns 202 when queued, not when scanning has completed.
  Concurrent scan or pending-save requests receive 409; body limit is 64 bytes.
- `GET /scan/results`: literal AP Host and `X-CSRF-Token` header; foreign Origins
  rejected. Returns `{state,error,networks:[{ssid,rssi,channel,authmode}]}`.
  Credentials and CSRF never appear in URLs, scan responses, storage or logs.
- Scanned names use DOM `createElement`/`textContent`, never unsafe HTML.
  Rescanning preserves names/password drafts and never automatically selects a
  result. A deliberate new selector choice clears that slot's password/open flag.
- Manual form submission also works without JavaScript. The existing main-owned
  profile mailbox and `takeProfiles()` contract are unchanged: saves replace
  **both** profiles. Re-enter passwords; blank passwords are not secret-preserving
  edits in this portal. Explicit `ho=1` / `so=1` permits an empty password for a
  named open network. An unused slot has empty SSID and password. Stored passwords
  are never populated into this page. Browser and server reject accidental open
  profiles; main commits NVS and checks workers as before.
- Save acknowledgement is queued, not a claim of NVS/association completion;
  the existing response tells the user to check the physical screen.

This is local HTTP over a temporary protected AP, not HTTPS. Anyone with the AP
password can reach the initial form. Physical close/timeout remains the boundary.

## Verification

```sh
python firmware/tests/test_wifi_profiles.py
python firmware/tests/test_wifi_provisioning.py
python firmware/tests/test_wifi_scan.py
python firmware/tests/test_boot_wifi.py

CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  python firmware/tests/test_wifi_profiles.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  python firmware/tests/test_wifi_provisioning.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  python firmware/tests/test_wifi_scan.py

# Use a scratch Playwright venv with Chromium installed (no firmware dependency).
/path/to/playwright-venv/bin/python firmware/tests/captive_browser_test.py
CAPTIVE_SCREENSHOTS=1 /path/to/playwright-venv/bin/python \
  firmware/tests/captive_browser_test.py
```

Host suites compile the actual manager and HTTP/DNS handlers with adapters for
IDF events/radio/NVS and HTTP/RTOS transports. Scan tests cover nonblocking start,
actual error codes, failed result reads, empty results, bounded extraction,
strongest deduplication, malformed/UTF-8/max-length SSIDs, signal ordering,
timeout, cache expiry, main mailbox, ignored late events/event barriers, and no
station association. They are **not RF or real ESP32 socket verification**.

The browser test obtains its HTML **and CSP header from the actual C++ handler**,
serves it on a disposable loopback HTTP server, and uses a clearly test-only
scan API fixture. Chromium exercises 320/375/1000 px layouts, signal/security
selectors, loading/retry/error states, draft preservation, password correction
after validation failure, explicit-open submission, hidden manual SSID/no-JS,
UTF-8 byte limits, hostile SSID text, hidden CSRF, no credential URLs and no root
or button overflow. `docs/screens/web/captive-{320,375,1000}.png` are genuine
Chromium renders with synthetic fixture names/passwords, not hardware scan proof.

Still requires device verification: phone captive detection, actual nearby RF
results, scan/AP latency, scan cancellation/event ordering on the target IDF,
station association after close, and the separate physical-screen QR.
Do not flash a public build fixture.
