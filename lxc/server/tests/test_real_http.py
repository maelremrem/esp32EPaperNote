"""Opt-in real uvicorn sockets + native Whistle; no ASGI/inference mocks."""
import hashlib
import io
import json
import os
import secrets
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import wave
from contextlib import contextmanager
from pathlib import Path

import httpx
import pytest

pytestmark = pytest.mark.skipif(os.getenv("RUN_REAL_WHISTLE") != "1", reason="set RUN_REAL_WHISTLE=1 for real native loopback HTTP")
SERVER = Path(__file__).parents[1]


def wav_body(pcm):
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(pcm)
    return output.getvalue()


@contextmanager
def real_server(directory, token):
    # A prebound loopback socket avoids port races and any LAN exposure.
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(128)
        port = listener.getsockname()[1]
        env = {**os.environ, "VOICE_NOTES_API_TOKEN": token,
               "VOICE_NOTES_DATA_DIR": str(directory / "data"),
               "WHISTLE_LANGUAGE": "fr", "MAX_UPLOAD_BYTES": "1100000",
               "NEEDLE_TELEMETRY": "0", "DO_NOT_TRACK": "1"}
        with (directory / "uvicorn.log").open("ab") as log:
            process = subprocess.Popen(
                [sys.executable, "-m", "uvicorn", "app:app", "--fd", str(listener.fileno()),
                 "--workers", "1", "--no-access-log"], cwd=SERVER, env=env,
                pass_fds=(listener.fileno(),), stdout=log, stderr=log)
            try:
                with httpx.Client(base_url=f"http://127.0.0.1:{port}", timeout=180, trust_env=False) as client:
                    deadline = time.monotonic() + 30
                    while True:
                        assert process.poll() is None, f"uvicorn exited; inspect {directory / 'uvicorn.log'}"
                        try:
                            response = client.get("/health", timeout=1)
                            if response.status_code == 200:
                                break
                        except httpx.TransportError:
                            pass
                        assert time.monotonic() < deadline, "uvicorn startup timeout"
                        time.sleep(0.05)
                    yield client, process.pid
            finally:
                process.terminate()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


