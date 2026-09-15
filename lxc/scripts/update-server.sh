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
/opt/voice-notes/venv/bin/pip install -r /opt/voice-notes/server/requirements.txt
systemctl restart voice-notes-api.service
systemctl --no-pager --full status voice-notes-api.service
