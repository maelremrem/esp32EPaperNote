# Carnet: local ESP32 web console

Open `http://<ESP32 station IPv4>/` on the same trusted Wi-Fi network. Use the
literal address shown under physical **Settings → Wi-Fi → ESP32 IP**, not a
hostname or reverse proxy. Fixed Host and Origin checks run before authorization.
No station API starts the captive access point.

## Local authorization is independent of transcription

On the ESP32 select **Settings → Wi-Fi → Open web settings**. The panel shows the
address and a fresh six-digit random PIN, including any leading zeros. Choose
**Connect** in the website or open locked **Settings**, then enter the PIN in the
connection modal. Cancel/Escape closes it without authorization; PIN input is
cleared when it closes. Five incorrect submitted credentials revoke the PIN and
queued commands across all API routes. Reopen access physically for a new PIN;
successful requests do not reset the failure budget. Missing authorization or
foreign Host/Origin requests do not consume PIN attempts.
This physically authorized session has an absolute ten-minute lifetime, not a
sliding lifetime. Opening another session invalidates the old code. Station loss,
IP change, server stop, expiry, or authenticated **Disconnect** revokes access.

The Settings shell and transcription address/token fields can be opened before
pairing; reads of private settings, notes, status, downloads and every write remain
protected by the local code. The LXC/STT token is **never** accepted as console
authorization. No permanent local password, cookies, localStorage or sessionStorage
are used. Codes go only in Authorization headers, never URLs, logs or assets.
Bearer headers plus fixed-origin/Host validation protect against cross-site writes.
Requests queued before expiry/loss are rejected when main takes the mailbox.
Disconnect clears private readback, passwords, note content and code immediately;
late responses are discarded by an access-generation check.

**HTTP is plaintext.** Use a trusted private LAN only. Do not expose ESP32 port 80
publicly. Anyone who can observe the code over this LAN can use it until revoked.
Physical flash access can recover unencrypted NVS secrets; this firmware does not
claim secure flash storage or TLS for the local console.

## Settings

Choose **Settings** beside **Notebook**, then Wi-Fi, Transcription server, Display
or Storage. The existing two-slot Wi-Fi, reconnect, persistent refresh interval,
non-destructive mount retry and Cancel-default, single-use 60-second format
confirmation remain available. SSIDs are read back, passwords are not. Blank
password retains a secret only for an unchanged SSID; new passwordless networks
require explicit Open. Saving Wi-Fi does not reconnect; Reconnect is separate and
may change the device address. Offline provisioning remains physically started.

Storage reports actual mounted/audio readiness, `NoteStore::lastError()` and FAT
usage. Main queries the installed IDF `esp_vfs_fat_info` only with stopped workers,
never during a download lease. A successful nonzero total and free <= total yield
used/total plus a gauge; errors/unavailable/busy storage say **Usage unknown**, not
an invented percentage. Bytes are uint64 internally; JSON numbers use cJSON's
numeric representation. Format still requires mounted responsive FAT storage,
stopped workers and main revalidation; damaged/unmountable cards need external
formatting. All cache entries are invalidated once destruction starts.

## Runtime transcription URL and write-only token

**Settings → Transcription server** accepts either a literal IPv4, `IPv4:port`, or
a full `http://`/`https://` IPv4 origin. The browser expands bare IPv4 to HTTP port
8080 and preserves explicit origin protocols/ports. Full origins without a port
retain normal HTTP 80 / HTTPS 443 behavior. No hostname, IPv6, credentials, path,
query, fragment, control character, ambiguous leading zeros, loopback, unspecified,
multicast/reserved, link-local or current-device address is accepted. Maximum URL
length is 63 bytes. HTTPS requires a trusted certificate matching the server IP.
Both preview and final upload disable HTTP redirects.

The **New transcription server token** is write-only, at most 192 printable
non-space ASCII bytes. Blank means unchanged. To remove the token, leave it blank
and explicitly select **Clear the saved transcription token**. The browser clears
entered secrets on submission/readback/logout; API GETs return only a configured
flag and successful-change revision. Clearing the token disables transcription,
not local WAV capture or web access. The original compile token remains a migration
default only when no valid saved `server/token` exists; an explicitly saved empty
token stays empty across reboot.

Main validates URL/token again and writes NVS `server/base_url` and optional
`server/token`. Only after commit succeeds does it atomically activate the pair.
Failures retain the previous runtime pair. ApiClient takes one mutex-protected,
immutable URL/token snapshot per final or live request. No secrets are read back
or echoed into browser storage. A 202 response is **queued**, not saved; the UI
waits for matching successful completion, exact URL, a newer revision and, when
changed, the expected configured flag before claiming success.

### APIs

All API routes require the physical-session Bearer code plus fixed Host/Origin.

- GET `/api/status`: bounded state/text and command completion snapshot.
- GET `/api/settings`: SSIDs/IP, mount/audio diagnostic, refresh interval, format
  challenge and `usage_known`, `total_bytes`, `free_bytes`; no passwords.
