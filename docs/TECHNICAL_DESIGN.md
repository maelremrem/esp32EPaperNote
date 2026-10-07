# ESP32-S3 ePaper Voice Notes — Technical design document

Version: 0.1
Date: September 15, 2026
Target: Waveshare ESP32-S3-ePaper-1.54 V2 + self-hosted Whisper server in LXC

> Historical design baseline: this document preserves the original proposal,
> including Whisper-era APIs, task splits and starter limitations. For the current
> Whistle implementation, windowed previews and web consoles, use the root README,
> `firmware/tests/WEB.md` and `lxc/README.md`. English documentation and interface
> labels do not change the default French speech-recognition language (`fr`).

---

## 1. Project overview

This project is a minimalist portable voice-note device based on the **Waveshare ESP32-S3-ePaper-1.54 V2**.

The device does not transcribe locally. Its responsibilities are to:

1. record an audio note when the user presses the single interface button;
2. immediately save the audio to microSD/TF;
3. work without a network;
4. automatically connect to the best known Wi-Fi network;
5. send pending notes to a Whisper server hosted in LXC;
6. retrieve the transcript;
7. store the text note on the SD card;
8. display the result on ePaper.

The fundamental principle is **offline-first**: no note may be lost when Wi-Fi, the server or the Internet is unavailable.

---

## 2. Goals

### 2.1 Main goals

- One-gesture voice recording.
- Fully offline recording.
- Automatic deferred synchronization.
- French transcription through Whisper on a local server.
- Local transcript storage on SD.
- A simple interface suited to ePaper.
- A single application button.
- Home Wi-Fi takes priority over the iPhone hotspot.
- Low standby power consumption.
- Resilience to Wi-Fi interruptions and restarts.

### 2.2 Initially out of scope

- Whisper STT directly on the ESP32-S3.
- Full text editing on the device.
- Bluetooth keyboard in the first version.
- Third-party cloud synchronization.
- Touch interface.
- Audio playback/TTS in the first version.

---

## 3. Target hardware

### 3.1 Main board

**Waveshare ESP32-S3-ePaper-1.54 V2**.

Relevant official specifications:

| Component | Specification |
|---|---|
| MCU | ESP32-S3-PICO-1-N8R8 |
| CPU | Dual-core Xtensa LX7, up to 240 MHz |
| Flash | 8 MB |
| PSRAM | 8 MB |
| Internal SRAM | 512 KB |
| Wi-Fi | 2.4 GHz 802.11 b/g/n |
| Bluetooth | Bluetooth 5 LE |
| Display | 1.54" ePaper |
| Actual resolution | **200 × 200 px** |
| Audio | Codec ES8311 |
| Audio input | Onboard microphone |
| Storage | TF/microSD card slot |
| RTC | PCF85063 |
| Sensor | SHTC3 temperature/humidity |
| USB | Native ESP32-S3 USB-C |
| Battery | Li-ion connector + charging management |

> Important: these UI mockups were designed as square references. The actual panel is **200 × 200 px**. The `screens/native_200x200/` versions are reduced references only; the final interface must be redrawn natively at 200 × 200, not merely resized. These historical raster assets retain their original French labels.

### 3.2 Hardware revision

The firmware must explicitly target **V2**.

Waveshare documentation states that:

- V1 uses an ESP32-S3FH4R2 with 4 MB Flash and 2 MB PSRAM;
- V2 uses an ESP32-S3-PICO-1-N8R8 with 8 MB Flash and 8 MB PSRAM;
- V1 and V2 examples are not interchangeable.

The code must isolate hardware definitions in a dedicated file, for example:

```text
src/board/waveshare_epaper_154_v2.h
```

### 3.3 Single button

The software design assumes **one application button**.

The PWR button remains reserved for power management.

Recommendation: use an external button on a free GPIO, such as GPIO1, GPIO2 or GPIO3, if the enclosure allows it.

BOOT/GPIO0 can technically be read after startup, but it is a strapping pin used for bootloader mode. Prefer a free GPIO for the main button when available.

---

## 4. Overall architecture

