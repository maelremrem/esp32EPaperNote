# LXC Whisper server

This directory contains both the API application and deployment scripts.

## Recommended LXC

- Debian 13
- 4 vCPU
- 4 GB RAM
- 1 GB swap
- 16 GB disk
- unprivileged container
- DHCP initially, then reserve its address in your DHCP server

The model cache is downloaded on first transcription. `small` is the default compromise for French notes on CPU. Change `WHISPER_MODEL` in `/etc/voice-notes.env` if needed.

## Deploy from a Proxmox host

```bash
cp lxc.env.example lxc.env
nano lxc.env
sudo ./scripts/deploy-proxmox.sh
```

## Install into an existing LXC

Copy this directory into the container, then:

```bash
sudo ./scripts/install-server.sh
```

## API

### Health

```http
GET /health
```

### Transcribe a WAV file

```http
POST /api/v1/notes/{note_id}/transcribe
Authorization: Bearer <token>
Content-Type: audio/wav

<raw WAV bytes>
```

Response:

```json
{
  "id": "20260915T220104-0043",
  "status": "done",
  "language": "fr",
  "duration": 11.2,
  "text": "Réunion projet demain matin.",
  "model": "small",
  "sha256": "...",
  "error": null
}
```

A completed `note_id` is idempotent: sending it again returns the existing transcription instead of creating another note.

## Systemd

```bash
systemctl status voice-notes-api
journalctl -u voice-notes-api -f
```

## Reverse proxy

For access outside the home network, expose only port 8080 through your HTTPS reverse proxy, keep the bearer token enabled, and apply a reasonable upload/rate limit. The firmware accepts a configurable HTTP or HTTPS API URL.
