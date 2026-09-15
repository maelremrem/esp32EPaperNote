# ESP32-S3 ePaper Voice Notes

Portable, offline-first voice note recorder for the **Waveshare ESP32-S3-ePaper-1.54 V2**.

The ESP32 records a WAV file to microSD first. When a known Wi-Fi network is available, the note is uploaded to a self-hosted LXC running Whisper. The returned transcription is stored locally on the ESP32 SD card as Markdown.

## Repository layout

```text
.
├── firmware/   PlatformIO + ESP-IDF project for the ESP32-S3
├── lxc/        Proxmox/LXC deployment scripts + Whisper API
└── docs/       Technical design document + UI mockups
```

## Intended hardware

- Waveshare ESP32-S3-ePaper-1.54 **V2**
- ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB PSRAM
- 1.54-inch 200×200 black/white e-paper
- ES8311 codec + onboard microphone
- microSD/TF card formatted FAT32
- one application button connected between `GPIO1` and GND
- PWR button remains dedicated to board power management

## Quick start

### 1. Deploy the transcription server

On the Proxmox host:

```bash
cd lxc
cp lxc.env.example lxc.env
nano lxc.env
sudo ./scripts/deploy-proxmox.sh
```

The deployment script creates a Debian LXC, copies the API files into it, installs the Python service and starts `voice-notes-api.service`.

For an existing Debian LXC, copy `lxc/` into the container and run:

```bash
sudo ./scripts/install-server.sh
```

### 2. Configure the ESP32 firmware

```bash
cd firmware
cp include/secrets.example.h include/secrets.h
nano include/secrets.h
```

Set:

- home Wi-Fi credentials;
- iPhone hotspot credentials;
- API URL;
- API bearer token.

### 3. Build and flash

```bash
pio run
pio run -t upload
pio device monitor
```

## One-button interaction

| State | Short press | Double press | Long press |
|---|---|---|---|
| Idle | refresh/current note* | new recording | menu |
| Recording | stop + save | — | stop + save |
| Menu | next item | next item | select |
| Offline with pending notes | refresh/current note* | new recording | reconnect + force sync |

`*` Browsing previous/next notes is represented in the UI design but left as the next firmware milestone.

## Data safety rule

A recording is always finalized on the SD card **before** any network operation starts. A WAV file is only archived/deleted after a valid transcription has been written locally.

## Status of this starter project

This ZIP is a development baseline rather than a production release. The server implementation is included and its Python/shell syntax is validated in this package. The firmware contains the full application architecture, SD queue, WAV recording path, Wi-Fi priority logic, API client and UI state machine. The e-paper and ES8311 board glue is isolated so hardware validation can be done without changing the application layer.

See [`docs/TECHNICAL_DESIGN.md`](docs/TECHNICAL_DESIGN.md) for the full design.
