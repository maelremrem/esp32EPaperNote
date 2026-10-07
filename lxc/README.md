# LXC Whistle server

This directory contains both the API application and deployment scripts.

## Recommended LXC

- Debian 13
- 4 vCPU
- 4 GB RAM
- 1 GB swap
- 16 GB disk
- unprivileged container
- DHCP initially, then reserve its address in your DHCP server

The CPU backend is **Whistle**, through `cactus-needle==3.1.1` (import `needle`).
The installer downloads the native engine and model and runs a silence smoke check
as `voicenotes`. Its writable cache is under `/var/lib/voice-notes/.cache`, not
`/root` or `/opt`; `NEEDLE_TELEMETRY=0` and `DO_NOT_TRACK=1` are set.

Set `WHISTLE_LANGUAGE=fr` in `/etc/voice-notes.env`; an empty value enables
detection. Supported languages are `en,de,fr,es,it,nl,pl`. Optionally set
`NEEDLE_WHISTLE_WEIGHTS` to an existing local Whistle `.cact` file. Old
`WHISPER_MODEL`, device and compute-type settings are no longer used.

This is **near-live windowed inference**, not causal audio/token streaming.
Whistle consumes at most 30 seconds per call. Preview requests typically carry
nonoverlapping 4.096-second firmware windows; words crossing window boundaries can be lost
or inaccurate. The full final WAV is always authoritative. Long final recordings
are split into consecutive <=30-second windows and their text is joined in order
(no overlap/deduplication or cross-window context).

## Deploy from a Proxmox host

Run in a **root terminal on the Proxmox host**, not inside an existing CT.
The independent installer uses existing `whiptail` when the terminal supports
it, otherwise plain terminal prompts (no extra host packages), with
`/dev/tty` for input even when the bootstrap's stdin contains downloaded script
text. It does not source Community Scripts helpers or claim affiliation.

From a local checkout:

```bash
sudo bash lxc/scripts/deploy-proxmox.sh
```

Intended one-line bootstrap **after these files are published to GitHub**:

```bash
bash -c 'set -e; script=$(curl --fail --silent --show-error --location --proto "=https" --proto-redir "=https" --connect-timeout 15 --max-time 60 https://raw.githubusercontent.com/maelremrem/esp32EPaperNote/main/lxc/scripts/bootstrap-proxmox.sh); bash -s <<< "$script"'
```

The wrapper executes nothing if the initial download fails. Prefer downloading
and inspecting the bootstrap before root execution. This URL returned **404**
during local verification: this work has not been committed, pushed or deployed.
Do not advertise it as a live installer yet. `main` is mutable; for reviewed,
immutable code replace `main` in the raw URL with a full commit SHA and pass
`--revision <same-40-character-SHA>` to the bootstrap. For an inspected local
bootstrap: `bash lxc/scripts/bootstrap-proxmox.sh --revision <full-SHA>`.
Only `main` or full hexadecimal commit SHAs are accepted; there is no custom
download URL or remote helper execution. The bootstrap downloads
`https://github.com/maelremrem/esp32EPaperNote/archive/refs/heads/main.tar.gz`
(or `/archive/<SHA>.tar.gz`), validates archive paths/types and required project
files, and removes its private scratch directory on success/failure/interrupt.
HTTPS authenticates GitHub transport, not an independent release signature.

### Choices, safeguards and progress

Prompts cover CTID, hostname, CPU, RAM, swap, disk, **separate** root/template
storage, active bridge, DHCP/static IPv4 CIDR, gateway, DNS and STT language.
The default language remains `fr`; `auto` enables detection. Storage choices
come from active node storage with `rootdir` or `vztmpl` support, not hard-coded
`local`/`local-lvm`. The suggested next ID comes from Proxmox; cluster-wide VM
and CT IDs are checked both before confirmation and immediately before create.

Input bounds are CPU 1–256, RAM 128–1048576 MiB, swap 0–1048576 MiB, disk
4–65536 GiB and CTID 100–999999999. These are input sanity limits, not a promise
that the host has capacity. Proxmox performs final resource checks. A static
address must include a prefix of /30 or wider, usable host IPv4 and gateway in
the same subnet. DNS accepts one IPv4 or `inherit`. Unprivileged mode is always
enabled; no nesting/keyctl features are needed. The validated summary requires
an explicit `yes` **before mutation**. Invalid input aborts without mutation;
rerun to correct it. No token is requested or displayed interactively.

