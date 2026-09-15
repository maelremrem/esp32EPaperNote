#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALLER="$SCRIPT_DIR/install-server.sh"

if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
  exec "$INSTALLER"
fi

if command -v sudo >/dev/null 2>&1; then
  exec sudo "$INSTALLER"
fi

echo "Run this script as root, or install sudo." >&2
exit 1
