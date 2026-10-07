"""Contract tests use an injected engine, never real speech inference."""
import asyncio
import importlib.util
import io
import hashlib
import wave
import threading
from pathlib import Path

import httpx
import pytest
import pytest_asyncio

spec = importlib.util.spec_from_file_location("voice_notes_app", Path(__file__).parents[1] / "app.py")
assert spec is not None and spec.loader is not None
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)
AUTH = {"Authorization": "Bearer test-token"}


@pytest.mark.asyncio
async def test_library_lists_real_notes_with_totals_and_timestamps(client):
    with api.db() as conn:
        conn.execute("INSERT INTO notes(note_id,status,text) VALUES('a','done','Bonjour')")
    assert (await client.get('/api/v1/notes')).status_code == 401
    assert (await client.get('/api/v1/notes', headers={'Authorization': 'Bearer wrong'})).status_code == 403
    response = await client.get('/api/v1/notes', headers=AUTH)
    assert response.status_code == 200
    result = response.json()
    assert result['total'] == 1
    assert result['limit'] == 30 and result['offset'] == 0
    assert result['items'][0]['id'] == 'a'
    assert result['items'][0]['created_at']
    assert result['items'][0]['updated_at']
    assert 'audio_path' not in result['items'][0]
    assert response.headers['cache-control'] == 'no-store'


@pytest.mark.asyncio
async def test_library_pagination_literal_search_and_status(client):
    # Preserve accented French fixtures to verify multilingual literal searches.
    with api.db() as conn:
        conn.executemany("INSERT INTO notes(note_id,status,text,created_at) VALUES(?,?,?,?)", [
            ('a','done','réunion 100%','2026-01-01 10:00:00'),
            ('b','error','réunion','2026-01-02 10:00:00'),
            ('c','done','réunion','2026-01-03 10:00:00')])
    result = (await client.get('/api/v1/notes?status=done&q=réunion&limit=1&offset=1', headers=AUTH)).json()
    assert result['total'] == 2 and result['limit'] == 1 and result['offset'] == 1
    assert [note['id'] for note in result['items']] == ['a']
    result = (await client.get('/api/v1/notes?q=%25', headers=AUTH)).json()
    assert result['total'] == 1
    assert (await client.get('/api/v1/notes?q=a', headers=AUTH)).json()['items'][0]['id'] == 'a'
    assert (await client.get('/api/v1/notes?offset=999', headers=AUTH)).json()['items'] == []


@pytest.mark.asyncio
@pytest.mark.parametrize('query', ['limit=0','limit=101','limit=no','offset=-1','offset=1000001','status=partial','q=' + 'a'*201])
async def test_library_validates_bounds(client, query):
    assert (await client.get('/api/v1/notes?' + query, headers=AUTH)).status_code == 422


@pytest.mark.asyncio
async def test_audio_is_authenticated_database_bound_wav(client):
    body = wav_bytes(20)
    await client.post('/api/v1/notes/audio/transcribe', content=body, headers={**AUTH, 'Content-Type':'audio/wav'})
    url = '/api/v1/notes/audio/audio'
    assert (await client.get(url)).status_code == 401
    assert (await client.get(url, headers={'Authorization':'Bearer wrong'})).status_code == 403
    response = await client.get(url, headers=AUTH)
    assert response.status_code == 200 and response.content == body
    assert response.headers['content-type'] == 'audio/wav'
    assert 'audio.wav' in response.headers['content-disposition']
    assert response.headers['cache-control'] == 'no-store'
    assert (await client.get('/api/v1/notes/unknown/audio', headers=AUTH)).status_code == 404
    assert (await client.get('/api/v1/notes/.invalid/audio', headers=AUTH)).status_code == 400
    (api.AUDIO_DIR / 'audio.wav').unlink()
    assert (await client.get(url, headers=AUTH)).status_code == 404


@pytest.mark.asyncio
@pytest.mark.parametrize('kind', ['outside','traversal','symlink','null','directory'])
async def test_audio_rejects_unsafe_stored_path(client, tmp_path, kind):
    outside = tmp_path / 'outside.wav'
    outside.write_bytes(b'private')
    link = api.AUDIO_DIR / 'link.wav'
    link.symlink_to(outside)
    paths = {'outside':str(outside), 'traversal':str(api.AUDIO_DIR / '../outside.wav'),
             'symlink':str(link), 'null':None, 'directory':str(api.AUDIO_DIR)}
    with api.db() as conn:
        conn.execute("INSERT INTO notes(note_id,status,audio_path) VALUES('unsafe','done',?)", (paths[kind],))
    assert (await client.get('/api/v1/notes/unsafe/audio', headers=AUTH)).status_code == 404