After confirmation the installer refreshes the template catalog, selects a
validated Debian 13 amd64 standard template, downloads it if absent and reads
the template list back before create. It starts the CT and retries bounded
execution/DNS/repository TCP connectivity instead of a blind startup delay.
The CT (not the host) runs `apt-get update` and `apt-get -y upgrade`, then the
existing service installer, including native/model warmup. Only seven explicit
runtime payload files are pushed (no tests, caches or local credentials).
Readiness, service and health checks each have up to 60 attempts, with a
10-second command timeout and 2-second interval; package/model downloads can
take longer. No host apt changes are made.

Success requires `systemctl is-active` and a successful curl to
`http://<actual-eth0-IPv4>:8080/health` **inside the CT**. Only then is that URL
printed; loopback/link-local/unspecified/multicast addresses are rejected.
This is not a LAN-client reachability, TLS, speech accuracy or hardware check.
The token is generated inside the CT and written with mode 0600 from its first
write to `/etc/voice-notes.env`; it is never printed or put in `pct exec` argv.
Failures name the phase/CTID and give journal hints. Created CTs are **kept**
for diagnostics, never automatically stopped or destroyed. Interrupt traps
remove only installer scratch; review a partially created CT manually.

### Explicit unattended compatibility

No arguments now intentionally means **interactive**, not the old automatic
`lxc.env` deployment. For the legacy settings-file path use an explicit flag:

```bash
cp lxc/lxc.env.example lxc/lxc.env
# Edit this local file; do not commit credentials.
sudo bash lxc/scripts/deploy-proxmox.sh --unattended
# Or: --unattended --env-file /root/my-lxc.env
```

Unattended mode skips only the prompts/confirmation, not validation. Env files
are parsed as literal `KEY=value` assignments (optional enclosing quotes), not
shell programs: no command substitutions, expansions, inline comments or extra
keys. `UNPRIVILEGED=0` is rejected; `START_ON_BOOT=0` remains supported. Leave
`API_TOKEN` empty to generate it inside the CT. If supplying a token, only
letters/digits/dot/underscore/hyphen are accepted; it is transferred in a private
mode-0600 file, not command arguments, and removed inside the CT when the install
command exits. An interrupted push can leave that root-only staging file for
manual cleanup; it is never included in diagnostics.

### Installer verification

`python -m pytest lxc/server/tests/test_proxmox_installer.py -q` exercises real
shell scripts against temporary fake Proxmox commands and a controlling PTY:
prompts, invalid input/abort, dynamic storage, VM/CT collisions, static network,
template verification, create/start/update/install/service/health failure,
payload paths, unattended literals, bootstrap download/revision/archive safety,
streamed stdin and interrupts. Token-write permissions are exercised against a
synthetic temporary file. These are local integration/syntax tests, **not real
Proxmox provisioning**; no CT, host apt, systemd service or root config was
created by verification. ShellCheck was not installed; `bash -n` passed.

## Install into an existing LXC

Copy this directory into the container, then:

```bash
sudo ./scripts/install-server.sh
```

For an existing Whisper installation use `sudo ./scripts/update-server.sh`
instead of reinstalling: it preserves the token and data, migrates
`WHISPER_LANGUAGE`, refreshes systemd and warms the Whistle cache. Existing
completed SQLite notes are preserved, including their original model field.
Internet access to PyPI and Hugging Face is needed for installation/warmup.
The Python distribution is pinned; upstream downloads of `.cact` assets use
Hugging Face's current repository revision, not a content-pinned release.
For reproducible/offline deployments retain the warmed cache or supply local
weights and retain the native engine cache.

## Carnet library

