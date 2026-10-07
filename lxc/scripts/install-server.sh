#!/usr/bin/env bash
set -euo pipefail
set +x

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LXC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "Run this script as root inside the Debian LXC." >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive LC_ALL=C LANG=C
apt-get update -o APT::Update::Error-Mode=any -o Acquire::Retries=3 \
  -o Acquire::http::Timeout=15 -o Acquire::https::Timeout=15
apt-get install -y --no-install-recommends \
  ca-certificates curl openssl python3 python3-venv python3-pip

if ! id voicenotes >/dev/null 2>&1; then
  useradd --system --home /var/lib/voice-notes --shell /usr/sbin/nologin voicenotes
fi

install -d -o voicenotes -g voicenotes /var/lib/voice-notes /var/lib/voice-notes/audio
install -d /opt/voice-notes/server
cp "$LXC_DIR/server/app.py" /opt/voice-notes/server/app.py
cp "$LXC_DIR/server/requirements.txt" /opt/voice-notes/server/requirements.txt
install -d /opt/voice-notes/server/web
cp "$LXC_DIR/server/web/index.html" /opt/voice-notes/server/web/index.html
cp "$LXC_DIR/server/web/app.css" /opt/voice-notes/server/web/app.css
cp "$LXC_DIR/server/web/app.js" /opt/voice-notes/server/web/app.js

python3 -m venv /opt/voice-notes/venv
/opt/voice-notes/venv/bin/pip install --upgrade pip wheel
/opt/voice-notes/venv/bin/pip install -r /opt/voice-notes/server/requirements.txt

API_TOKEN="${API_TOKEN:-}"
umask 077
if [[ -z "$API_TOKEN" ]]; then
  API_TOKEN="$(openssl rand -hex 32)"
fi

WHISTLE_LANGUAGE="${WHISTLE_LANGUAGE-fr}"

cat > /etc/voice-notes.env <<ENV
VOICE_NOTES_API_TOKEN=${API_TOKEN}
VOICE_NOTES_DATA_DIR=/var/lib/voice-notes
WHISTLE_LANGUAGE=${WHISTLE_LANGUAGE}
HOME=/var/lib/voice-notes
HF_HOME=/var/lib/voice-notes/.cache/huggingface
NEEDLE_TELEMETRY=0
DO_NOT_TRACK=1
MAX_UPLOAD_BYTES=67108864
ENV
chmod 600 /etc/voice-notes.env

# Cache the weights AND native engine as the service user, not under /root.
runuser -u voicenotes -- env HOME=/var/lib/voice-notes \
  HF_HOME=/var/lib/voice-notes/.cache/huggingface NEEDLE_TELEMETRY=0 DO_NOT_TRACK=1 \
  /opt/voice-notes/venv/bin/python -c 'import needle; print(needle.Whistle().transcribe([0.0] * 16000))'

cp "$LXC_DIR/systemd/voice-notes-api.service" /etc/systemd/system/voice-notes-api.service
systemctl daemon-reload
systemctl enable --now voice-notes-api.service

cat <<INFO

Voice Notes API files installed; service started. Verify health before use.

API token: stored in /etc/voice-notes.env (not printed).
Health:    verify /health on the assigned CT address before use.
Logs:      journalctl -u voice-notes-api -f

After health verification, configure the firmware with the CT address and token.
INFO
