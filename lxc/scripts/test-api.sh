#!/usr/bin/env bash
set -euo pipefail

API_URL="${API_URL:-http://127.0.0.1:8080}"
API_TOKEN="${API_TOKEN:-}"
WAV_FILE="${1:-}"
PCM_FILE="${2:-}"
NOTE_ID="test-$(date +%Y%m%d-%H%M%S)"

curl -fsS "$API_URL/health" | python3 -m json.tool

# Optional second argument: nonoverlapping <=30s raw PCM16 mono/16k preview.
if [[ -n "$PCM_FILE" ]]; then
  if [[ -z "$API_TOKEN" ]]; then
    echo "Set API_TOKEN before testing live preview." >&2
    exit 1
  fi
  curl -fsS \
    -H "Authorization: Bearer ${API_TOKEN}" \
    -H "Content-Type: application/octet-stream" \
    --data-binary "@$PCM_FILE" \
    "$API_URL/api/v1/live/$NOTE_ID" | python3 -m json.tool
fi

if [[ -n "$WAV_FILE" ]]; then
  if [[ -z "$API_TOKEN" ]]; then
    echo "Set API_TOKEN before testing transcription." >&2
    exit 1
  fi

  curl -fsS \
    -H "Authorization: Bearer $API_TOKEN" \
    -H "Content-Type: audio/wav" \
    --data-binary "@$WAV_FILE" \
    "$API_URL/api/v1/notes/$NOTE_ID/transcribe" | python3 -m json.tool
fi
