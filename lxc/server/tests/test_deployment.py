"""Deployment syntax/config checks; never install or touch /etc in tests."""
import subprocess
import sys
from pathlib import Path

import pytest

LXC = Path(__file__).parents[2]


@pytest.mark.parametrize("script", sorted((LXC / "scripts").glob("*.sh")), ids=lambda p: p.name)
def test_shell_syntax(script):
    subprocess.run(["bash", "-n", str(script)], check=True)


@pytest.mark.parametrize('script', ['install-server.sh', 'update-server.sh'])
def test_deployment_copies_library_assets(script):
    source = (LXC / 'scripts' / script).read_text()
    assert 'install -d /opt/voice-notes/server/web' in source
    for asset in ['index.html', 'app.css', 'app.js']:
        assert f'cp "$LXC_DIR/server/web/{asset}" /opt/voice-notes/server/web/{asset}' in source


@pytest.mark.parametrize("language", ["en", "fr", ""])
def test_update_migrates_language_without_changing_token_or_data(tmp_path, language):
    # Exercise the actual embedded migration against a temporary synthetic file.
    source = (LXC / "scripts/update-server.sh").read_text()
    migration = source.split("/opt/voice-notes/venv/bin/python -c '\n", 1)[1].split("\n'\n", 1)[0]
    target = tmp_path / "test-config"
    original = f"VOICE_NOTES_API_TOKEN=test-only-token\nVOICE_NOTES_DATA_DIR=/custom/data\nWHISPER_MODEL=small\nWHISPER_LANGUAGE={language}\nMAX_UPLOAD_BYTES=12345\n"
    target.write_text(original)
    migration = migration.replace("/etc/voice-notes.env", str(target))
    subprocess.run([sys.executable, "-c", migration], check=True)
    result = target.read_text()
    assert "VOICE_NOTES_API_TOKEN=test-only-token\n" in result
    assert "VOICE_NOTES_DATA_DIR=/custom/data\n" in result
    assert "MAX_UPLOAD_BYTES=12345\n" in result
    assert f"WHISTLE_LANGUAGE={language}\n" in result
    assert "WHISPER_" not in result
    assert "HOME=/var/lib/voice-notes\n" in result
    assert target.stat().st_mode & 0o777 == 0o600
    subprocess.run([sys.executable, "-c", migration], check=True)
    assert target.read_text() == result
