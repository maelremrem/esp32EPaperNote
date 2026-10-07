"""Readiness regressions without root, services or model downloads."""
import re
import subprocess
from pathlib import Path

import pytest

LXC = Path(__file__).parents[2]


@pytest.mark.parametrize("filename", ["requirements.txt", "requirements-dev.txt"])
def test_direct_requirements_are_exactly_pinned(filename):
    lines = (LXC / "server" / filename).read_text().splitlines()
    dependencies = [line for line in lines if line and not line.startswith(("#", "-r"))]
    assert all(re.fullmatch(r"[A-Za-z0-9_\[\].-]+==[A-Za-z0-9_.+-]+", line) for line in dependencies)


def test_local_validation_help_does_not_need_dependencies_or_secrets():
    result = subprocess.run(["bash", str(LXC / "scripts/validate-local.sh"), "--help"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert "RUN_REAL_WHISTLE=1" in result.stdout


def test_installer_does_not_print_api_token():
    source = (LXC / "scripts/install-server.sh").read_text()
    assert 'API token: ${API_TOKEN}' not in source
