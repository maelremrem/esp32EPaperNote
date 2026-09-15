from __future__ import annotations

import asyncio
import hashlib
import os
import re
import secrets
import sqlite3
import tempfile
from pathlib import Path
from typing import Any

from fastapi import FastAPI, Header, HTTPException, Request
from fastapi.responses import JSONResponse
from faster_whisper import WhisperModel

APP_NAME = "ESP32 Voice Notes STT"
DATA_DIR = Path(os.getenv("VOICE_NOTES_DATA_DIR", "/var/lib/voice-notes"))
AUDIO_DIR = DATA_DIR / "audio"
DB_PATH = DATA_DIR / "voice-notes.sqlite3"
API_TOKEN = os.getenv("VOICE_NOTES_API_TOKEN", "")
MODEL_NAME = os.getenv("WHISPER_MODEL", "small")
MODEL_DEVICE = os.getenv("WHISPER_DEVICE", "cpu")
MODEL_COMPUTE_TYPE = os.getenv("WHISPER_COMPUTE_TYPE", "int8")
LANGUAGE = os.getenv("WHISPER_LANGUAGE", "fr") or None
MAX_UPLOAD_BYTES = int(os.getenv("MAX_UPLOAD_BYTES", str(64 * 1024 * 1024)))

NOTE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$")

app = FastAPI(title=APP_NAME, version="0.1.0")
_model: WhisperModel | None = None
_model_init_lock = asyncio.Lock()
_transcribe_lock = asyncio.Lock()