- POST `/api/settings`: JSON cap 768 bytes. Existing exact schemas:
  `{"action":"wifi","profiles":[{"ssid":"Home","password":"","open":false},{"ssid":"Hotspot","password":"","open":false}]}`;
  `{"action":"display","partial_limit":20}`; `{"action":"mount"}`;
  `{"action":"reconnect"}`; `{"action":"prepare_format"}`;
  `{"action":"format","challenge":123}`. The sample challenge is not usable.
- GET `/api/config/server`: `base_url`, `token_configured`, `server_revision` and
  matching command metadata, never the token.
- POST same path: JSON cap 512 bytes, required `base_url`, optional `token` and
  optional boolean `clear_token`. Blank token keeps the previous value; nonblank
  token and true clear together are rejected. Unknown/duplicate fields, malformed
  JSON, escaped controls/NUL and incomplete bodies fail closed.
- POST `/api/command/{start,stop,sync}`: empty body, 202 means queued.
- POST `/api/session/close`: empty body; revokes the local session.
- GET `/api/notes`: bounded newest 40 stable safe IDs with `audio` and
  `transcribed` flags. Audio-only includes cancelled pending notes.
- GET `/api/download/audio?id=<safe-ID>` or `/api/download/markdown?id=<safe-ID>`:
  read-only attachments, `audio/wav` or `text/markdown; charset=utf-8`, stable
  `<ID>.wav`/`<ID>.md` filename, no-store/nosniff. No path or percent-encoded ID.
  Markdown returns the **complete generated file**, not the reader's truncated
  transcript prefix. Missing/malformed/nonregular/symlink files are rejected.

## Downloads and SD exclusion

HTTP never calls NoteStore or opens an arbitrary path. It reserves the one-slot
mailbox and a bounded lease. Main revalidates idle/menu, mounted card and joined
workers, resolves the safe ID, validates generated Markdown and opens a regular
read-only file. Ownership of that FILE is transferred to the handler only through
the mutex-protected handoff. While leased, physical and web recording, sync,
format, mounting, cancellation and portal launch are blocked; main skips SD scans
and capacity queries. A 2-second handoff timeout releases the request; a stale
main reply closes its file immediately.

The handler streams the observed file size in <=2048-byte chunks, without loading
WAV into ESP32 RAM. Every success, read failure, socket abort, session loss/expiry
and timeout closes the FILE **before** releasing exclusion. There is no card-pointer
use in HTTP. Browser downloads fetch authenticated bytes into a Blob, trigger the
attachment filename and revoke the object URL; original card files are unchanged.
Browser memory usage depends on file size; the ESP32 stream remains bounded.

## Physical pending cancellation

**Settings → Sync** now opens **Sync now / Pending notes / Cancel all pending /
Back**. Pending notes offers selective cancellation. Both selective and all use
an explicit **Back-default** warning; Back keeps pending notes. The confirming
choice archives the original WAV without generating a transcription. An existing
archive collision or filesystem error retains that source pending; counts report
actual results. Saved history/downloads retain audio-only status. No in-progress
blocking synchronization HTTP request is aborted; cancel while idle/in the menu.

## Reproducible checks

```sh
python3 firmware/tests/test_pending_cancellation.py
python3 firmware/tests/test_runtime_token.py
python3 firmware/tests/run_note_history_tests.py
python3 firmware/tests/test_boot_storage_mount.py
python3 firmware/tests/run_web_server_tests.py
python3 firmware/tests/test_server_settings.py
python3 firmware/tests/test_api_runtime.py
python3 firmware/tests/test_web_settings.py
node firmware/tests/web_ui_test.mjs
node firmware/tests/web_settings_test.mjs
node firmware/tests/web_server_settings_test.mjs
node firmware/tests/web_download_test.mjs
python3 tests/ui/render_test.py
CXX=clang++ HOST_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 firmware/tests/run_web_server_tests.py
```

UI tests require scratch `portal-qr-venv` (Pillow/zxing-cpp) to decode the actual
framebuffer QR, with no skips. Chrome checks use scratch `carnet-browser-venv`
(Playwright), serve production assets, and inject **explicit synthetic API
fixtures**, never fabricated device results. They cover 320/375/414/768/1440px,
real Blob downloads with exact byte readback, runtime secret clearing, public
Settings/protected writes, queued completion, XSS-safe text and no persistence:

```sh
"$TMPDIR/carnet-browser-venv/bin/python" firmware/tests/web_browser_test.py
```

Real-handler host tests compile production HTTP handlers and installed IDF cJSON
with transport/mutex adapters; filesystem tests compile production NoteStore with
scratch POSIX files and fake SD mounting. This is not real sockets or SD hardware.
Record/SD/download concurrency, card removal, network loss, HTTP-task stack high
water and heap headroom, real phone access and physical gauge/QR remain hardware
checks. Parent verification performs the clean secret-excluding public fixture
build and embedded-byte check. Never flash that nonfunctional compile fixture.