```text
                ┌──────────────────────────┐
                │ Waveshare ESP32-S3       │
                │ ePaper 1.54 V2           │
                │                          │
Button ────────►│ State machine            │
Mic + ES8311   ►│ Recorder                 │
                │ SD queue                 │
                │ Wi-Fi manager            │
                │ Sync client              │
                │ ePaper UI                │
                └────────────┬─────────────┘
                             │ HTTPS
                  ┌──────────┴──────────┐
                  │                     │
             Home Wi-Fi            iPhone hotspot
             priority 100          priority 50
                  │                     │
                  └──────────┬──────────┘
                             │
                             ▼
                    Reverse proxy HTTPS
                             │
                             ▼
                    ┌─────────────────┐
                    │ LXC Notes/STT   │
                    │                 │
                    │ REST API        │
                    │ Job queue       │
                    │ Whisper         │
                    │ SQLite          │
                    └─────────────────┘
```

---

## 5. ESP32 firmware architecture

### 5.1 Recommended framework

**PlatformIO + ESP-IDF**.

Reasons:

- better FreeRTOS task control;
- native I2S API for ES8311;
- fine-grained Wi-Fi control;
- SDMMC ;
- deep sleep ;
- watchdog ;
- NVS storage;
- better component separation than a monolithic sketch.

Proposed structure:

```text
firmware/
├── platformio.ini
├── sdkconfig.defaults
├── partitions.csv
├── include/
│   └── config.h
└── src/
    ├── main.cpp
    ├── app/
    │   ├── app_state.cpp
    │   ├── button.cpp
    │   └── events.cpp
    ├── audio/
    │   ├── audio_capture.cpp
    │   └── wav_writer.cpp
    ├── display/
    │   ├── epaper.cpp
    │   ├── ui.cpp
    │   └── icons.cpp
    ├── storage/
    │   ├── sd_store.cpp
    │   └── note_queue.cpp
    ├── network/
    │   ├── wifi_manager.cpp
    │   └── api_client.cpp
    ├── power/
    │   └── power_manager.cpp
    └── board/
        └── waveshare_epaper_154_v2.h
```

---

## 6. State machine

Main states:

```text
BOOT
  │
  ▼
IDLE
  │ click / double-click depending on context
  ▼
RECORDING
  │ click
  ▼
SAVING
  │
  ▼
PENDING_SYNC
  │ network available
  ▼
UPLOADING
  │
  ▼
WAITING_TRANSCRIPTION
  │ result received
  ▼
STORE_TRANSCRIPT
  │
  └──────────────► IDLE
```

Secondary states:

```text
MENU
OFFLINE
SYNC_ERROR
SD_ERROR
SERVER_ERROR
LOW_BATTERY
```

### 6.1 Important rule

Stopping a recording must always write the audio file to SD **before** any network attempt.

Wi-Fi is never part of the critical path for creating a note.

---

## 7. Single-button interaction

Behavior must remain predictable.

### 7.1 IDLE state

| Action | Function |
|---|---|
| Single click | Next note / next screen |
| Double click | Start a new note |
| Long press | Open menu |

### 7.2 RECORDING state

| Action | Function |
|---|---|
| Single click | Stop and save |
| Long press | Cancel recording, optional |

V1 may omit cancellation to reduce the risk of accidental deletion.

### 7.3 MENU state

| Action | Function |
|---|---|
| Single click | Next item |
| Long press | Confirm selected item |

### 7.4 Software detection

Recommended starting values:

```text
Debounce       : 35 ms
Double-click   : <= 350 ms
Long press     : >= 800 ms
Very long press: >= 2500 ms, reserved
```

---

## 8. ePaper interface

### 8.1 Visual principles

- monochrome ;
- white background;
- thin black lines;
- bitmap/monospace typography;
- very few frames;
- generous negative space;
- simple icons;
- hierarchical information;
- no fake touch buttons;
- one bottom help line tied to the physical button.

### 8.2 ePaper constraints

ePaper must not be treated as a real-time LCD.

Avoid:

- high-frequency animated waveforms;
- a timer updated every second using full refresh;
- animations ;
- frequent flashing.

