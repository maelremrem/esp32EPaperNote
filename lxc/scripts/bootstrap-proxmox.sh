#!/usr/bin/env bash
# Standalone bootstrap: downloads this project's archive, never external helpers.
set -Eeuo pipefail
set +x
umask 077
REVISION=main
if [[ ${1:-} == --revision ]]; then
  [[ $# -ge 2 ]] || { echo 'Missing revision.' >&2; exit 2; }
  REVISION=$2
  shift 2
fi
[[ "$REVISION" == main || "$REVISION" =~ ^[a-fA-F0-9]{40}$ ]] || { echo 'Revision must be main or a full 40-character commit SHA.' >&2; exit 2; }
[[ $(id -u) == 0 ]] || { echo 'Run as root in a Proxmox host terminal.' >&2; exit 1; }
for command in curl python3 mktemp bash; do
  command -v "$command" >/dev/null || { echo "Missing command: $command" >&2; exit 1; }
done
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/voice-notes-bootstrap.XXXXXXXX")
trap 'rm -rf -- "$SCRATCH"' EXIT
trap 'echo "Bootstrap interrupted; any created CT is kept." >&2; exit 130' INT TERM
trap 'echo "Bootstrap failed; any created CT is kept. No automatic destruction." >&2' ERR
URL="https://github.com/maelremrem/esp32EPaperNote/archive/$REVISION.tar.gz"
[[ "$REVISION" != main ]] || URL='https://github.com/maelremrem/esp32EPaperNote/archive/refs/heads/main.tar.gz'
curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
  --connect-timeout 15 --max-time 120 --retry 2 --max-filesize 52428800 \
  --output "$SCRATCH/repository.tar.gz" "$URL"
ROOT=$(python3 - "$SCRATCH" <<'PY'
import pathlib, sys, tarfile
scratch = pathlib.Path(sys.argv[1])
with tarfile.open(scratch / 'repository.tar.gz', 'r:gz') as archive:
    members = archive.getmembers()
    if not members or len(members) > 20000 or sum(x.size for x in members) > 250 * 1024 * 1024:
        sys.exit('Invalid repository archive size')
    roots = set()
    names = set()
    for entry in members:
        path = pathlib.PurePosixPath(entry.name)
        if path.is_absolute() or '..' in path.parts or not path.parts or not (entry.isfile() or entry.isdir()) or entry.name in names:
            sys.exit('Unsafe repository archive member')
        names.add(entry.name)
        roots.add(path.parts[0])
    if len(roots) != 1:
        sys.exit('Expected one repository root')
    # All paths/types were checked before any write. Python 3.11 on PVE also works.
    archive.extractall(scratch / 'source', members=members)
root = scratch / 'source' / roots.pop()
for relative in ('lxc/scripts/deploy-proxmox.sh', 'lxc/scripts/install-server.sh',
                 'lxc/server/app.py', 'lxc/server/requirements.txt',
                 'lxc/server/web/index.html', 'lxc/server/web/app.css',
                 'lxc/server/web/app.js', 'lxc/systemd/voice-notes-api.service'):
    if not (root / relative).is_file():
        sys.exit('Incomplete repository: ' + relative)
print(root)
PY
)
# Keep bootstrap's EXIT trap alive until the local installer exits.
# Prompts use /dev/tty even when this bootstrap arrived through curl | bash.
if [[ ${1:-} == --unattended ]]; then
  bash "$ROOT/lxc/scripts/deploy-proxmox.sh" "$@"
else
  bash "$ROOT/lxc/scripts/deploy-proxmox.sh" "$@" </dev/tty
fi
