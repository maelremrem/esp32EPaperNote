#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LXC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ENV_FILE="$LXC_DIR/lxc.env"

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "Run this script as root on the Proxmox host." >&2
  exit 1
fi

if [[ ! -f "$ENV_FILE" ]]; then
  echo "Missing $ENV_FILE. Copy lxc.env.example to lxc.env first." >&2
  exit 1
fi

# shellcheck disable=SC1090
source "$ENV_FILE"

: "${VMID:?Missing VMID}"
: "${HOSTNAME:=voice-notes-stt}"
: "${STORAGE:=local-lvm}"
: "${TEMPLATE_STORAGE:=local}"
: "${BRIDGE:=vmbr0}"
: "${IP_CONFIG:=dhcp}"
: "${CORES:=4}"
: "${MEMORY_MB:=4096}"
: "${SWAP_MB:=1024}"
: "${DISK_GB:=16}"
: "${UNPRIVILEGED:=1}"
: "${START_ON_BOOT:=1}"

if pct status "$VMID" >/dev/null 2>&1; then
  echo "LXC $VMID already exists. Refusing to overwrite it." >&2
  exit 1
fi

pveam update >/dev/null
TEMPLATE="$(pveam available --section system | awk '$2 ~ /^debian-13-standard_/ {print $2}' | tail -n1)"
if [[ -z "$TEMPLATE" ]]; then
  echo "No Debian 13 standard template found in pveam." >&2
  exit 1
fi

if ! pveam list "$TEMPLATE_STORAGE" | grep -Fq "$TEMPLATE"; then
  echo "Downloading $TEMPLATE..."
  pveam download "$TEMPLATE_STORAGE" "$TEMPLATE"
fi

pct create "$VMID" "$TEMPLATE_STORAGE:vztmpl/$TEMPLATE" \
  --hostname "$HOSTNAME" \
  --cores "$CORES" \
  --memory "$MEMORY_MB" \
  --swap "$SWAP_MB" \
  --rootfs "$STORAGE:$DISK_GB" \
  --net0 "name=eth0,bridge=$BRIDGE,ip=$IP_CONFIG" \
  --unprivileged "$UNPRIVILEGED" \
  --features nesting=1 \
  --onboot "$START_ON_BOOT" \
  --start 1

sleep 3

TMP_ARCHIVE="$(mktemp --suffix=.tar.gz)"
trap 'rm -f "$TMP_ARCHIVE"' EXIT
tar -C "$LXC_DIR" -czf "$TMP_ARCHIVE" server systemd scripts/install-server.sh
pct push "$VMID" "$TMP_ARCHIVE" /root/voice-notes-deploy.tar.gz
pct exec "$VMID" -- bash -lc 'mkdir -p /root/voice-notes-deploy && tar -xzf /root/voice-notes-deploy.tar.gz -C /root/voice-notes-deploy'

INSTALL_ENV=(
  "API_TOKEN=${API_TOKEN:-}"
  "WHISPER_MODEL=${WHISPER_MODEL:-small}"
  "WHISPER_COMPUTE_TYPE=${WHISPER_COMPUTE_TYPE:-int8}"
  "WHISPER_LANGUAGE=${WHISPER_LANGUAGE:-fr}"
)

pct exec "$VMID" -- env "${INSTALL_ENV[@]}" bash /root/voice-notes-deploy/scripts/install-server.sh

echo
echo "LXC $VMID deployed."
pct exec "$VMID" -- hostname -I