During recording, the mockup waveform is a **conceptual visual indicator**.

Recommended implementation:

- show the RECORDING screen once at recording start;
- update the timer slowly only if the driver correctly supports partial refresh;
- otherwise update the screen only when recording ends;
- periodically perform a full refresh to remove ghosting.

### 8.3 Included screens

#### 01 — IDLE / NOTE

Reference:

```text
screens/reference/01_idle_note.png
```

Contents:

- Wi-Fi status;
- SD ;
- battery;
- date/time;
- note number;
- audio duration;
- transcription ;
- synchronization status.

#### 02 — RECORDING

```text
screens/reference/02_recording.png
```

Contents:

- microphone;
- REC state;
- duration;
- illustrative waveform;
- `1x stop` reminder.

#### 03 — OFFLINE QUEUE

```text
screens/reference/03_offline_queue.png
```

Contents:

- no network;
- number of pending notes;
- latest note;
- option to force synchronization.

#### 04 — SYNCING

```text
screens/reference/04_syncing.png
```

Contents:

- overall progress;
- note currently being uploaded;
- Whisper indicator.

#### 05 — MENU

```text
screens/reference/05_menu.png
```

Initial menu:

```text
New note
Force sync
History
Network
```

---

## 9. Audio recording

### 9.1 Recommended format

For Whisper:

```text
Container       WAV
Codec           PCM signed 16-bit LE
Channels        Mono
Sample rate     16 kHz
Bitrate         256 kbit/s
```

Storage calculation:

```text
16000 samples/s × 2 bytes = 32000 bytes/s
≈ 1.92 MB/minute
```

A 1 GB SD card can already hold several hours of raw notes.

### 9.2 Temporary file

During recording:

```text
/audio/recording/current.tmp
```

When stopping:

1. finalize the WAV header;
2. `fsync` ;
3. close the file;
4. atomically rename to `/audio/pending/<id>.wav`.

Example:

```text
/audio/pending/20260915T220104Z-0043.wav
```

This sequence prevents a battery interruption from producing a note incorrectly treated as valid.

---

## 10. Note identifiers

Each note has a stable identifier:

```text
YYYYMMDDTHHMMSSZ-NNNN
```

Example:

```text
20260915T220104Z-0043
```

This identifier is used for:

- the WAV file;
- metadata;
- the API;
- idempotence;
- the final Markdown filename.

---

## 11. SD card layout

```text
/
├── audio/
│   ├── pending/
│   ├── uploading/
│   ├── archive/
│   └── recording/
├── notes/
│   ├── 2026/
│   │   └── 09/
│   └── index.jsonl
├── queue/
│   └── queue.jsonl
├── config/
│   └── device.json
└── logs/
    └── latest.log
```

### 11.1 Audio policy

Default option:

- retain the WAV until the transcript is received;
- after success, move the WAV to `/audio/archive/`;
- an option may automatically delete archives older than N days.

Never delete the WAV before the Markdown file has been written and synchronized to SD.

---

## 12. Local note format

Example:

```markdown
# Note 0043

Project meeting tomorrow morning.

---

- ID: 20260915T220104Z-0043
- Recorded: 2026-09-15T22:01:04+02:00
- Duration: 11.2 s
- Language: fr
- STT: whisper
- Status: synced
```

Filename:

```text
/notes/2026/09/20260915T220104Z-0043.md
```

---

## 13. Local index

To avoid scanning every Markdown file at startup:

```json
{"id":"20260915T220104Z-0043","file":"/notes/2026/09/20260915T220104Z-0043.md","created":1789502464,"status":"synced"}
```

One JSON object per line in:

```text
/notes/index.jsonl
```

JSONL enables simple appends and reduces the corruption risk of a large monolithic JSON file.

---

## 14. Wi-Fi management

### 14.1 Configured networks

Example:

```cpp
struct WifiProfile {
    const char* ssid;
    const char* password;
    uint8_t priority;
};

WifiProfile profiles[] = {
    {"Maison", "...", 100},
    {"iPhone-Mael", "...", 50},
};
```