def test_real_http_native_and_durable_storage():
    scratch = Path(os.environ["TMPDIR"])
    directory = Path(tempfile.mkdtemp(prefix="whistle-http-smoke-", dir=scratch))
    token = secrets.token_hex(32)
    auth = {"Authorization": "Bearer " + token}
    wav_headers = {**auth, "Content-Type": "audio/wav"}
    report = {"artifact_dir": str(directory), "engine": "cactus-needle==3.1.1", "checks": [], "server_pids": []}
    start = time.monotonic()

    def check(client, method, path, status, **kwargs):
        response = client.request(method, path, **kwargs)
        assert response.status_code == status, f"{method} {path}: {response.status_code} != {status}"
        report["checks"].append({"method": method, "path": path, "status": status})
        return response

    try:
        with real_server(directory, token) as (client, pid):
            report["server_pids"].append(pid)
            assert not check(client, "GET", "/health", 200).json()["model_loaded"]
            for path, method, body in [("/api/v1/live/silence", "POST", b"\0\0"),
                                       ("/api/v1/notes/final/transcribe", "POST", b"invalid"),
                                       ("/api/v1/notes/final", "GET", None),
                                       ("/api/v1/notes", "GET", None),
                                       ("/api/v1/notes/final/audio", "GET", None)]:
                check(client, method, path, 401, content=body)
                check(client, method, path, 403, content=body, headers={"Authorization": "Bearer wrong"})
            for body, status in [(b"", 400), (b"\0", 400), (b"\0" * 960002, 413)]:
                check(client, "POST", "/api/v1/live/bounds", status, content=body, headers=auth)
            # Chunked bounds also exercised over real sockets (no Content-Length).
            check(client, "POST", "/api/v1/live/chunked", 413,
                  content=iter([b"\0" * 480000, b"\0" * 480002]), headers=auth)
            check(client, "POST", "/api/v1/live/.invalid", 400, content=b"\0\0", headers=auth)
            check(client, "GET", "/api/v1/notes?limit=101", 422, headers=auth)
            check(client, "POST", "/api/v1/notes/bad/transcribe", 400, content=b"not-wav", headers=wav_headers)
            check(client, "POST", "/api/v1/notes/bad/transcribe", 415, content=b"bad", headers={**auth, "Content-Type": "text/plain"})
            check(client, "POST", "/api/v1/notes/oversized/transcribe", 413, content=b"\0" * 1100001, headers=wav_headers)
            check(client, "POST", "/api/v1/notes/.invalid/transcribe", 400, content=b"bad", headers=wav_headers)
            preview = check(client, "POST", "/api/v1/live/silence", 200, content=b"\0" * 64000, headers=auth).json()
            assert preview["status"] == "partial" and preview["text"] == "" and preview["language"] == "fr"
            assert check(client, "GET", "/health", 200).json()["model_loaded"]
            check(client, "GET", "/api/v1/notes/silence", 404, headers=auth)
            assert check(client, "GET", "/api/v1/notes", 200, headers=auth).json()["total"] == 0
            assert list((directory / "data/audio").iterdir()) == []
            edge = check(client, "POST", "/api/v1/live/exact30", 200, content=b"\0" * 960000, headers=auth).json()
            assert edge["text"] == ""
            body = wav_body(b"\0" * (31 * 16000 * 2))
            final = check(client, "POST", "/api/v1/notes/final/transcribe", 200, content=body, headers=wav_headers).json()
            assert final["status"] == "done" and final["text"] == "" and final["duration"] == 31
            assert final["sha256"] == hashlib.sha256(body).hexdigest()
            assert check(client, "GET", "/api/v1/notes/final", 200, headers=auth).json() == final
            assert check(client, "POST", "/api/v1/notes/final/transcribe", 200, content=body, headers=wav_headers).json() == final
            check(client, "POST", "/api/v1/notes/final/transcribe", 409, content=wav_body(b"\0" * 32000), headers=wav_headers)
            assert check(client, "GET", "/api/v1/notes/final", 200, headers=auth).json() == final
            assert check(client, "GET", "/api/v1/notes/final/audio", 200, headers=auth).content == body
            report["silence_final"] = final
            source = os.getenv("WHISTLE_TEST_WAV")
            if source:
                with wave.open(source, "rb") as wav:
                    assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (1, 2, 16000)
                    assert 0 < wav.getnframes() <= 30 * 16000
                    pcm = wav.readframes(wav.getnframes())
                preview = check(client, "POST", "/api/v1/live/speech", 200, content=pcm, headers=auth).json()
                assert preview["text"] and preview["language"] == "fr"
                speech = check(client, "POST", "/api/v1/notes/speech/transcribe", 200, content=Path(source).read_bytes(), headers=wav_headers).json()
                assert speech["text"] == preview["text"]
                assert check(client, "GET", "/api/v1/notes/speech", 200, headers=auth).json() == speech
                report["speech_source"] = str(Path(source).resolve())
                report["speech_final"] = speech
            else:
                report["speech"] = "not requested; no speech accuracy claim"
            # Exercise the existing curl smoke tool with the same ephemeral credentials.
            (directory / "silence.wav").write_bytes(wav_body(b"\0" * 64000))
            (directory / "silence.pcm").write_bytes(b"\0" * 64000)
            result = subprocess.run(["bash", str(SERVER.parents[0] / "scripts/test-api.sh"),
                                     str(directory / "silence.wav"), str(directory / "silence.pcm")],
                                    env={**os.environ, "API_URL": str(client.base_url).rstrip("/"), "API_TOKEN": token},
                                    capture_output=True, text=True, timeout=180)
            assert result.returncode == 0, "curl smoke failed (token withheld)"
            assert token not in result.stdout + result.stderr
            (directory / "curl-smoke.log").write_text(result.stdout + result.stderr)
            decoder = json.JSONDecoder()
            rest = result.stdout.lstrip()
            responses = []
            while rest:
                value, end = decoder.raw_decode(rest)
                responses.append(value)
                rest = rest[end:].lstrip()
            curl_final = responses[-1]
            assert curl_final["status"] == "done"
            assert check(client, "GET", "/api/v1/notes/" + curl_final["id"], 200, headers=auth).json() == curl_final
            assert check(client, "GET", "/api/v1/notes/" + curl_final["id"] + "/audio", 200, headers=auth).content == (directory / "silence.wav").read_bytes()
            report["curl_smoke"] = "passed with authenticated note/audio readback"
        assert (directory / "data/audio/final.wav").read_bytes() == body
        with sqlite3.connect(directory / "data/voice-notes.sqlite3") as conn:
            row = conn.execute("SELECT status,sha256,duration FROM notes WHERE note_id='final'").fetchone()
            assert row == ("done", final["sha256"], 31)
        # Restart without inference to verify on-disk persistence, not only process state.
        with real_server(directory, token) as (client, pid):
            report["server_pids"].append(pid)
            assert not check(client, "GET", "/health", 200).json()["model_loaded"]
            assert check(client, "GET", "/api/v1/notes/final", 200, headers=auth).json() == final
            assert check(client, "GET", "/api/v1/notes/final/audio", 200, headers=auth).content == body
        report["result"] = "passed"
    finally:
        report["elapsed_seconds"] = round(time.monotonic() - start, 3)
        report["servers_stopped"] = True
        (directory / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        print("REAL HTTP smoke artifacts:", directory)
    assert token not in (directory / "uvicorn.log").read_text()
    print("REAL HTTP smoke:", len(report["checks"]), "checks; speech:", report.get("speech_final", {}).get("text", "not requested"))
