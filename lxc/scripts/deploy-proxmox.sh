#!/usr/bin/env bash
# Independent installer. No Community Scripts code or remote helper execution.
set -Eeuo pipefail
set +x
export LC_ALL=C LANG=C
umask 077
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LXC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
UNATTENDED=0
ENV_FILE="$LXC_DIR/lxc.env"
while (($#)); do
  case "$1" in
    --unattended) UNATTENDED=1; shift ;;
    --env-file) [[ $# -ge 2 ]] || exit 2; ENV_FILE=$2; shift 2 ;;
    --help) echo 'Usage: deploy-proxmox.sh [--unattended [--env-file PATH]]'; exit 0 ;;
    *) echo 'Unknown argument.' >&2; exit 2 ;;
  esac
done
[[ $(id -u) == 0 ]] || { echo 'Run as root on the Proxmox host.' >&2; exit 1; }
for command in pvesh pveam pct python3 tar timeout; do
  command -v "$command" >/dev/null || { echo "Missing command: $command" >&2; exit 1; }
done
phase() { PHASE=$1; printf 'Phase: %s\n' "$PHASE"; }
phase discovery
VMID=unknown
SCRATCH=''
cleanup() { [[ -z "$SCRATCH" ]] || rm -rf -- "$SCRATCH"; }
failed() {
  echo "Failed phase: $PHASE; CTID: $VMID. Any created CT is kept for diagnostics." >&2
  if [[ "$PHASE" == readiness || "$PHASE" == update ]]; then
    echo 'Check CT DNS, DHCP/static gateway, bridge and firewall. A working host network does not prove CT connectivity.' >&2
    timeout 10 pct exec "$VMID" -- env LC_ALL=C LANG=C bash -c 'ip -4 addr show dev eth0; ip -4 route; printf "DNS configuration:\n"; cat /etc/resolv.conf; getent ahostsv4 deb.debian.org; getent ahostsv4 security.debian.org' >&2 || true
    echo "Repair DNS if appropriate: pct set $VMID --nameserver <reachable-DNS-IPv4>" >&2
  fi
  echo "Inspect: pct status $VMID; pct exec $VMID -- journalctl -u voice-notes-api -n 100" >&2
}
trap cleanup EXIT
trap 'failed' ERR
trap 'failed; exit 130' INT TERM
NODE=$(hostname -s)
STORAGES=$(pvesh get "/nodes/$NODE/storage" --output-format json)
ROOT_STORES=$(python3 -c 'import json,sys; print(" ".join(x["storage"] for x in json.loads(sys.argv[1]) if x.get("active") and "rootdir" in x.get("content", "").split(",")))' "$STORAGES")
TEMPLATE_STORES=$(python3 -c 'import json,sys; print(" ".join(x["storage"] for x in json.loads(sys.argv[1]) if x.get("active") and "vztmpl" in x.get("content", "").split(",")))' "$STORAGES")
NETWORKS=$(pvesh get "/nodes/$NODE/network" --output-format json)
BRIDGES=$(python3 -c 'import json,sys; print(" ".join(x["iface"] for x in json.loads(sys.argv[1]) if x.get("type") == "bridge" and x.get("active")))' "$NETWORKS")
VMID=$(pvesh get /cluster/nextid)
HOSTNAME=voice-notes-stt
CORES=4 MEMORY_MB=4096 SWAP_MB=1024 DISK_GB=16
STORAGE=${ROOT_STORES%% *} TEMPLATE_STORAGE=${TEMPLATE_STORES%% *} BRIDGE=${BRIDGES%% *}
IP_CONFIG=dhcp GATEWAY=none DNS=1.1.1.1 WHISTLE_LANGUAGE=fr
API_TOKEN='' UNPRIVILEGED=1 START_ON_BOOT=1
if ((UNATTENDED)); then
  [[ -f "$ENV_FILE" ]] || { echo 'Missing unattended env file.' >&2; exit 1; }
  # Parse assignments as data, never source/eval a user-supplied file.
  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ "$line" =~ ^[[:space:]]*(#.*)?$ ]] && continue
    [[ "$line" =~ ^([A-Z_]+)=(.*)$ ]] || { echo 'Invalid env assignment.' >&2; exit 1; }
    key=${BASH_REMATCH[1]} value=${BASH_REMATCH[2]}
    case "$key" in
      VMID|HOSTNAME|CORES|MEMORY_MB|SWAP_MB|DISK_GB|STORAGE|TEMPLATE_STORAGE|BRIDGE|IP_CONFIG|GATEWAY|DNS|WHISTLE_LANGUAGE|API_TOKEN|UNPRIVILEGED|START_ON_BOOT) ;;
      *) echo 'Invalid env key.' >&2; exit 1 ;;
    esac
    if [[ "$value" == \"*\" || "$value" == \'*\' ]]; then value=${value:1:${#value}-2}; fi
    [[ "$value" != *'$'* && "$value" != *'`'* && "$value" != *';'* ]] || { echo 'Invalid env value.' >&2; exit 1; }
    printf -v "$key" '%s' "$value"
  done < "$ENV_FILE"
  [[ "$UNPRIVILEGED" == 1 && "$START_ON_BOOT" =~ ^[01]$ ]] || { echo 'Invalid container privilege/onboot setting.' >&2; exit 1; }
  [[ -z "$API_TOKEN" || "$API_TOKEN" =~ ^[A-Za-z0-9._-]+$ ]] || { echo 'Invalid API token format.' >&2; exit 1; }
  [[ -n "$WHISTLE_LANGUAGE" ]] || WHISTLE_LANGUAGE=auto
fi
ask() {
  local variable=$1 label=$2 default=$3 answer
  if [[ ${TERM:-dumb} != dumb ]] && command -v whiptail >/dev/null; then
    # Keep dialog rendering on the terminal and only its answer in the capture.
    answer=$(whiptail --title 'Voice Notes LXC' --inputbox "$label" 12 78 "$default" --output-fd 4 4>&1 1>&3 2>&3 <&3)
  else
    printf '%s [%s]: ' "$label" "$default" >&3
    IFS= read -r answer <&3
  fi
  printf -v "$variable" '%s' "${answer:-$default}"
}
if ((!UNATTENDED)); then
exec 3<>/dev/tty
ask VMID 'Container ID' "$VMID"
ask HOSTNAME 'Hostname' voice-notes-stt
ask CORES 'CPU cores' 4
ask MEMORY_MB 'RAM (MiB)' 4096
ask SWAP_MB 'Swap (MiB)' 1024
ask DISK_GB 'Disk (GiB)' 16
ask STORAGE "Root storage ($ROOT_STORES)" "${ROOT_STORES%% *}"
ask TEMPLATE_STORAGE "Template storage ($TEMPLATE_STORES)" "${TEMPLATE_STORES%% *}"
ask BRIDGE "Bridge ($BRIDGES)" "${BRIDGES%% *}"
ask IP_CONFIG 'IPv4 CIDR or dhcp' dhcp
ask GATEWAY 'Gateway (none for DHCP)' none
ask DNS 'DNS IPv4 (use your LAN resolver if public DNS is blocked; inherit for host settings)' "$DNS"
ask WHISTLE_LANGUAGE 'Whistle language' fr
fi
phase validation
validate() {
  local resources
  resources=$(pvesh get /cluster/resources --type vm --output-format json)
  python3 - "$VMID" "$HOSTNAME" "$CORES" "$MEMORY_MB" "$SWAP_MB" "$DISK_GB" "$STORAGE" "$TEMPLATE_STORAGE" "$BRIDGE" "$IP_CONFIG" "$GATEWAY" "$DNS" "$WHISTLE_LANGUAGE" "$ROOT_STORES" "$TEMPLATE_STORES" "$BRIDGES" "$resources" <<'PY'
import ipaddress, json, re, sys
v, host, cpu, ram, swap, disk, storage, template, bridge, network, gateway, dns, language, roots, templates, bridges, resources = sys.argv[1:]
def require(ok, message):
    if not ok:
        sys.exit('Invalid ' + message)
for value, low, high, name in ((v,100,999999999,'CTID'),(cpu,1,256,'CPU'),(ram,128,1048576,'RAM'),(swap,0,1048576,'swap'),(disk,4,65536,'disk')):
    require(re.fullmatch(r'[0-9]+', value) and low <= int(value) <= high, name)
require(not any(int(x.get('vmid', -1)) == int(v) for x in json.loads(resources)), 'CTID: VM or CT already exists')
require(len(host) <= 253 and all(re.fullmatch(r'[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?', label) for label in host.split('.')), 'hostname')
for value, choices, name in ((storage, roots,'root storage'),(template,templates,'template storage'),(bridge,bridges,'bridge')):
    require(value in choices.split() and re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9_-]*', value), name)
def address(value):
    try:
        ip = ipaddress.IPv4Address(value)
    except ValueError:
        sys.exit('Invalid IPv4 address')
    require(not (ip.is_loopback or ip.is_link_local or ip.is_multicast or ip.is_unspecified or int(ip) == 4294967295), 'IPv4 address')
    return ip
if network == 'dhcp':
    require(gateway == 'none', 'gateway: DHCP must use none')
else:
    try:
        require('/' in network, 'static CIDR')
        interface = ipaddress.IPv4Interface(network)
    except ValueError:
        sys.exit('Invalid static CIDR')
    ip = address(str(interface.ip))
    require(interface.network.prefixlen <= 30 and ip not in (interface.network.network_address, interface.network.broadcast_address), 'static host address/prefix')
    require(gateway != 'none', 'gateway: required for static networking')
    gw = address(gateway)
    require(gw in interface.network and gw != ip and gw not in (interface.network.network_address, interface.network.broadcast_address), 'gateway subnet')
if dns != 'inherit':
    address(dns)
require(language in ('fr','en','de','es','it','nl','pl','auto'), 'language')
PY
}
validate
if ((!UNATTENDED)); then
printf -v SUMMARY 'Debian 13; CTID=%s hostname=%s CPU=%s RAM=%s swap=%s disk=%s root=%s template=%s bridge=%s IP=%s gateway=%s DNS=%s language=%s unprivileged=1' \
  "$VMID" "$HOSTNAME" "$CORES" "$MEMORY_MB" "$SWAP_MB" "$DISK_GB" "$STORAGE" "$TEMPLATE_STORAGE" "$BRIDGE" "$IP_CONFIG" "$GATEWAY" "$DNS" "$WHISTLE_LANGUAGE"
ask CONFIRM "$SUMMARY"$'\nCreate and install? Type yes' no
[[ "$CONFIRM" == yes ]] || { echo 'Aborted; no changes made.'; exit 1; }
fi
phase template
pveam update
AVAILABLE=$(pveam available --section system)
TEMPLATE=$(python3 -c 'import re,sys; a=[x.split()[1] for x in sys.argv[1].splitlines() if len(x.split())==2 and re.fullmatch(r"debian-13-standard_[A-Za-z0-9.+_-]+_amd64\.tar\.(?:zst|gz|xz)", x.split()[1])]; print(sorted(a)[-1] if a else "")' "$AVAILABLE")
[[ -n "$TEMPLATE" ]] || { echo 'No Debian 13 standard template found.' >&2; failed; exit 1; }
template_exists() {
  local templates
  templates=$(pveam list "$TEMPLATE_STORAGE")
  python3 -c 'import sys; sys.exit(0 if any(x.split() and x.split()[0] == sys.argv[2] for x in sys.argv[1].splitlines()) else 1)' "$templates" "$TEMPLATE_STORAGE:vztmpl/$TEMPLATE"
}
if ! template_exists; then
  pveam download "$TEMPLATE_STORAGE" "$TEMPLATE"
fi
template_exists
phase create
# Recheck cluster-wide IDs immediately before create; pct also fails on races.
validate
NET="name=eth0,bridge=$BRIDGE,ip=$IP_CONFIG"
[[ "$GATEWAY" == none ]] || NET+=",gw=$GATEWAY"
DNS_ARGS=()
[[ "$DNS" == inherit ]] || DNS_ARGS=(--nameserver "$DNS")
pct create "$VMID" "$TEMPLATE_STORAGE:vztmpl/$TEMPLATE" --hostname "$HOSTNAME" \
  --cores "$CORES" --memory "$MEMORY_MB" --swap "$SWAP_MB" --rootfs "$STORAGE:$DISK_GB" \
  --net0 "$NET" --unprivileged 1 --onboot "$START_ON_BOOT" "${DNS_ARGS[@]}"
phase start
pct start "$VMID"
await() {
  local attempt
  for ((attempt=0; attempt<60; attempt++)); do
    if timeout 10 pct exec "$VMID" -- env LC_ALL=C LANG=C "$@" >/dev/null 2>&1; then return 0; fi
    sleep 2
  done
  echo "Timed out waiting for $PHASE." >&2
  return 1
}
phase readiness
await bash -ec 'ip -4 route show default | grep -q .; for repository in deb.debian.org security.debian.org; do address=$(getent ahostsv4 "$repository" | head -n 1 | cut -d " " -f 1); test -n "$address"; timeout 3 bash -c "exec 4<>/dev/tcp/$address/80"; done'
phase update
pct exec "$VMID" -- env LC_ALL=C LANG=C bash -ec 'export DEBIAN_FRONTEND=noninteractive; apt-get update -o APT::Update::Error-Mode=any -o Acquire::Retries=3 -o Acquire::http::Timeout=15 -o Acquire::https::Timeout=15; apt-get -y upgrade'
phase payload
SCRATCH=$(mktemp -d)
tar -C "$LXC_DIR" -czf "$SCRATCH/payload.tar.gz" server/app.py server/requirements.txt \
  server/web/index.html server/web/app.css server/web/app.js systemd/voice-notes-api.service scripts/install-server.sh
pct push "$VMID" "$SCRATCH/payload.tar.gz" /root/voice-notes-deploy.tar.gz
pct exec "$VMID" -- env LC_ALL=C LANG=C bash -c 'umask 077; mkdir -p /root/voice-notes-deploy; tar -xzf /root/voice-notes-deploy.tar.gz -C /root/voice-notes-deploy'
phase install
[[ "$WHISTLE_LANGUAGE" != auto ]] || WHISTLE_LANGUAGE=''
if [[ -n "$API_TOKEN" ]]; then
  printf 'export API_TOKEN=%q\n' "$API_TOKEN" > "$SCRATCH/installer.env"
  pct push "$VMID" "$SCRATCH/installer.env" /root/voice-notes-installer.env --perms 0600
  pct exec "$VMID" -- env LC_ALL=C LANG=C "WHISTLE_LANGUAGE=$WHISTLE_LANGUAGE" bash -c 'set +x; source /root/voice-notes-installer.env; trap "rm -f /root/voice-notes-installer.env" EXIT; bash /root/voice-notes-deploy/scripts/install-server.sh'
else
  pct exec "$VMID" -- env LC_ALL=C LANG=C "WHISTLE_LANGUAGE=$WHISTLE_LANGUAGE" bash /root/voice-notes-deploy/scripts/install-server.sh
fi
phase service
await systemctl is-active --quiet voice-notes-api.service
phase address
ADDRESSES=$(pct exec "$VMID" -- env LC_ALL=C LANG=C ip -4 -o addr show dev eth0 scope global)
IP=$(python3 -c 'import ipaddress,sys; a=[ipaddress.ip_interface(x.split()[x.split().index("inet")+1]).ip for x in sys.argv[1].splitlines() if "inet" in x.split()]; print(next(str(x) for x in a if not (x.is_loopback or x.is_link_local or x.is_unspecified or x.is_multicast)))' "$ADDRESSES")
phase health
await curl --fail --silent --show-error --max-time 5 "http://$IP:8080/health"
echo "LXC $VMID deployed and health verified. API URL: http://$IP:8080"
echo 'Token remains root-only in the CT at /etc/voice-notes.env (not printed).'