### 14.2 Priority

Order:

```text
1. Home Wi-Fi
2. iPhone hotspot
3. Offline
```

### 14.3 Switching rules

- never change networks during recording;
- never interrupt an active upload solely to change SSID;
- after a network operation completes, migrate to a higher-priority network if available;
- when offline, scan periodically with backoff.

Example:

```text
5 s → 15 s → 30 s → 60 s → 5 min
```

### 14.4 iPhone hotspot

The ESP32-S3 uses **2.4 GHz** Wi-Fi.

On recent iPhones, **Maximize Compatibility** disables 5 GHz/6 GHz for the hotspot and forces 2.4 GHz with WPA2, which may be needed for a reliable ESP32 connection.

Recommended iPhone settings:

```text
Settings
→ Personal Hotspot
→ Maximize Compatibility: enabled
```

### 14.5 iPhone VPN

Do not assume that Wi-Fi clients connected to an iPhone hotspot automatically use the iPhone's active VPN tunnel.

Two architectures are possible:

#### A. Fully private API

```text
ESP → hotspot iPhone → VPN → LAN → LXC
```

Use only after verifying real routing with the chosen VPN.

#### B. Internet-accessible HTTPS API — recommended for mobile use

```text
ESP → hotspot iPhone → Internet → HTTPS → Nginx Proxy Manager → LXC
```

The API is protected by a token, TLS, rate limiting and a size limit.

This option operates independently of iOS VPN/tethering behavior.

---

## 15. Offline-first synchronization

### 15.1 Algorithm

```text
if recording:
    do nothing network-related

if network available:
    load oldest pending note
    upload note
    wait/poll transcription job
    validate response
    write Markdown
    fsync
    mark note synced
    archive audio
    process next pending note
```

### 15.2 FIFO queue

Notes are synchronized in chronological order.

```text
oldest first
```

### 15.3 Retry

The following errors are retryable:

- DNS ;
- timeout ;
- connection refused;
- HTTP 429 ;
- HTTP 500/502/503/504.

Example backoff:

```text
10 s
30 s
1 min
5 min
15 min
30 min
```

HTTP 400/401/403 must be shown as configuration errors and must not be retried aggressively.

---

## 16. Proposed server API

### 16.1 Create a job

```http
POST /api/v1/notes
Authorization: Bearer <token>
Idempotency-Key: 20260915T220104Z-0043
Content-Type: multipart/form-data
```

Form fields:

```text
id
recorded_at
duration_ms
language=fr
device_id
file=<wav>
```

Response:

```json
{
  "id": "20260915T220104Z-0043",
  "status": "queued"
}
```

HTTP :

```text
202 Accepted
```

### 16.2 Read status

```http
GET /api/v1/notes/20260915T220104Z-0043
Authorization: Bearer <token>
```

During processing:

```json
{
  "id": "20260915T220104Z-0043",
  "status": "transcribing"
}
```

Completed:

```json
{
  "id": "20260915T220104Z-0043",
  "status": "done",
  "language": "fr",
  "duration": 11.2,
  "text": "Project meeting tomorrow morning."
}
```

### 16.3 Idempotence

If the ESP resends the same note after a network interruption, the server must not create a second transcript.

The key is:

```text
Idempotency-Key = note_id
```

---

## 17. LXC architecture

Recommended stack:

```text
Debian LXC
├── Nginx or Nginx Proxy Manager in front
├── notes-api
│   ├── FastAPI
│   ├── SQLite
│   └── queue worker
└── whisper.cpp
```

Alternative: `faster-whisper` can replace `whisper.cpp` if it performs better on the host machine.

### 17.1 Server directory layout

```text
/opt/voice-notes/
├── app/
├── models/
├── data/
│   ├── jobs/
│   ├── audio/
│   └── transcriptions.db
└── config/
    └── config.env
```

### 17.2 SQLite

Minimum table:

```sql
CREATE TABLE jobs (
    id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    created_at TEXT NOT NULL,
    status TEXT NOT NULL,
    audio_path TEXT,
    transcript TEXT,
    language TEXT,
    error TEXT
);
```