@pytest.mark.asyncio
async def test_public_library_shell_is_safe_and_assets_are_bounded(client):
    with api.db() as conn:
        conn.execute("INSERT INTO notes(note_id,status,text) VALUES('secret-note','done','private transcript')")
    response = await client.get('/')
    assert response.status_code == 200
    assert response.headers['content-type'].startswith('text/html')
    assert 'Carnet' in response.text and 'lang="en"' in response.text
    assert 'type="password"' in response.text
    assert 'private transcript' not in response.text and 'test-token' not in response.text
    csp = response.headers['content-security-policy']
    assert "default-src 'none'" in csp and "script-src 'self'" in csp
    assert "frame-ancestors 'none'" in csp and 'unsafe-inline' not in csp
    for filename, mime in [('app.js','text/javascript'),('app.css','text/css')]:
        asset = await client.get('/assets/' + filename)
        assert asset.status_code == 200 and asset.headers['content-type'].startswith(mime)
        assert asset.headers['x-content-type-options'] == 'nosniff'
    source = (await client.get('/assets/app.js')).text
    assert 'textContent' in source and 'AbortController' in source
    assert 'localStorage' not in source and 'sessionStorage' not in source and 'innerHTML' not in source
    assert 'Authorization' in source and 'revokeObjectURL' in source
    for path in ['/assets/app.py','/assets/.env','/assets/%2e%2e/app.py','/assets/not-found','/.env']:
        assert (await client.get(path)).status_code == 404


class InjectedEngine:
    # French output is intentional: English UI labels must not change STT language.
    def __init__(self):
        self.calls = []

    def transcribe(self, audio, language=None):
        self.calls.append((list(audio), language))
        return {"text": " Bonjour. ", "language": "fr"}


@pytest_asyncio.fixture
async def client(tmp_path, monkeypatch):
    monkeypatch.setattr(api, "DATA_DIR", tmp_path)
    monkeypatch.setattr(api, "AUDIO_DIR", tmp_path / "audio")
    monkeypatch.setattr(api, "DB_PATH", tmp_path / "notes.sqlite3")
    monkeypatch.setattr(api, "API_TOKEN", "test-token")
    monkeypatch.setattr(api, "_model", InjectedEngine())
    monkeypatch.setattr(api, "_model_init_lock", asyncio.Lock())
    monkeypatch.setattr(api, "_transcribe_lock", asyncio.Lock())
    monkeypatch.setattr(api, "_finalize_lock", asyncio.Lock())
    api.init_storage()
    async with httpx.AsyncClient(transport=httpx.ASGITransport(app=api.app), base_url="http://test") as c:
        yield c


@pytest.mark.asyncio
async def test_live_pcm_returns_partial_without_persistence(client):
    response = await client.post("/api/v1/live/note-1", content=b"\x00\x80\xff\x7f", headers=AUTH)
    assert response.status_code == 200
    assert response.json() == {"id": "note-1", "status": "partial", "text": "Bonjour.", "language": "fr", "model": "whistle"}
    assert api._model.calls == [([-1.0, 32767 / 32768], "fr")]
    assert (await client.get("/api/v1/notes/note-1", headers=AUTH)).status_code == 404
    assert list(api.AUDIO_DIR.iterdir()) == []


@pytest.mark.asyncio
@pytest.mark.parametrize("body,code", [(b"", 400), (b"\0", 400), (b"\0" * 960002, 413)], ids=["empty", "odd", "too-long"])
async def test_live_rejects_invalid_pcm_bounds(client, body, code):
    response = await client.post("/api/v1/live/note-1", content=body, headers=AUTH)
    assert response.status_code == code
    assert api._model.calls == []


@pytest.mark.asyncio
async def test_live_accepts_exactly_thirty_seconds(client):
    response = await client.post("/api/v1/live/note-1", content=b"\0" * 960000, headers=AUTH)
    assert response.status_code == 200
    assert len(api._model.calls[0][0]) == 480000


@pytest.mark.asyncio
@pytest.mark.parametrize("headers,code", [({}, 401), ({"Authorization": "Bearer wrong"}, 403)])
async def test_live_requires_token(client, headers, code):
    assert (await client.post("/api/v1/live/note-1", content=b"\0\0", headers=headers)).status_code == code
    assert api._model.calls == []