def db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_storage() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    AUDIO_DIR.mkdir(parents=True, exist_ok=True)
    with db() as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS notes (
                note_id TEXT PRIMARY KEY,
                status TEXT NOT NULL,
                sha256 TEXT,
                audio_path TEXT,
                language TEXT,
                duration REAL,
                text TEXT,
                model TEXT,
                error TEXT,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            )
            """
        )
        conn.commit()


@app.on_event("startup")
def startup() -> None:
    init_storage()
    if not API_TOKEN:
        raise RuntimeError("VOICE_NOTES_API_TOKEN is required")


def require_token(authorization: str | None) -> None:
    prefix = "Bearer "
    if not authorization or not authorization.startswith(prefix):
        raise HTTPException(status_code=401, detail="Missing bearer token")
    supplied = authorization[len(prefix) :]
    if not secrets.compare_digest(supplied, API_TOKEN):
        raise HTTPException(status_code=403, detail="Invalid bearer token")


async def get_model() -> WhisperModel:
    global _model
    if _model is not None:
        return _model
    async with _model_init_lock:
        if _model is None:
            _model = await asyncio.to_thread(
                WhisperModel,
                MODEL_NAME,
                device=MODEL_DEVICE,
                compute_type=MODEL_COMPUTE_TYPE,
            )
    return _model


def serialize_note(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "id": row["note_id"],
        "status": row["status"],
        "language": row["language"],
        "duration": row["duration"],
        "text": row["text"],
        "model": row["model"],
        "sha256": row["sha256"],
        "error": row["error"],
    }


async def save_request_body(request: Request, destination: Path) -> tuple[int, str]:
    hasher = hashlib.sha256()
    size = 0
    destination.parent.mkdir(parents=True, exist_ok=True)

    fd, temp_name = tempfile.mkstemp(prefix=destination.name + ".", suffix=".tmp", dir=destination.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            async for chunk in request.stream():
                size += len(chunk)
                if size > MAX_UPLOAD_BYTES:
                    raise HTTPException(status_code=413, detail="Audio file too large")
                out.write(chunk)
                hasher.update(chunk)
            out.flush()
            os.fsync(out.fileno())
        os.replace(temp_name, destination)
    except Exception:
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass
        raise

    if size < 44:
        destination.unlink(missing_ok=True)
        raise HTTPException(status_code=400, detail="Invalid or empty WAV file")
    return size, hasher.hexdigest()


def run_transcription(model: WhisperModel, audio_path: str) -> tuple[str, float, str | None]:
    segments, info = model.transcribe(
        audio_path,
        language=LANGUAGE,
        beam_size=5,
        vad_filter=True,
        condition_on_previous_text=True,
    )
    text = " ".join(segment.text.strip() for segment in segments if segment.text.strip()).strip()
    return text, float(info.duration or 0.0), info.language


@app.get("/health")
def health() -> dict[str, Any]:
    return {
        "status": "ok",
        "service": APP_NAME,
        "model": MODEL_NAME,
        "model_loaded": _model is not None,
    }


@app.get("/api/v1/notes/{note_id}")
def get_note(note_id: str, authorization: str | None = Header(default=None)) -> dict[str, Any]:
    require_token(authorization)
    if not NOTE_ID_RE.fullmatch(note_id):
        raise HTTPException(status_code=400, detail="Invalid note id")
    with db() as conn:
        row = conn.execute("SELECT * FROM notes WHERE note_id = ?", (note_id,)).fetchone()
    if row is None:
        raise HTTPException(status_code=404, detail="Note not found")
    return serialize_note(row)


@app.post("/api/v1/notes/{note_id}/transcribe")
async def transcribe(
    note_id: str,
    request: Request,
    authorization: str | None = Header(default=None),
) -> JSONResponse:
    require_token(authorization)
    if not NOTE_ID_RE.fullmatch(note_id):
        raise HTTPException(status_code=400, detail="Invalid note id")
    if request.headers.get("content-type", "").split(";", 1)[0].strip() not in {
        "audio/wav",
        "audio/x-wav",
        "application/octet-stream",
    }:
        raise HTTPException(status_code=415, detail="Expected WAV audio body")

    audio_path = AUDIO_DIR / f"{note_id}.wav"
    incoming_path = AUDIO_DIR / f"{note_id}.incoming"
    _, digest = await save_request_body(request, incoming_path)

    with db() as conn:
        existing = conn.execute("SELECT * FROM notes WHERE note_id = ?", (note_id,)).fetchone()
        if existing is not None and existing["status"] == "done":
            incoming_path.unlink(missing_ok=True)
            if existing["sha256"] and existing["sha256"] != digest:
                raise HTTPException(status_code=409, detail="note_id already exists with different audio")
            return JSONResponse(serialize_note(existing), status_code=200)

    os.replace(incoming_path, audio_path)

    with db() as conn:
        conn.execute(
            """
            INSERT INTO notes (note_id, status, sha256, audio_path, model)
            VALUES (?, 'processing', ?, ?, ?)
            ON CONFLICT(note_id) DO UPDATE SET
                status='processing', sha256=excluded.sha256,
                audio_path=excluded.audio_path, model=excluded.model,
                error=NULL, updated_at=CURRENT_TIMESTAMP
            """,
            (note_id, digest, str(audio_path), MODEL_NAME),
        )
        conn.commit()

    try:
        model = await get_model()
        async with _transcribe_lock:
            text, duration, detected_language = await asyncio.to_thread(
                run_transcription, model, str(audio_path)
            )
        language = detected_language or LANGUAGE
        with db() as conn:
            conn.execute(
                """
                UPDATE notes
                SET status='done', text=?, duration=?, language=?, error=NULL,
                    updated_at=CURRENT_TIMESTAMP
                WHERE note_id=?
                """,
                (text, duration, language, note_id),
            )
            conn.commit()
            row = conn.execute("SELECT * FROM notes WHERE note_id = ?", (note_id,)).fetchone()
        return JSONResponse(serialize_note(row), status_code=200)
    except Exception as exc:
        with db() as conn:
            conn.execute(
                "UPDATE notes SET status='error', error=?, updated_at=CURRENT_TIMESTAMP WHERE note_id=?",
                (str(exc)[:1000], note_id),
            )
            conn.commit()
        raise HTTPException(status_code=500, detail="Transcription failed") from exc
