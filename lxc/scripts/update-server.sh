#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LXC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "Run as root inside the LXC." >&2
  exit 1
fi

cp "$LXC_DIR/server/app.py" /opt/voice-notes/server/app.py
cp "$LXC_DIR/server/requirements.txt" /opt/voice-notes/server/requirements.txt
install -d /opt/voice-notes/server/web
cp "$LXC_DIR/server/web/index.html" /opt/voice-notes/server/web/index.html
cp "$LXC_DIR/server/web/app.css" /opt/voice-notes/server/web/app.css
cp "$LXC_DIR/server/web/app.js" /opt/voice-notes/server/web/app.js
/opt/voice-notes/venv/bin/pip install -r /opt/voice-notes/server/requirements.txt
# Preserve the token/data path and migrate the old language setting in place.
/opt/voice-notes/venv/bin/python -c '
from pathlib import Path
p = Path("/etc/voice-notes.env")
lines = p.read_text().splitlines()
settings = dict(line.split("=", 1) for line in lines if "=" in line and not line.startswith("#"))
language = settings.get("WHISTLE_LANGUAGE", settings.get("WHISPER_LANGUAGE", "fr"))
lines = [line for line in lines if not line.startswith(("WHISPER_", "WHISTLE_LANGUAGE=", "HOME=", "HF_HOME=", "NEEDLE_TELEMETRY=", "DO_NOT_TRACK="))]
lines += ["WHISTLE_LANGUAGE=" + language, "HOME=/var/lib/voice-notes", "HF_HOME=/var/lib/voice-notes/.cache/huggingface", "NEEDLE_TELEMETRY=0", "DO_NOT_TRACK=1"]
p.write_text("\n".join(lines) + "\n")
p.chmod(0o600)
'
runuser -u voicenotes -- env HOME=/var/lib/voice-notes \
  HF_HOME=/var/lib/voice-notes/.cache/huggingface NEEDLE_TELEMETRY=0 DO_NOT_TRACK=1 \
  /opt/voice-notes/venv/bin/python -c 'import needle; print(needle.Whistle().transcribe([0.0] * 16000))'
cp "$LXC_DIR/systemd/voice-notes-api.service" /etc/systemd/system/voice-notes-api.service
systemctl daemon-reload
systemctl restart voice-notes-api.service
systemctl --no-pager --full status voice-notes-api.service
