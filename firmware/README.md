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

## Important hardware validation

The Waveshare V2 board has dedicated examples for its display, SD and ES8311 codec. This project keeps all pin assignments in `include/board_pins.h`. Before building a final enclosure, validate:

1. ePaper orientation and busy polarity;
2. microphone I2S channel and gain;
3. SDMMC pins in 1-bit mode;
4. audio power-enable behavior;
5. onboard BOOT short/long presses after normal startup.

The application architecture does not depend on those details, so board-level fixes remain localized.

## Local web portal / transcription server

Open `http://<ESP32 station IPv4>/` on trusted Wi-Fi and enter the existing API
access token. The **Transcription server** section lets you save a bounded server
base URL, for example `http://192.168.1.20:8000` (not the ESP32 IP). Literal IPv4,
HTTP/HTTPS and an optional port are supported; no path, hostname or credentials.
Save while idle or in Settings, not during recording/finalizing/synchronization.
The page waits for NVS completion and exact readback, not just a queued response.
The setting persists in NVS `server/base_url` and is used by both live preview and
final transcription. Missing/invalid settings retain the compile-time default.
No SD card is needed to configure it.

**The server must still use the token compiled into the firmware.** Changing the
address does not change tokens, Wi-Fi credentials or PCM16/WAV formats. HTTP exposes
audio/token in plaintext: do not expose the device or server on the public internet.
HTTPS requires a trusted certificate matching the server IP. Disconnect clears
page data; tokens are not stored in the browser. See [the portal API and checks](tests/WEB.md).

## E-paper refresh

Existing UI screens automatically use the Waveshare V2 partial waveform. The first
frame is a full update that seeds both controller RAM planes. Changed frames then
use partial updates; identical frames cause no SPI traffic. By default, after 10
successful partial updates the next changed frame performs a full cleaning update.
This is full-frame differential refresh, not a rectangular region update.

Open **Settings → Full refresh** using BOOT (long opens/selects, short advances).
The seventh menu item scrolls into view above the footer; Settings works without SD.
In the editor, short presses cycle **1, 5, 10, 20, 50, 100 partial updates**, then
**Full only**. Long saves and returns to Settings. The menu shows the active count
(or `Only`); editor changes are not active until saved. Larger counts reduce full
flashing but may allow more ghosting. `Full only` uses full updates for changed
frames, still skipping identical frames.

The setting is stored in NVS (`display` / `partial_limit`, uint8). Boot loads it after
NVS initialization and before the first display frame. Missing, unreadable, wrong-type
or unsupported values fall back to 10 without writing a replacement. A failed open,
write or commit shows **Save failed** and retains the previous active setting; long
returns to Settings to retry. Changing the interval never resets accumulated partials:
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
then hold to select. After inserting a card, select **Storage** to retry mounting it.
Mounting never formats the card. Recording requires both usable SD storage and audio;
an unavailable codec does not prevent browsing Settings either.

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
