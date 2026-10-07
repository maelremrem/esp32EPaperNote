from __future__ import annotations

import asyncio
import array
import hashlib
import os
import re
import secrets
import sqlite3
import sys
import tempfile
import wave
from pathlib import Path
from contextlib import asynccontextmanager
from typing import Any

from fastapi import FastAPI, Header, HTTPException, Request, Query
from fastapi.responses import JSONResponse, FileResponse
from needle import Whistle

APP_NAME = "ESP32 Voice Notes STT"
DATA_DIR = Path(os.getenv("VOICE_NOTES_DATA_DIR", "/var/lib/voice-notes"))
AUDIO_DIR = DATA_DIR / "audio"
DB_PATH = DATA_DIR / "voice-notes.sqlite3"
API_TOKEN = os.getenv("VOICE_NOTES_API_TOKEN", "")
MODEL_NAME = "whistle"
MODEL_WEIGHTS = os.getenv("NEEDLE_WHISTLE_WEIGHTS") or None
LANGUAGE = os.getenv("WHISTLE_LANGUAGE", "fr") or None
PCM_RATE = 16000
MAX_WINDOW_BYTES = 30 * PCM_RATE * 2
MAX_UPLOAD_BYTES = int(os.getenv("MAX_UPLOAD_BYTES", str(64 * 1024 * 1024)))

NOTE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$")


@asynccontextmanager
async def lifespan(app: FastAPI):
    startup()
    yield


app = FastAPI(title=APP_NAME, version="0.2.0", lifespan=lifespan)
_model: Whistle | None = None
_model_init_lock = asyncio.Lock()
_transcribe_lock = asyncio.Lock()
_finalize_lock = asyncio.Lock()


async def run_blocking(function, *args, **kwargs):
    """Do not release an engine lock while a cancelled native call still runs."""
    task = asyncio.create_task(asyncio.to_thread(function, *args, **kwargs))
    cancelled = False
    while True:
        try:
            result = await asyncio.shield(task)
            break
        except asyncio.CancelledError:
            if task.cancelled():
                raise
            cancelled = True
    if cancelled:
        raise asyncio.CancelledError
    return result


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


