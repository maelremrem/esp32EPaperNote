#!/usr/bin/env bash
# Repository-only validation. Never installs services or modifies host configuration.
set -euo pipefail
if [[ "${1:-}" == "--help" ]]; then
  printf '%s\n' 'PYTHON=/scratch/venv/bin/python bash lxc/scripts/validate-local.sh' \
    'RUN_REAL_WHISTLE=1 enables native inference and loopback HTTP smoke (may download assets).' \
    'WHISTLE_TEST_WAV=/scratch/french.wav optionally checks real/synthetic speech.' \
    'TMPDIR must be an existing writable scratch directory; no token is printed.'
  exit 0
fi
if [[ $# -ne 0 ]]; then printf '%s\n' 'Only --help is supported.' >&2; exit 2; fi
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
: "${TMPDIR:?Set TMPDIR to an existing writable scratch directory}"
[[ -d "$TMPDIR" && -w "$TMPDIR" ]] || { printf '%s\n' 'TMPDIR is not writable.' >&2; exit 1; }
PYTHON="${PYTHON:-python3}"
export NEEDLE_TELEMETRY=0 DO_NOT_TRACK=1
"$PYTHON" -m pip check
for script in lxc/scripts/*.sh; do bash -n "$script"; done
# A glob deliberately excludes the optional Playwright browser runner.
"$PYTHON" -m pytest lxc/server/tests/test_*.py -q -s
node --test lxc/server/tests/test_web.cjs
