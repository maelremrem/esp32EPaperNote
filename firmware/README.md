# Firmware

PlatformIO project using ESP-IDF.

## Setup

```bash
cp include/secrets.example.h include/secrets.h
nano include/secrets.h
pio run
pio run -t upload
pio device monitor
```

## Hardware button

The firmware uses the onboard **BOOT** button on `GPIO0` (active low); no external button is required. After startup, a short press starts/stops recording, a long press opens Settings, and in menus a short press advances and a long press selects. Holding BOOT during reset still enters the ESP32 download mode: release it and restart normally to use the interface. A button held when the input task starts is ignored until its first release to avoid a phantom recording.

## Staged startup and stopping synchronization

Startup uses the same bold 8×12 header/icons as the notebook, with a clockwise
progress ring and status **below** it. The ring counts four completed stages
(**SD, audio, Wi-Fi, server**), not elapsed time or success percentage. Failed or
skipped stages still complete; there is no timer-driven e-paper animation.
SD initialization is shown before mounting, followed by its actual Ready/Unavailable
result. Home/Hotspot keep separate Pending/Connecting/Connected/Failed/Skipped/Disabled
states. Connected and the displayed IP require `GOT_IP`, not association.

After Wi-Fi, firmware makes one public `GET <active server>/health`, without a token
or redirects, using a 5-second socket timeout and a bounded response. Ready requires
HTTP 200 and JSON `status=ok`, `service=ESP32 Voice Notes STT`. It does not require
`model_loaded=true`: Whistle loads lazily. Ready is service readiness, **not** proof
that the transcription token is accepted. No Wi-Fi or missing URL/token skips the
check. Unavailable/skipped never prevent Settings or local recording when SD/audio
are usable. The configured URL/token are copied atomically per request.

While synchronizing, **hold BOOT to cancel the current batch** (Long → Cancel).
The HTTP worker cooperatively stops; main keeps polling BOOT and shows Stopping sync
until that worker has closed its HTTP handle and WAV and signalled completion.
No worker is force-deleted and main never closes a worker's HTTP handle. SD writes,
archiving, display and application state remain main-owned; recording, downloads,
formatting and configuration are excluded until completion. A stop queued alongside
a result wins before local persistence; already committed earlier notes stay committed.
Cancelled/incomplete audio remains pending and no subsequent note is started.
Automatic retry is paused until an explicit **Sync now** or web Sync starts a new batch.
The web Sync command completes with `cancelled`, not success.

Cancellation cannot undo work already accepted by the server. Network waits use
2-second socket timeouts with retry for slow inference and a 180-second overall
request deadline checked between calls. Cancellation latency is **not guaranteed
at two seconds**: DNS/TLS and ESP-IDF's internal header/body loops can delay return.
The UI remains in Stopping sync, not falsely cancelled, until the worker joins.
Host tests cover these ownership/stop semantics; real Wi-Fi/SD/e-paper timing still
requires the physical device. Previews: [boot and sync states](../docs/screens/new/README.md).

## Important hardware validation

The Waveshare V2 board has dedicated examples for its display, SD and ES8311 codec. This project keeps all pin assignments in `include/board_pins.h`. Before building a final enclosure, validate:

1. ePaper orientation and busy polarity;
2. microphone I2S channel and gain;
3. SDMMC pins in 1-bit mode;
4. audio power-enable behavior;
5. onboard BOOT short/long presses after normal startup.

The application architecture does not depend on those details, so board-level fixes remain localized.

## Settings and Wi-Fi provisioning

Settings is ordered **Notes → Sync → Wi-Fi → Storage → Full refresh → About → Back**.
BOOT short presses advance, long presses select. Information screens return to their
parent submenu with either gesture; no SD card is needed to use Settings.

**Wi-Fi** contains **ESP32 IP**, **Saved networks**, **Start portal**, **Reconnect**,
**Open web settings**, and **Back**, in that order. IP shows the actual station address only when connected,
otherwise Offline; saved networks show SSIDs only, in the Home and Hotspot slots.
These two profiles persist in NVS. Missing/invalid saved configuration uses the
compile-time Home/iPhone defaults. Selecting IP or Saved networks does not reconnect.