async def get_model() -> Whistle:
    global _model
    if _model is not None:
        return _model
    async with _model_init_lock:
        if _model is None:
            _model = await run_blocking(
                Whistle, weights=MODEL_WEIGHTS,
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
        "created_at": row["created_at"],
        "updated_at": row["updated_at"],
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
        await run_blocking(validate_wav, temp_name)
        os.replace(temp_name, destination)
    except BaseException:
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass
        raise

    return size, hasher.hexdigest()


def validate_wav(path: str) -> None:
    try:
        with wave.open(path, "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) != (1, 2, PCM_RATE, "NONE"):
                raise ValueError("Expected PCM s16le mono 16000 WAV")
            expected = wav.getnframes() * 2
            actual = 0
            while pcm := wav.readframes(MAX_WINDOW_BYTES // 2):
                actual += len(pcm)
            if not actual or actual != expected:
                raise ValueError("Empty or truncated WAV")
    except (wave.Error, EOFError, ValueError) as exc:
        raise HTTPException(status_code=400, detail="Invalid WAV: expected PCM s16le mono 16000") from exc


def run_transcription(model: Whistle, audio_path: str) -> tuple[str, float, str | None]:
    texts = []
    language = LANGUAGE
    with wave.open(audio_path, "rb") as wav:
        duration = wav.getnframes() / PCM_RATE
        while pcm := wav.readframes(MAX_WINDOW_BYTES // 2):
            result = model.transcribe(pcm_samples(pcm), language=LANGUAGE)
            if result["text"].strip():
                texts.append(result["text"].strip())
            language = result.get("language") or language
    return " ".join(texts), duration, language


@app.get("/health")
def health() -> dict[str, Any]:
    return {
        "status": "ok",
        "service": APP_NAME,
        "model": MODEL_NAME,
        "model_loaded": _model is not None,
    }


def pcm_samples(pcm: bytes | bytearray) -> array.array:
    samples = array.array("h")
    samples.frombytes(pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    return array.array("f", (value / 32768.0 for value in samples))


@app.post("/api/v1/live/{note_id}")
async def live(note_id: str, request: Request, authorization: str | None = Header(default=None)) -> dict[str, Any]:
    require_token(authorization)
    if not NOTE_ID_RE.fullmatch(note_id):
        raise HTTPException(status_code=400, detail="Invalid note id")
    pcm = bytearray()
    async for chunk in request.stream():
        if len(pcm) + len(chunk) > MAX_WINDOW_BYTES:
            raise HTTPException(status_code=413, detail="PCM window exceeds 30 seconds")
        pcm.extend(chunk)
    if not pcm or len(pcm) % 2:
        raise HTTPException(status_code=400, detail="Expected nonempty PCM s16le mono 16000")
    try:
        async with _transcribe_lock:
            model = await get_model()
            result = await run_blocking(lambda: model.transcribe(pcm_samples(pcm), language=LANGUAGE))
    except Exception as exc:
        raise HTTPException(status_code=500, detail="Transcription failed") from exc
    return {"id": note_id, "status": "partial", "text": result["text"].strip(),
            "language": result.get("language") or LANGUAGE, "model": MODEL_NAME}


class PrivateResponses:
    """Pure ASGI: preserve native-call cancellation and streaming semantics."""
    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        async def with_headers(message):
            if message["type"] == "http.response.start":
                message.setdefault("headers", []).extend([
                    (b"cache-control", b"no-store"),
                    (b"x-content-type-options", b"nosniff"),
                ])
            await send(message)
        await self.app(scope, receive, with_headers)


app.add_middleware(PrivateResponses)


WEB_DIR = Path(__file__).parent / "web"
WEB_CSP = (
    "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
    "media-src blob:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'"
)


@app.get("/", include_in_schema=False)
def library_shell() -> FileResponse:
    return FileResponse(WEB_DIR / "index.html", media_type="text/html", headers={
        "Content-Security-Policy": WEB_CSP, "Referrer-Policy": "no-referrer",
    })


@app.get("/assets/{filename}", include_in_schema=False)
def library_asset(filename: str) -> FileResponse:
    mime = {"app.js": "text/javascript", "app.css": "text/css"}.get(filename)
    if mime is None:
        raise HTTPException(status_code=404, detail="Asset not found")
    return FileResponse(WEB_DIR / filename, media_type=mime)


@app.get("/api/v1/notes")
def list_notes(
    authorization: str | None = Header(default=None),
    limit: int = Query(default=30, ge=1, le=100),
    offset: int = Query(default=0, ge=0, le=1_000_000),
    q: str = Query(default="", max_length=200),
    status: str | None = Query(default=None, pattern=r"^(done|processing|error)$"),
) -> dict[str, Any]:
    require_token(authorization)
    clauses, params = [], []
    if status:
        clauses.append("status = ?")
        params.append(status)
    if q:
        # instr performs literal matching: user %/_ characters are not wildcards.
        clauses.append("(instr(lower(note_id), lower(?)) > 0 OR instr(lower(coalesce(text, '')), lower(?)) > 0)")
        params.extend([q, q])
    where = " WHERE " + " AND ".join(clauses) if clauses else ""
    with db() as conn:
        total = conn.execute("SELECT COUNT(*) FROM notes" + where, params).fetchone()[0]
        rows = conn.execute("SELECT * FROM notes" + where + " ORDER BY created_at DESC, note_id DESC LIMIT ? OFFSET ?", [*params, limit, offset]).fetchall()
    return {"items": [serialize_note(row) for row in rows], "total": total, "limit": limit, "offset": offset}


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


@app.get("/api/v1/notes/{note_id}/audio")
def get_audio(note_id: str, authorization: str | None = Header(default=None)) -> FileResponse:
    require_token(authorization)
    if not NOTE_ID_RE.fullmatch(note_id):
        raise HTTPException(status_code=400, detail="Invalid note id")
    with db() as conn:
        row = conn.execute("SELECT audio_path FROM notes WHERE note_id = ?", (note_id,)).fetchone()
    if row is None or not row["audio_path"]:
        raise HTTPException(status_code=404, detail="Audio not found")
    try:
        path = Path(row["audio_path"]).resolve(strict=True)
        path.relative_to(AUDIO_DIR.resolve())
        if not path.is_file() or path.suffix.lower() != ".wav":
            raise ValueError("Not a WAV file")
    except (OSError, ValueError, RuntimeError):
        raise HTTPException(status_code=404, detail="Audio not found") from None
    return FileResponse(path, media_type="audio/wav", filename=f"{note_id}.wav")


@app.post("/api/v1/notes/{note_id}/transcribe")
async def transcribe(
    note_id: str,
    request: Request,
    authorization: str | None = Header(default=None),
) -> JSONResponse:
    require_token(authorization)
    # One finalizer also prevents concurrent uploads overwriting the same WAV.
    async with _finalize_lock:
        return await finalize_note(note_id, request)


async def finalize_note(
    note_id: str,
    request: Request,
) -> JSONResponse:
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
        async with _transcribe_lock:
            model = await get_model()
            text, duration, detected_language = await run_blocking(
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
