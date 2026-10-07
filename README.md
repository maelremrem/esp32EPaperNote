# ESP32-S3 ePaper Voice Notes

Portable, offline-first voice note recorder for the **Waveshare ESP32-S3-ePaper-1.54 V2**.

The ESP32 records a WAV file to microSD. During recording, available Wi-Fi enables provisional Whistle transcription from short PCM windows on a self-hosted LXC. After stopping, the complete saved WAV is uploaded for authoritative transcription, stored locally as Markdown.

Live preview is windowed, not word-by-word streaming: audio windows are 4.096 seconds. The e-paper driver automatically uses partial refresh after its initial full frame and skips identical frames. Settings → Full refresh selects 1, 5, 10 (default), 20, 50 or 100 partial updates before the next changed frame performs full cleaning, or Full only. BOOT short cycles choices; long saves to NVS and returns. This setting works without SD and does not change STT windows. Physical speed and ghosting still require device validation. Preview audio may be dropped during network work without affecting the local WAV. See `lxc/README.md` for Whistle deployment and local native/HTTP validation, and `firmware/tests/README.md` for verification and hardware limitations. Actual-render UI previews are in `docs/screens/reference-ui/` (`overview.png` shows the four reference-inspired screens).

## Repository layout

```text
.
├── firmware/   PlatformIO + ESP-IDF project for the ESP32-S3
├── lxc/        Proxmox/LXC deployment scripts + Whistle API
└── docs/       Technical design document + UI mockups
```

## Intended hardware

- Waveshare ESP32-S3-ePaper-1.54 **V2**
- ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB PSRAM
- 1.54-inch 200×200 black/white e-paper
- ES8311 codec + onboard microphone
- microSD/TF card formatted FAT32
- onboard BOOT button on `GPIO0` for recording and menu navigation
- PWR button remains dedicated to board power management

## Quick start

### 1. Deploy the transcription server

Run this one-liner as root in the Proxmox host terminal **after the bootstrap and
current server files have been published to the repository's `main` branch**:

```bash
bash -c 'set -e; script=$(curl --fail --silent --show-error --location --proto "=https" --proto-redir "=https" --connect-timeout 15 --max-time 60 https://raw.githubusercontent.com/maelremrem/esp32EPaperNote/main/lxc/scripts/bootstrap-proxmox.sh); bash -s <<< "$script"'
```

The interactive installer asks for CPU cores, RAM, swap, disk size, CT/template
storages and networking, then asks for confirmation. It creates an unprivileged
Debian 13 CT, updates it, installs Whistle and checks the service before reporting
success. Failed CTs are kept for diagnostics; tokens are not printed. This executes
code from this repository as root: inspect it first. Local uncommitted changes are
not included in the download. See `lxc/README.md` for immutable-revision installation
and unattended options. Until published, the bootstrap URL may return HTTP 404.

Alternatively, copy the current `lxc/` directory to the Proxmox host and run:

```bash
cd lxc
sudo bash scripts/deploy-proxmox.sh
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

## Carnet web interfaces

Both interfaces are English, responsive and framework-free, with local assets and
no remote fonts or analytics. Enter the existing API bearer token in their access
form; it is not saved in browser storage.

- ESP32: open `http://<ESP32-IPv4>/` on the same trusted Wi-Fi network. View the
  device state, pending recordings and latest session note; start/stop the onboard
  microphone and request synchronization. The authenticated **Transcription server**
  form lets you save a base URL such as `http://192.168.1.20:8080` without reflashing.
  Use the LXC's IPv4 address and service port, not the ESP32 address. Settings persist
  in NVS and apply to both live preview and final uploads. Changes are blocked during
  recording/synchronization; failed saves retain the previous address. The server must
  still accept the firmware's existing API token. Only HTTP/HTTPS literal IPv4 origins
  are supported (no hostname or base path). See `firmware/tests/WEB.md`.
- LXC: open `http://<LXC-IP>:8080/` (or your HTTPS proxy). Search/filter archived
  notes, read/copy/export transcripts and listen to/download their original WAV.
  The install/update scripts include the web assets. See `lxc/README.md`.

Screenshots and verification notes are in `docs/screens/web/`. Browser fixtures
are test data, not real recordings. Firmware compilation and host/browser checks
do not replace testing on the physical ESP32. Do not expose the ESP32's plain HTTP
interface to the Internet; use HTTPS for non-private LXC access.

Interface labels, logs and documentation are English. Speech recognition still
defaults to French (`fr`); transcript text remains in the recorded language.
The original PNG mockups in `docs/screens/reference/` and
`docs/screens/native_200x200/` retain historical French raster labels and are not
screenshots of the current interface.

## One-button interaction

| State | Short press | Double press | Long press |
|---|---|---|---|
| Idle (online or offline) | new recording | new recording | settings |
| Recording | stop + save | stop + save | stop + save |
| Settings / saved notes | next item | next item | select / open |
| Note reader | next page (wraps) | next page (wraps) | saved notes |
| Information | settings | settings | settings |

The 200×200 monochrome interface follows the supplied references: large microphone,
inverted selected rows, pixel icons, separators and a two-column button legend.
English labels use the panel's existing ASCII font.

Settings contains Notes, Wi-Fi (reconnect and show network information), Sync
(request synchronization; automatic idle synchronization remains enabled),
Storage, About and Back. Notes lists up to 100 saved IDs from pending/archived
WAV files and Markdown transcripts, deduplicated and sorted by descending ID;
this is not a reliable chronological order for IDs created before clock sync.
The document icon denotes an existing transcript; the microphone denotes audio
without a transcript. Long-press opens text, not audio playback. A Return entry
exits the list. Reading is paginated, with bounded transcript loading; the complete
Markdown file stays on the card. Synchronization does not interrupt menu browsing.

No battery percentage, brightness slider, fabricated duration/date or free-space
estimate is displayed. The SD symbol is the mount result, not hot-plug detection.
A failed mount shows a large warning, a FAT32 hint and a restart instruction.
Startup errors never offer a nonfunctional button action. These are firmware
changes and host-rendered previews; physical panel/button checks are still needed.

## Data safety rule

A recording is always finalized on the SD card **before** any network operation starts. A WAV file is only archived/deleted after a valid transcription has been written locally.

## Status of this starter project

This ZIP is a development baseline rather than a production release. The server implementation is included and its Python/shell syntax is validated in this package. The firmware contains the full application architecture, SD queue, WAV recording path, Wi-Fi priority logic, API client and UI state machine. The e-paper and ES8311 board glue is isolated so hardware validation can be done without changing the application layer.

See [`docs/TECHNICAL_DESIGN.md`](docs/TECHNICAL_DESIGN.md) for the full design.