Select **Start portal** physically to enable the temporary configuration access point.
Scan the Wi-Fi QR code displayed on the e-paper to join the temporary access point,
or enter its SSID and generated eight-character WPA2 password manually. Passwords
are regenerated at each opening and omit ambiguous I/O/0/1 characters. Then open
`http://192.168.4.1/` if the phone does not open a captive-page notification. Browser
auto-opening depends on the phone and is not guaranteed. Enter the Home/Hotspot
profiles in the setup form. Passwords are not shown in the saved-network screen or
logged. NVS commit must succeed before saved profiles are used. A queued HTTP request
is not a successful save; the device screen reports the completed save/connection.

The portal is never started automatically and lasts at most ten minutes. Either BOOT
gesture closes it, stops AP/DNS/HTTP and resumes station Wi-Fi. Provisioning pauses
the station console and cannot run while recording workers are active. The temporary
AP is for nearby trusted users: its physically displayed password and CSRF/Host/Origin
validation reduce unsolicited provisioning; there is no TLS. NVS credentials are not
encrypted by this firmware, so physical flash access remains a security limitation.

## Authenticated station console / transcription server

Open `http://<ESP32 station IPv4>/` on trusted Wi-Fi. On the ESP32 choose
**Settings → Wi-Fi → Open web settings**, then enter its six-digit PIN in the website's
connection modal. Leading zeros count. After five incorrect attempts, reopen access
on the ESP32 to generate another PIN.
This ten-minute physical session is independent of the transcription/LXC token.
The public Settings shell opens before pairing; private reads and all writes require
local authorization. IP/network loss, server stop, expiry, a new session or Disconnect
revokes access. Choose **Settings** beside **Notebook** to configure **Wi-Fi**,
**Transcription server**, **Display** and **Storage**. Wi-Fi saves the two NVS profiles
without dropping the current connection; blank password keeps the old secret only for
an unchanged SSID, while an open network is explicit. Reconnect is a separate action
and may change the IP. Passwords are never returned. Display uses the same persistent
refresh options as BOOT. Storage shows mount/audio readiness and actual SD errors,
provides non-destructive retry, and formats only after Cancel-default confirmation
with a main-validated single-use 60-second challenge. Workers must be stopped; an
unmountable card still needs external formatting.

**Settings → Transcription server** lets you save a bounded server
base URL, for example `http://192.168.1.20:8000` (not the ESP32 IP). Literal IPv4,
HTTP/HTTPS and an optional port are supported; no path, hostname or credentials.
Save while idle or in Settings, not during recording/finalizing/synchronization.
The page waits for NVS completion and exact readback, not just a queued response.
The setting persists in NVS `server/base_url` and is used by both live preview and
final transcription. Missing/invalid settings retain the compile-time default.
No SD card is needed to configure it.

The server URL and a write-only **New transcription server token** are configurable
at runtime, persisted in NVS and activated atomically only after commit. Blank keeps
the saved token; explicitly Clear removes it, including across reboot. A bare IPv4
uses HTTP port 8080; full origins retain explicit protocol/port. API readback exposes
only token-configured and successful-change revision. The compile token remains a
migration default when no saved token exists, never station-console authorization.
Changing this target does not alter Wi-Fi credentials or PCM16/WAV formats. HTTP exposes
audio/token in plaintext: do not expose the device or server on the public internet.
HTTPS requires a trusted certificate matching the server IP. Disconnect clears
page data; tokens are not stored in the browser. See [the portal API and checks](tests/WEB.md).

## Pending cancellation, capacity and downloads

**Settings → Sync** opens **Sync now**, **Pending notes**, **Cancel all pending**
and **Back**. Select a pending ID or Cancel all, then explicitly confirm the
Back-default warning. Cancellation preserves original audio by archiving it;
collisions/errors leave that source pending. No Markdown or completed transcription
is fabricated. This queued-note cancellation is separate from Long BOOT during an
active sync: the latter stops the batch but retains unfinished WAVs **pending** for
manual retry. Archived audio-only files remain in saved history and downloads.

Storage's physical submenu and status show real FAT used/total with a gauge. Missing,
busy or failed capacity queries say Usage unknown. Station Settings exposes the same
main-owned measurement, not card labels or an invented percentage.