---

## 18. Security

### 18.1 Transport

Always use:

```text
HTTPS
```

except for the earliest strictly LAN-only tests.

### 18.2 Authentication

V1: a randomly generated bearer token of at least 32 bytes.

```http
Authorization: Bearer <device-token>
```

Store the token in NVS or a configuration partition, not in logs.

### 18.3 Server

Apply:

- upload limit, for example 20 MB;
- rate limit ;
- timeout ;
- strict MIME validation;
- server-side WAV validation;
- no execution of client-supplied filenames;
- token-free logging;
- no direct Internet exposure of the Whisper service.

---

## 19. Time management

Sources in priority order:

```text
1. RTC PCF85063
2. NTP when Wi-Fi is available
3. time returned by the server
```

At startup:

- read the RTC;
- synchronize NTP if a network is available;
- correct the RTC if needed.

Internal storage must use ISO 8601.

Example:

```text
2026-09-15T22:01:04+02:00
```

---

## 20. Battery and power

### 20.1 Strategy

Outside recording or synchronization:

```text
render ePaper
flush state
Wi-Fi off
deep sleep
```

Possible wake sources:

- button;
- RTC timer;
- scheduled synchronization event.

### 20.2 Periodic synchronization

If notes are pending:

```text
wake every 5 min
→ test Wi-Fi
→ sync if available
→ sleep
```

If no notes are pending, no periodic network wakeup is needed.

---

## 21. ePaper rendering strategy

Generate the interface using primitives rather than full bitmap screenshots.

Examples:

```text
font rendering
icons 1-bit
horizontal lines
progress bar
text wrapping
```

Reasons:

- much lower Flash usage;
- dynamic text;
- better rendering at 200 × 200;
- future localization;
- less RAM;
- no dependency on rasterized mockups.

The PNGs in this directory are visual references only.

---

## 22. User configuration

Logical configuration file:

```json
{
  "device_name": "voice-note-01",
  "language": "fr",
  "api_url": "https://notes.example.net",
  "audio_archive_days": 30,
  "wifi": [
    {"ssid": "Maison", "priority": 100},
    {"ssid": "iPhone-Mael", "priority": 50}
  ]
}
```

Avoid writing Wi-Fi passwords and tokens in plaintext on SD where possible.

Recommendation: eventually use encrypted NVS storage.

---

## 23. Logging

Levels:

```text
ERROR
WARN
INFO
DEBUG
```

Use USB serial during development.

In production, tightly limit SD writes to:

- preserve the card;
- reduce power consumption;
- avoid corruption risks.

`/logs/latest.log` may be a circular file with a maximum size, for example 128 KB.

---

## 24. Error handling

### Missing SD

```text
SD ERROR
Insert SD card
```

Recording is refused when no safe storage is available.

### Missing Wi-Fi

No functional problem.

The note remains `pending`.

### Server unavailable

The note remains `pending`.

### Transcription failed

The note remains available with its WAV.

Status:

```text
transcription_error
```

### Restart during upload

On reboot:

- move all `/audio/uploading/` entries to `/audio/pending/`;
- resume using the idempotency key.

### Interruption during Markdown writing

First write:

```text
<id>.md.tmp
```

then atomically rename to:

```text
<id>.md
```

---

## 25. Persistent state

The firmware must be able to restart at any point without losing context.

Minimum persistent state:

```text
last_note_id
current_queue_count
last_sync_at
wifi_failure_count
selected_ui_note
```

Critical content remains on SD so the card is the source of truth.

---

## 26. OTA

Plan for OTA from the beginning, even if disabled in the MVP.

Recommendation:

- dual OTA partitions ;
- eventually signed firmware;
- update only with sufficient battery;
- never start OTA while recording or synchronizing a note.

---

## 27. Proposed FreeRTOS task split

```text
ui_task
audio_task
storage_task
network_task
sync_task
power_task
```

### Conceptual priorities

```text
audio_task   high
storage_task high
ui_task      medium
network_task medium
sync_task    low
power_task   low
```

During RECORDING:

- audio and SD writing take priority;
- Wi-Fi may be disabled or left inactive;
- heavy ePaper refreshes must not cause audio underruns.

---

## 28. Concurrency and audio buffers

Use a double-buffer or ring-buffer architecture:

```text
ES8311/I2S
   ↓
DMA
   ↓
ring buffer
   ↓
SD writer
```

The network never reads directly from the recording buffer.

It operates only on an already closed WAV file.

---

## 29. MVP

The MVP is complete when:

- [ ] a click/double-click starts a note;
- [ ] the ES8311 microphone produces a valid 16 kHz mono WAV;
- [ ] the file is saved to SD;
- [ ] the device works without Wi-Fi;
- [ ] pending notes survive reboot;
- [ ] home Wi-Fi has priority;
- [ ] the iPhone hotspot works;
- [ ] the note is sent to LXC;
- [ ] Whisper returns French;
- [ ] final Markdown is written to SD;
- [ ] the UI displays the transcript;
- [ ] the WAV is not lost when synchronization fails.

---

## 30. Development phases

### Phase 1 — Hardware bring-up

- V2 board startup;
- ePaper ;
- SD ;
- button;
- RTC ;
- battery;
- ES8311 and microphone.

### Phase 2 — Audio

- I2S capture;
- WAV ;
- PC validation;
- reliable recordings of 1 to 10 minutes.

### Phase 3 — Storage

- IDs ;
- queue ;
- index ;
- reboot recovery.

### Phase 4 — UI

- IDLE ;
- RECORDING ;
- OFFLINE ;
- SYNCING ;
- MENU.

### Phase 5 — Wi-Fi

- multi-profile ;
- priority;
- iPhone hotspot;
- backoff.

### Phase 6 — LXC

- FastAPI ;
- authentication;
- job storage;
- whisper.cpp ;
- asynchronous API.

### Phase 7 — Sync

- upload ;
- poll ;
- retry ;
- idempotence ;
- final Markdown.

### Phase 8 — Power

- Wi-Fi off ;
- deep sleep ;
- button wakeup;
- periodic synchronization.

### Phase 9 — Robustness

- battery interruption tests;
- full SD;
- server unavailable;
- unstable Wi-Fi;
- long audio;
- OTA.

---

## 31. Validation tests

### Test A — Offline

1. disable all Wi-Fi;
2. record 5 notes;
3. restart the ESP;
4. verify that all 5 WAV files are present;
5. reconnect Wi-Fi;
6. verify transcription of all 5 notes.

### Test B — Network interruption during upload

1. start synchronization;
2. disable Wi-Fi during upload;
3. restore Wi-Fi;
4. verify that only one note exists on the server;
5. verify that the local WAV was not deleted prematurely.

### Test C — Power interruption during recording

1. record;
2. cut power;
3. reboot ;
4. verify that the `.tmp` file is not treated as a final note.

### Test D — Network priority

1. make Maison and iPhone available;
2. verify connection to Maison;
3. disable Maison;
4. verify fallback to iPhone;
5. restore Maison;
6. verify return to Maison after the current network transaction.

### Test E — Whisper server unavailable

1. record a note;
2. stop the API;
3. verify pending status;
4. restart the API;
5. verify automatic synchronization.

---

## 32. Points to watch

### Resolution

The hardware is **200 × 200**, not 240 × 240.

### ePaper

Do not treat the display as an animated screen.

### iPhone hotspot

Enable **Maximize Compatibility** if needed to ensure 2.4 GHz.

### iPhone VPN

Explicitly test hotspot-client routing through the VPN. Do not rely on this behavior without verification.

### BOOT button

GPIO0 is a strapping pin. Prefer a free GPIO for the main button if possible.

### SD

Waveshare documentation requires a FAT32 TF card.

### V1 / V2

Never mix GPIO configurations and examples from different revisions.

---

## 33. Possible extensions

After the MVP:

- note search;
- automatic tags through a local LLM;
- daily summary;
- project/personal classification;
- Home Assistant endpoint;
- web interface for viewing notes;
- Git/Markdown synchronization;
- Obsidian export;
- local encryption;
- voice-controlled deletion;
- TTS to read a note;
- Bluetooth keyboard;
- OTA from a private GitHub release;
- unobtrusive temperature/humidity indicator;
- estimated remaining battery;
- network/SD/audio diagnostics page.

---

## 34. Mockups included in this archive

```text
screens/
├── reference/
│   ├── 01_idle_note.png
│   ├── 02_recording.png
│   ├── 03_offline_queue.png
│   ├── 04_syncing.png
│   └── 05_menu.png
└── native_200x200/
    ├── 01_idle_note_200x200_1bit.png
    ├── 02_recording_200x200_1bit.png
    ├── 03_offline_queue_200x200_1bit.png
    ├── 04_syncing_200x200_1bit.png
    └── 05_menu_200x200_1bit.png
```

`reference/` contains the five historical generated mockups. Their embedded French raster labels are retained as historical design evidence, not current English UI screenshots.

`native_200x200/` contains 1-bit conversions intended only to visualize the actual panel constraints. These also retain French raster labels. Rebuild the UI with graphics primitives rather than editing or reusing these mockups.

---

## 35. Technical sources

Official Waveshare documentation:

- ESP32-S3-ePaper-1.54 : https://docs.waveshare.com/ESP32-S3-ePaper-1.54
- Resources and schematic: https://docs.waveshare.com/ESP32-S3-ePaper-1.54/Resources-And-Documents

Apple hotspot documentation:

- Personal Hotspot / Maximize Compatibility : https://support.apple.com/guide/security/wi-fi-security-secfd166f620/web

Recheck these sources during development if Waveshare releases a new hardware revision.

---

## 24. Starter project implementation included in this ZIP

This design has now been materialized into an initial project tree.

```text
esp32_s3_voice_notes_project/
├── README.md
├── firmware/
│   ├── platformio.ini
│   ├── sdkconfig.defaults
│   ├── partitions.csv
│   ├── include/
│   │   ├── board_pins.h
│   │   ├── project_config.h
│   │   ├── secrets.example.h
│   │   └── secrets.h
│   └── src/
│       ├── main.cpp
│       ├── app/button.*
│       ├── audio/audio_recorder.*
│       ├── audio/wav_writer.*
│       ├── display/epaper_display.*
│       ├── display/ui.*
│       ├── network/api_client.*
│       ├── network/wifi_manager.*
│       └── storage/note_store.*
├── lxc/
│   ├── server/
│   │   ├── app.py
│   │   └── requirements.txt
│   ├── systemd/voice-notes-api.service
│   ├── lxc.env.example
│   └── scripts/
│       ├── deploy-proxmox.sh
│       ├── install-server.sh
│       ├── update-server.sh
│       └── test-api.sh
└── docs/
    ├── TECHNICAL_DESIGN.md
    ├── references/
    └── screens/
```

### 24.1 Firmware stack

The starter implementation uses **PlatformIO + ESP-IDF** and the Espressif `esp_codec_dev` managed component for the ES8311 codec.

Default audio format:

```text
16 kHz
16-bit signed PCM
mono
WAV
```

The firmware performs the following sequence:

```text
button double-click
      ↓
record to /sdcard/audio/recording/current.tmp
      ↓
short press
      ↓
finalize WAV header + fsync
      ↓
atomic rename to /sdcard/audio/pending/<note-id>.wav
      ↓
Wi-Fi available?
  ┌───┴────┐
 no       yes
  │         │
wait      POST WAV
            ↓
          Whisper
            ↓
      transcription JSON
            ↓
     write Markdown + fsync
            ↓
      archive original WAV
```

### 24.2 Hardware definitions used by the starter

All board assumptions live in `firmware/include/board_pins.h`.

| Function | GPIO |
|---|---:|
| ePaper power | 6 |
| ePaper BUSY | 8 |
| ePaper RST | 9 |
| ePaper DC | 10 |
| ePaper CS | 11 |
| ePaper SCLK | 12 |
| ePaper MOSI | 13 |
| ES8311 MCLK | 14 |
| ES8311 BCLK | 15 |
| ES8311 ADC data to ESP | 16 |
| ES8311 LRCK | 38 |
| ES8311 DAC data from ESP | 45 |
| Audio power | 42 |
| PA control | 46 |
| I2C SDA | 47 |
| I2C SCL | 48 |
| SD CLK | 39 |
| SD D0 | 40 |
| SD CMD | 41 |
| Application button | 1 |

