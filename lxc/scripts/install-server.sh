#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LXC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "Run this script as root inside the Debian LXC." >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates curl ffmpeg openssl python3 python3-venv python3-pip

if ! id voicenotes >/dev/null 2>&1; then
  useradd --system --home /var/lib/voice-notes --shell /usr/sbin/nologin voicenotes
fi

install -d -o voicenotes -g voicenotes /var/lib/voice-notes /var/lib/voice-notes/audio
install -d /opt/voice-notes/server
cp "$LXC_DIR/server/app.py" /opt/voice-notes/server/app.py
cp "$LXC_DIR/server/requirements.txt" /opt/voice-notes/server/requirements.txt

python3 -m venv /opt/voice-notes/venv
/opt/voice-notes/venv/bin/pip install --upgrade pip wheel
/opt/voice-notes/venv/bin/pip install -r /opt/voice-notes/server/requirements.txt

API_TOKEN="${API_TOKEN:-}"
if [[ -z "$API_TOKEN" ]]; then
  API_TOKEN="$(openssl rand -hex 32)"
fi

WHISPER_MODEL="${WHISPER_MODEL:-small}"
WHISPER_COMPUTE_TYPE="${WHISPER_COMPUTE_TYPE:-int8}"
WHISPER_LANGUAGE="${WHISPER_LANGUAGE:-fr}"

cat > /etc/voice-notes.env <<ENV
VOICE_NOTES_API_TOKEN=${API_TOKEN}
VOICE_NOTES_DATA_DIR=/var/lib/voice-notes
WHISPER_MODEL=${WHISPER_MODEL}
WHISPER_DEVICE=cpu
WHISPER_COMPUTE_TYPE=${WHISPER_COMPUTE_TYPE}
WHISPER_LANGUAGE=${WHISPER_LANGUAGE}
MAX_UPLOAD_BYTES=67108864
ENV
chmod 600 /etc/voice-notes.env

cp "$LXC_DIR/systemd/voice-notes-api.service" /etc/systemd/system/voice-notes-api.service
systemctl daemon-reload
systemctl enable --now voice-notes-api.service

cat <<INFO

Voice Notes API installed.

API URL:   http://$(hostname -I | awk '{print $1}'):8080
API token: ${API_TOKEN}
Health:    curl http://127.0.0.1:8080/health
Logs:      journalctl -u voice-notes-api -f

Copy the API URL and token into firmware/include/secrets.h.
INFO
