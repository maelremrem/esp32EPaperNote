# Implementation checklist

## Hardware smoke test

- [ ] Confirm board is V2.
- [ ] FAT32 microSD detected.
- [ ] ePaper full refresh works and orientation is correct.
- [ ] GPIO1 application button works with internal pull-up.
- [ ] ES8311 detected on I2C at `0x18`.
- [ ] 16 kHz mono WAV contains clean microphone audio.
- [ ] Wi-Fi home network connects.
- [ ] iPhone hotspot fallback connects.

## Server

- [ ] LXC has a stable DHCP reservation.
- [ ] `GET /health` responds.
- [ ] API token is configured.
- [ ] First Whisper model download completes.
- [ ] Manual WAV transcription works with `scripts/test-api.sh`.
- [ ] systemd service starts after reboot.

## End-to-end

- [ ] Double click starts a note.
- [ ] Short click stops it.
- [ ] WAV appears in `audio/pending` before network access.
- [ ] Online note is transcribed and saved as Markdown.
- [ ] Original WAV moves to `audio/archive` after success.
- [ ] Offline notes remain pending after reboot.
- [ ] Pending queue syncs automatically when Wi-Fi returns.
- [ ] Re-sending a completed note ID does not duplicate it server-side.