The application button is intentionally placed on a free GPIO rather than GPIO0/BOOT. It is expected to be wired between GPIO1 and GND and uses the ESP32 internal pull-up.

### 24.3 ePaper implementation rule

The display driver uses an initial full refresh followed by automatic full-frame differential partial refresh. Identical frames are skipped; after 10 successful partial updates, the next changed frame performs full cleaning. Sleep and transport failures invalidate the reference and require full recovery. The application must not attempt LCD-like animation. Physical latency and ghosting remain unverified.

The recording screen is drawn once at recording start. The waveform is a static visual indicator. A live timer is intentionally not refreshed once per second.

Before enclosure freeze, validate the following on the real V2 board:

- image orientation;
- BUSY polarity;
- full refresh waveform behavior;
- ghosting after multiple UI transitions;
- sleep/power-off sequence.

If required, only `display/epaper_display.cpp` should need adaptation to the exact Waveshare waveform implementation.

### 24.4 LXC API implementation

The starter server uses:

```text
FastAPI
Uvicorn
faster-whisper
SQLite
systemd
```

Default listen address:

```text
0.0.0.0:8080
```

Main endpoint:

```http
POST /api/v1/notes/{note_id}/transcribe
Authorization: Bearer <token>
Content-Type: audio/wav
```

The request body is the raw WAV file. The API stores the audio file before starting transcription.

Successful response:

```json
{
  "id": "20260915T220104Z-0043",
  "status": "done",
  "language": "fr",
  "duration": 11.2,
  "text": "Project meeting tomorrow morning.",
  "model": "small",
  "sha256": "...",
  "error": null
}
```

### 24.5 API idempotence

`note_id` is the primary key of the server SQLite table.

If the ESP retries a note for which transcription is already complete, the server returns the existing result. This is required because the ESP cannot safely know whether a connection failure happened before or after the server completed a request.

### 24.6 Initial LXC sizing

Suggested starting point for CPU inference:

```text
Debian 13 LXC
4 vCPU
4 GB RAM
1 GB swap
16 GB disk
Whisper model: small
Compute type: int8
Language: fr
```

These values are configurable in `lxc/lxc.env`.

### 24.7 Deployment flow

On Proxmox:

```bash
cd lxc
cp lxc.env.example lxc.env
nano lxc.env
./scripts/deploy-proxmox.sh
```

The deployment script:

1. locates a Debian 13 LXC template;
2. creates an unprivileged LXC;
3. starts it;
4. copies the server files;
5. installs Python, FFmpeg and the virtual environment;
6. installs faster-whisper;
7. generates an API token if none is supplied;
8. installs and starts a systemd unit.

### 24.8 First development milestones

Recommended order:

1. build and flash a minimal firmware;
2. validate SD mounting;
3. validate ePaper full refresh;
4. validate the one-button event detector;
5. record a WAV and inspect it on a PC;
6. deploy the LXC and test it with `curl`;
7. configure the ESP API URL and token;
8. test one online recording end-to-end;
9. disable Wi-Fi and record three notes;
10. restore Wi-Fi and verify automatic queue synchronization;
11. test hotspot fallback;
12. add deep sleep only after the complete flow is stable.

### 24.9 Known MVP limitations

- history browsing is represented in the menu but not implemented yet;
- RTC-specific time acquisition is not implemented yet; NTP is used when Wi-Fi is available;
- battery percentage is not yet read from the ADC;
- OTA is reserved for a later milestone;
- ePaper partial refresh is implemented; physical waveform performance and ghosting still require validation;
- network credentials are compile-time values in `secrets.h`; moving them to NVS is planned;
- the synchronous HTTP request can keep the ESP awake while Whisper works; an asynchronous job API can be introduced later if needed.
