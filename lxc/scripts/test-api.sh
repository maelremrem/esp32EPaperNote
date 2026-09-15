#!/usr/bin/env bash
set -euo pipefail

API_URL="${API_URL:-http://127.0.0.1:8080}"
API_TOKEN="${API_TOKEN:-}"
WAV_FILE="${1:-}"

curl -fsS "$API_URL/health" | python3 -m json.tool

if [[ -n "$WAV_FILE" ]]; then
  if [[ -z "$API_TOKEN" ]]; then
    echo "Set API_TOKEN before testing transcription." >&2
    exit 1
  fi
  NOTE_ID="test-$(date +%Y%m%d-%H%M%S)"
  curl -fsS \
    -H "Authorization: Bearer $API_TOKEN" \
    -H "Content-Type: audio/wav" \
    --data-binary "@$WAV_FILE" \
    "$API_URL/api/v1/notes/$NOTE_ID/transcribe" | python3 -m json.tool
fi
