#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ! -f include/secrets.h ]]; then
  cp include/secrets.example.h include/secrets.h
  echo "Created include/secrets.h from template. Edit it before using networking."
fi
pio run
