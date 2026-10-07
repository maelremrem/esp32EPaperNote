"""Opt-in integration checks: REAL native Whistle, no injected inference."""
import os
import wave
from pathlib import Path

import pytest
from needle import Whistle
from test_api import AUTH, api, client, wav_bytes

pytestmark = [pytest.mark.asyncio, pytest.mark.skipif(os.getenv("RUN_REAL_WHISTLE") != "1", reason="set RUN_REAL_WHISTLE=1 to download/run real Whistle")]


async def test_real_silence_preview_and_long_final(client, monkeypatch):
    # Lazy initialization exercises the real constructor/native downloader too.
    monkeypatch.setattr(api, "_model", None)
    preview = await client.post("/api/v1/live/real", content=b"\0" * 64000, headers=AUTH)
    assert preview.status_code == 200
    assert preview.json()["status"] == "partial"
    assert preview.json()["text"] == ""
    assert isinstance(api._model, Whistle)
    assert (await client.get("/api/v1/notes/real", headers=AUTH)).status_code == 404
    final = await client.post("/api/v1/notes/real/transcribe", content=wav_bytes(31 * 16000), headers={**AUTH, "Content-Type": "audio/wav"})
    assert final.status_code == 200
    assert final.json()["text"] == ""
    assert final.json()["duration"] == 31
    assert final.json()["model"] == "whistle"
    assert (await client.get("/api/v1/notes/real", headers=AUTH)).json() == final.json()


async def test_real_speech_file_through_both_endpoints(client, monkeypatch):
    source = os.getenv("WHISTLE_TEST_WAV")
    if not source:
        pytest.skip("set WHISTLE_TEST_WAV to a French PCM s16le mono 16000 WAV")
    assert source is not None
    monkeypatch.setattr(api, "_model", None)
    with wave.open(source, "rb") as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (1, 2, 16000)
        assert wav.getnframes() <= 30 * 16000
        pcm = wav.readframes(wav.getnframes())
    preview = await client.post("/api/v1/live/speech", content=pcm, headers=AUTH)
    assert preview.status_code == 200
    assert preview.json()["text"]
    assert preview.json()["language"] == "fr"
    final = await client.post("/api/v1/notes/speech/transcribe", content=Path(source).read_bytes(), headers={**AUTH, "Content-Type": "audio/wav"})
    assert final.status_code == 200
    assert final.json()["text"] == preview.json()["text"]
    print("REAL Whistle speech:", final.json()["text"])