@pytest.mark.asyncio
async def test_live_rejects_invalid_note_id(client):
    response = await client.post("/api/v1/live/.invalid", content=b"\0\0", headers=AUTH)
    assert response.status_code == 400


def wav_bytes(frames=32000, rate=16000, channels=1, width=2):
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(width)
        wav.setframerate(rate)
        wav.writeframes(b"\0" * frames * channels * width)
    return output.getvalue()


@pytest.mark.asyncio
@pytest.mark.parametrize("frames,lengths", [(32000, [32000]), (960000 + 1, [480000, 480000, 1])])
async def test_final_wav_is_windowed_persisted_and_idempotent(client, frames, lengths):
    body = wav_bytes(frames)
    headers = {**AUTH, "Content-Type": "audio/wav"}
    url = "/api/v1/notes/final-1/transcribe"
    response = await client.post(url, content=body, headers=headers)
    assert response.status_code == 200
    result = response.json()
    assert result["status"] == "done"
    assert result["text"] == " ".join(["Bonjour."] * len(lengths))
    assert result["duration"] == frames / 16000
    assert result["model"] == "whistle"
    assert result["sha256"] == hashlib.sha256(body).hexdigest()
    assert [len(call[0]) for call in api._model.calls] == lengths
    assert (api.AUDIO_DIR / "final-1.wav").read_bytes() == body
    assert (await client.get("/api/v1/notes/final-1", headers=AUTH)).json() == result
    assert (await client.post(url, content=body, headers=headers)).json() == result
    assert len(api._model.calls) == len(lengths)
    conflict = await client.post(url, content=wav_bytes(20), headers=headers)
    assert conflict.status_code == 409
    assert (api.AUDIO_DIR / "final-1.wav").read_bytes() == body


@pytest.mark.asyncio
@pytest.mark.parametrize("body", [b"not a wav" * 10, wav_bytes(0), wav_bytes(rate=8000), wav_bytes(channels=2), wav_bytes(width=1), wav_bytes()[:-1]], ids=["invalid", "empty", "rate", "stereo", "width", "truncated"])
async def test_final_rejects_invalid_wav_before_persistence(client, body):
    response = await client.post("/api/v1/notes/invalid/transcribe", content=body, headers={**AUTH, "Content-Type": "audio/wav"})
    assert response.status_code == 400
    assert api._model.calls == []
    assert (await client.get("/api/v1/notes/invalid", headers=AUTH)).status_code == 404
    assert list(api.AUDIO_DIR.iterdir()) == []


class BlockingEngine(InjectedEngine):
    def __init__(self):
        super().__init__()
        self.entered = threading.Event()
        self.release = threading.Event()
        self.active = 0
        self.max_active = 0

    def transcribe(self, audio, language=None):
        self.active += 1
        self.max_active = max(self.max_active, self.active)
        self.entered.set()
        try:
            assert self.release.wait(5), "test engine was not released"
            return super().transcribe(audio, language)
        finally:
            self.active -= 1


@pytest.mark.asyncio
async def test_cancelled_live_keeps_engine_serialized_and_loop_responsive(client, monkeypatch):
    engine = BlockingEngine()
    monkeypatch.setattr(api, "_model", engine)
    first = asyncio.create_task(client.post("/api/v1/live/a", content=b"\0\0", headers=AUTH))
    second = None
    try:
        assert await asyncio.to_thread(engine.entered.wait, 2)
        first.cancel()
        second = asyncio.create_task(client.post("/api/v1/live/b", content=b"\0\0", headers=AUTH))
        health = await asyncio.wait_for(client.get("/health"), 0.5)
        assert health.status_code == 200
        await asyncio.sleep(0.05)
        assert engine.max_active == 1
        assert not second.done()
    finally:
        engine.release.set()
        await asyncio.gather(first, return_exceptions=True)
        if second is not None:
            await second


@pytest.mark.asyncio
async def test_concurrent_same_note_finalizes_once(client, monkeypatch):
    engine = BlockingEngine()
    monkeypatch.setattr(api, "_model", engine)
    body = wav_bytes()
    headers = {**AUTH, "Content-Type": "audio/wav"}
    first = asyncio.create_task(client.post("/api/v1/notes/same/transcribe", content=body, headers=headers))
    try:
        assert await asyncio.to_thread(engine.entered.wait, 2)
        second = asyncio.create_task(client.post("/api/v1/notes/same/transcribe", content=body, headers=headers))
        await asyncio.sleep(0.05)
    finally:
        engine.release.set()
    responses = await asyncio.gather(first, second)
    assert [r.status_code for r in responses] == [200, 200]
    assert responses[0].json() == responses[1].json()
    assert len(engine.calls) == 1