Open `http://<LXC-IP>:8080/` (or your proxy's HTTPS URL).
The public page contains neither notes nor a token. Enter the **same bearer token**
used by the firmware to open the SQLite library. The token grants access to all
notes on the server: this is not a multi-user login. It stays only in this tab's
memory, never in browser storage. Locking, reloading or closing clears access and
visible data. A 401/403 response also locks the interface.

Search transcript text or IDs, filter transcribed/processing/failed notes,
navigate pages, and read, copy or export plain text. Original WAV audio is loaded
on demand through an authenticated request, then can be played and downloaded.
Missing files are reported without exposing their paths. Refresh reloads notes
and service status. "Whistle not loaded" means the engine has not been initialized,
not that the service is unavailable. Processing notes do not refresh automatically.
Clipboard copying requires HTTPS or localhost; `.txt` export works on HTTP LANs.

**Network security:** HTTP sends the token, notes and audio without encryption.
Use it only on a trusted private LAN. Use HTTPS through a proxy on shared networks
or for remote access; do not expose port 8080 directly. Never add tokens to URLs.
Locking does not delete files already exported to your computer. HTML/CSS/JS are
local, with no CDN, external fonts or frontend dependencies. Install/update
scripts copy the three `server/web/` files alongside `app.py`.

### Library API

`GET /api/v1/notes` requires `Authorization: Bearer <token>` and accepts:

- `limit`: 1–100, default 30;
- `offset`: 0–1000000, default 0;
- `q`: up to 200 characters, literal substring in ID or transcript (SQLite
  case-insensitive matching for ASCII; accent/case variants are not folded);
- `status`: omitted, `done`, `processing` or `error`.

Response: `{ "items": [...], "total": <filtered count>, "limit": ..., "offset": ... }`.
Ordering is newest creation first, then descending ID for ties. Note responses
include `created_at` and `updated_at` (SQLite UTC timestamps); existing fields
are preserved and filesystem paths are never serialized. An empty page returns
an empty array with the actual filtered total. Invalid parameters return 422.

`GET /api/v1/notes/{note_id}/audio` uses the same authorization and returns
`audio/wav` with a download filename. The path comes only from SQLite, must
resolve to a WAV inside the configured audio directory, and must exist; missing
notes/files or unsafe stored paths return 404. Invalid IDs return 400. Library,
note and audio responses use `Cache-Control: no-store`; the public shell has a
restrictive CSP and the asset route serves only the two named CSS/JS files.

## API

### Health

```http
GET /health
```

### Transcribe a WAV file

```http
POST /api/v1/notes/{note_id}/transcribe
Authorization: Bearer <token>
Content-Type: audio/wav

<raw WAV bytes>
```

Response:

```json
{
  "id": "20260915T220104-0043",
  "status": "done",
  "language": "fr",
  "duration": 11.2,
  "text": "Project meeting tomorrow morning.",
  "model": "whistle",
  "sha256": "...",
  "error": null
}
```

A completed `note_id` is idempotent: identical audio returns the stored result
without inference; different audio returns HTTP 409 without changing it.
Concurrent final uploads are serialized, including same-ID retries. WAV must be
nonempty PCM signed 16-bit little-endian, mono, 16000 Hz (the firmware format).
Invalid, unsupported or truncated WAV returns 400; uploads above
`MAX_UPLOAD_BYTES` (default 64 MiB) return 413. Successful finals persist the WAV,
digest and completed SQLite note. Failed inference records an error and can be
retried. `GET /api/v1/notes/{note_id}` uses the same bearer token and returns 404
until a final note exists.

### Live preview

```http
POST /api/v1/live/{note_id}
Authorization: Bearer <token>
Content-Type: application/octet-stream

<raw PCM s16le mono 16000 bytes, no WAV header>
```

```json
{"id":"20260915T220104-0043","status":"partial","text":"Meeting tomorrow.","language":"fr","model":"whistle"}
```

Each request returns only that window's transcript. The firmware appends
nonoverlapping preview text for display, then replaces it with the final WAV
response. Preview requests are stateless: **no WAV or database row is written**,
and they never set or replace a note's `done` status. Retrying a preview may
repeat text; clients must not append duplicate retries. Silence may return empty
text. No MIME type is required for raw preview bodies; `application/octet-stream`
is recommended. The declared format is fixed, not inferred from raw bytes.
Empty/odd-length bodies return 400; >960000 bytes (>30 seconds) return 413,
including chunked uploads. Exactly 30 seconds is accepted. Missing/invalid token
returns 401/403, invalid note identifiers return 400, engine failures return 500.

One process-wide engine lock covers initialization, preview and final inference:
Whistle's native model is **not thread-safe**. Work runs off the asyncio event
loop; cancellation retains the lock until the native call finishes. Keep
`uvicorn --workers 1`; more workers are independent engines, not shared locks.
Large final requests can delay previews; rate-limit and size-limit the proxy.

## Tests and verified API

For a running service, `API_TOKEN=... ./lxc/scripts/test-api.sh note.wav preview.pcm`
checks health, an optional raw preview (second argument), and an optional final
WAV (first argument), using the same generated note ID.

From the repository root, use a disposable **scratch** venv, not system Python.
The direct Python dependencies are pinned to the versions exercised below;
transitive dependencies and upstream native/model assets are not content-locked.
Keep a `pip freeze` with your validation artifacts if exact environment replay is
needed. Python >=3.10, Node.js and curl are required for the complete local check.

```bash
# Set TMPDIR to your own writable scratch directory, never a production data path.
export TMPDIR="${TMPDIR:-$HOME/.cache/voice-notes-validation}"
mkdir -p "$TMPDIR"
VENV="$TMPDIR/whistle-investigate"
python3 -m venv "$VENV"  # Or reuse an existing scratch venv.
"$VENV/bin/python" -m pip install -r lxc/server/requirements-dev.txt
PYTHON="$VENV/bin/python" bash lxc/scripts/validate-local.sh
# Native checks: may download model/engine on first use; never deploys services.
RUN_REAL_WHISTLE=1 PYTHON="$VENV/bin/python" bash lxc/scripts/validate-local.sh
# Include speech through native ASGI tests AND actual uvicorn loopback HTTP:
WHISTLE_TEST_WAV=/path/to/french16.wav RUN_REAL_WHISTLE=1 \
  PYTHON="$VENV/bin/python" bash lxc/scripts/validate-local.sh
```

The runner checks pip dependencies, `bash -n` for all scripts, every Python
`test_*.py`, and dependency-free Node UI/security tests. Without
`RUN_REAL_WHISTLE=1`, inference contracts use injected engines and native checks
are skipped. The Python glob is deliberate: directory-wide pytest also imports
`web_browser_test.py`, which requires optional Playwright.

The real HTTP test starts **its own** uvicorn, with one worker and a prebound
loopback-only socket on an ephemeral port. It generates an in-memory temporary
token, overrides the data directory with a new scratch directory, disables
telemetry, and stops/reaps its subprocesses even on failure. It never reads a
real service token or writes `/etc`, `/opt`, Proxmox or systemd. It checks public
health; 401/403 auth; invalid IDs/bodies/MIME/list bounds; oversized and chunked
preview rejection; exactly 30-second native silence; stateless preview; a
31-second final split into <=30-second native windows; authenticated note/WAV
readback; identical-audio idempotence; different-audio 409 with unchanged data;
SQLite/WAV persistence after uvicorn restart; and the existing curl smoke tool.
Optional speech must be a nonempty PCM s16le mono 16000 WAV <=30 seconds. It
checks nonempty French text and agreement between preview/final, **not accuracy**.

Each HTTP run prints its scratch artifact path `whistle-http-smoke-*/`, containing
`report.json`, `uvicorn.log`, `curl-smoke.log`, fixtures and `data/` (SQLite/WAV).
Reports contain request/status checks, transcript results and stopped server PIDs,
not bearer tokens. These transcripts/audio may still be private: remove the
scratch artifacts when no longer needed. Existing warmed native/model caches
are reused; absent caches may require Internet. No new clean-download guarantee
is implied by a cached run.

A repeatable **synthetic** French fixture is available if espeak-ng/ffmpeg are
installed; it is not a microphone recording:

```bash
SPEECH_DIR="$(mktemp -d "$TMPDIR/whistle-speech-XXXXXX")"
espeak-ng -v fr -s 135 -w "$SPEECH_DIR/source.wav" \
  'Bonjour. Ceci est une note vocale pour le projet. Réunion demain matin.'
ffmpeg -hide_banner -loglevel error -i "$SPEECH_DIR/source.wav" \
  -ar 16000 -ac 1 -c:a pcm_s16le "$SPEECH_DIR/french16.wav"
WHISTLE_TEST_WAV="$SPEECH_DIR/french16.wav" RUN_REAL_WHISTLE=1 \
  PYTHON="$VENV/bin/python" bash lxc/scripts/validate-local.sh
```

Optional browser QA is separate (not included in the runner):

```bash
"$VENV/bin/python" -m pip install playwright
WEB_API_PYTHON="$VENV/bin/python" "$VENV/bin/python" lxc/server/tests/web_browser_test.py
# Uses a local Chrome binary; CHROME can override it.
```

### Local readiness evidence and remaining deployment checks

The complete runner passed on Linux x86_64/Python 3.14.7 using
`cactus-needle==3.1.1`: **60 Python tests**, **3 Node tests**, pip dependency
checks and all six shell syntax checks. The real HTTP smoke recorded **40 HTTP
status checks**, plus SQLite/file/curl checks and server restart readback. Native
silence returned empty text. Existing normalized PCM float conversion, <=30s
windowing, engine serialization/cancellation and French defaults remain intact.
No API/inference implementation was replaced.

For the synthetic reference above, native preview and final both returned:
`Bonjour, ceci est une autre vocale pour le prochain et une autre mamata.`
This has substantial errors, including the meeting phrase; a green smoke test
is **not** evidence of acceptable speech accuracy or an ESP32 latency benchmark.

Install/update scripts and the systemd unit were inspected, not run. Shell
syntax and temporary-file configuration migration tests are **not a Debian
installation**. Host `systemd-analyze verify` cannot fully verify this unit
because `/opt/voice-notes/venv/bin/uvicorn` is deliberately not installed here.
Before any real deployment, separately verify:

- Debian 13/Python 3.13 package installation, native shared-library compatibility,
  network/asset downloads, service-user cache ownership and systemd sandboxing.
- `/var/lib/voice-notes` is the persistent writable path declared in the supplied
  unit. A preserved custom `VOICE_NOTES_DATA_DIR` outside that tree requires a
  matching `ReadWritePaths` override and ownership, otherwise systemd denies
  its writes.
- Use **update**, not fresh install, for an existing instance: fresh install
  generates/writes configuration and may replace its token. The updater preserves
  token/data settings but replaces files/packages in place; it is not a rollback
  or atomic upgrade mechanism. Back up configuration, SQLite and audio first.
- Keep `--workers 1`, test `/health` **and actual inference** (health alone does
  not initialize Whistle), and verify stored notes/audio after restart.
- Supply HTTPS/proxy limits for any untrusted network and exercise real ESP32
  microphone quality, preview boundaries, retry timing and offline behavior.

The installer no longer prints the bearer token; it points to the root-readable
`/etc/voice-notes.env` instead. No LXC/service was deployed by these local tests.

The installed 3.1.1 API was inspected and exercised: `needle.Whistle(weights=None)`
then `model.transcribe(array.array("f", normalized_samples), language="fr")`
returns a dictionary with `text`, `language`, `ttft_ms`, `decode_tps`. PCM16 must
be normalized to floats in [-1,1]; raw PCM bytes are **not** accepted as PCM16 by
the Python API (bytes are interpreted as float32). No faster-whisper parameters
are passed. Version 3.1.1 fetches Whistle weights in its `whistle/2.0.0` cache and
the shared Needle 3 engine version `3.1.0`.

Sources: [Whistle article](https://cactuscompute.com/blog/whistle),
[Needle source](https://github.com/cactus-compute/needle),
[Whistle model](https://huggingface.co/Cactus-Compute/whistle).

## Systemd

```bash
systemctl status voice-notes-api
journalctl -u voice-notes-api -f
```

## Reverse proxy

For access outside the home network, expose only port 8080 through your HTTPS reverse proxy, keep the bearer token enabled, and apply a reasonable upload/rate limit. The firmware accepts a configurable HTTP or HTTPS API URL.