Notebook lists the newest 40 safe SD note IDs with **Download audio** and, for
completed generated notes, **Download Markdown**. Downloads are read-only attachments.
Main opens each validated regular file through a bounded mailbox/lease. HTTP streams
<=2048-byte chunks; recording, sync, cancellation, mount, format and portal launch
are excluded until the FILE closes, including interrupted transfers. The generated
Markdown download is complete, unlike the bounded e-paper reader prefix. Original
card files remain unchanged. Browser Blobs may require memory proportional to file
size; ESP32 WAV buffering stays bounded. See [API/security/verification](tests/WEB.md).

## E-paper refresh

Existing UI screens automatically use the Waveshare V2 partial waveform. The first
frame is a full update that seeds both controller RAM planes. Changed frames then
use partial updates; identical frames cause no SPI traffic. By default, after 10
successful partial updates the next changed frame performs a full cleaning update.
This is full-frame differential refresh, not a rectangular region update.

Open **Settings → Full refresh** using BOOT (long opens/selects, short advances).
The seventh menu item (Back) scrolls into view above the footer; Settings works without SD.
In the editor, short presses cycle **1, 5, 10, 20, 50, 100 partial updates**, then
**Full only**. Long saves and returns to Settings. The menu shows the active count
(or `Only`); editor changes are not active until saved. Larger counts reduce full
flashing but may allow more ghosting. `Full only` uses full updates for changed
frames, still skipping identical frames.

The setting is stored in NVS (`display` / `partial_limit`, uint8). Boot loads it after
NVS initialization and before the first display frame. Missing, unreadable, wrong-type
or unsupported values fall back to 10 without writing a replacement. A failed open,
write or commit shows **Save failed** and retains the previous active setting; long
returns to the editor to retry. Changing the interval never resets accumulated partials:
if the new limit is already reached, the next changed frame cleans the display.
This setting does **not** change the 4.096-second STT windows or live UI update gate.

`refresh(true)` explicitly forces a full update and reports success. SPI errors or
BUSY timeouts invalidate the reference image; the next call attempts reset/full
recovery. Returning from display sleep also requires a full update. The driver uses
the official V2 full/partial LUTs while preserving the existing image orientation.

Host tests validate transport sequences, not physical waveform performance. Check
black-to-white erasure, repeated menu changes, live text, periodic cleaning, and
wake-up on the real panel. Partial refresh does not turn windowed STT into word streaming.

## Storage flow

Missing or unreadable SD storage no longer stops boot or disables menu navigation.
Hold the application button to open Settings, press briefly to move between items,
then hold to select. After inserting a card, select **Storage → Status / retry mount**.
Mounting never formats the card. Recording requires both usable SD storage and audio;
an unavailable codec does not prevent browsing Settings either.

**Storage → Format SD** opens an explicit warning that formatting erases **ALL card
data**, including notes and recordings. **Cancel** is selected initially. Press BOOT
briefly to select **Erase SD**, then hold to confirm; the hold that opened the warning
cannot erase anything. This calls the ESP-IDF FAT format API, not a file-delete loop.
Formatting requires an already mounted FAT card (unformatted/unmountable cards must
be formatted externally). Card presence is rechecked and recording/preview workers
must have stopped. Browser commands are blocked during confirmation. Note directories
are recreated; cached history/transcripts are cleared even if formatting fails.
Failure leaves storage unavailable, never claims success, and does not change audio
readiness. Cleanup failures require restart rather than reuse a freed card handle.

The V2 vendor example uses 1-bit SDMMC with CLK=GPIO39, CMD=GPIO41 and D0=GPIO40,
matching this project's pin map. Use a FAT32 card. A successful host test is not
proof that a physical card is mounted: if retry fails, capture `SD mount failed`
from the serial monitor before diagnosing wiring, filesystem or card compatibility.

Host regressions:

```bash
python3 firmware/tests/test_boot_storage.py
python3 firmware/tests/test_boot_storage_mount.py
python3 firmware/tests/run_navigation_tests.py
python3 firmware/tests/run_note_history_tests.py
```

```text
recording/current.tmp
       ↓ stop
pending/<note-id>.wav
       ↓ successful API response
notes/<note-id>.md
archive/<note-id>.wav
```

The pending WAV is never removed before the Markdown note has been safely written.