@pytest.mark.asyncio
async def test_live_engine_failure_returns_generic_500_without_note(client, monkeypatch):
    def fail(*args, **kwargs):
        raise RuntimeError("internal engine path")
    monkeypatch.setattr(api._model, "transcribe", fail)
    response = await client.post("/api/v1/live/fail", content=b"\0\0", headers=AUTH)
    assert response.status_code == 500
    assert response.json() == {"detail": "Transcription failed"}
    assert (await client.get("/api/v1/notes/fail", headers=AUTH)).status_code == 404


@pytest.mark.asyncio
async def test_final_failure_persists_error_and_can_be_retried(client, monkeypatch):
    def fail(*args, **kwargs):
        raise RuntimeError("engine failure")
    monkeypatch.setattr(api._model, "transcribe", fail)
    body = wav_bytes()
    headers = {**AUTH, "Content-Type": "audio/wav"}
    url = "/api/v1/notes/retry/transcribe"
    assert (await client.post(url, content=body, headers=headers)).status_code == 500
    note = (await client.get("/api/v1/notes/retry", headers=AUTH)).json()
    assert note["status"] == "error"
    monkeypatch.setattr(api, "_model", InjectedEngine())
    assert (await client.post(url, content=body, headers=headers)).json()["status"] == "done"
    preview = await client.post("/api/v1/live/retry", content=b"\0\0", headers=AUTH)
    assert preview.json()["status"] == "partial"
    assert (await client.get("/api/v1/notes/retry", headers=AUTH)).json()["status"] == "done"


@pytest.mark.asyncio
async def test_final_upload_limit_cleans_temporary_file(client, monkeypatch):
    monkeypatch.setattr(api, "MAX_UPLOAD_BYTES", 44)
    response = await client.post("/api/v1/notes/large/transcribe", content=wav_bytes(), headers={**AUTH, "Content-Type": "audio/wav"})
    assert response.status_code == 413
    assert list(api.AUDIO_DIR.iterdir()) == []


@pytest.mark.asyncio
async def test_cancelled_upload_cleans_temporary_file(client):
    received = asyncio.Event()
    async def body():
        yield wav_bytes()[:44]
        received.set()
        await asyncio.Event().wait()
    request = asyncio.create_task(client.post("/api/v1/notes/cancel/transcribe", content=body(), headers={**AUTH, "Content-Type": "audio/wav"}))
    await asyncio.wait_for(received.wait(), 1)
    request.cancel()
    await asyncio.gather(request, return_exceptions=True)
    assert list(api.AUDIO_DIR.iterdir()) == []


@pytest.mark.asyncio
async def test_engine_initialization_runs_off_event_loop_once(client, monkeypatch):
    engine = InjectedEngine()
    main_thread = threading.get_ident()
    calls = []
    def factory(weights=None):
        calls.append(threading.get_ident())
        return engine
    monkeypatch.setattr(api, "Whistle", factory)
    monkeypatch.setattr(api, "_model", None)
    responses = await asyncio.gather(*(client.post("/api/v1/live/initialize", content=b"\0\0", headers=AUTH) for _ in range(2)))
    assert [r.status_code for r in responses] == [200, 200]
    assert len(calls) == 1
    assert calls[0] != main_thread


@pytest.mark.asyncio
async def test_lifespan_requires_token(client, monkeypatch):
    monkeypatch.setattr(api, "API_TOKEN", "")
    with pytest.raises(RuntimeError, match="VOICE_NOTES_API_TOKEN is required"):
        async with api.app.router.lifespan_context(api.app):
            pass


@pytest.mark.asyncio
async def test_live_chunked_body_checks_total_not_chunk_alignment(client):
    async def chunks():
        yield b"\0"
        yield b"\0\0"
        yield b"\0"
    response = await client.post("/api/v1/live/chunked", content=chunks(), headers=AUTH)
    assert response.status_code == 200
    assert api._model.calls == [([0.0, 0.0], "fr")]


@pytest.mark.asyncio
async def test_live_chunked_body_stops_reading_at_limit(client):
    async def chunks():
        yield b"\0" * 960000
        yield b"\0\0"
        raise AssertionError("must not consume past the size limit")
    response = await client.post("/api/v1/live/chunked", content=chunks(), headers=AUTH)
    assert response.status_code == 413
    assert api._model.calls == []
