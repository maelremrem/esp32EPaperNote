#!/usr/bin/env python3
"""Compile/run the real pure C++ preview helpers without ESP-IDF or credentials."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
scratch = Path(os.environ.get("TMPDIR", str(Path.home() / ".hermes/cache/scratch")))
scratch.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="live-preview-tests-", dir=scratch) as directory:
    binary = str(Path(directory) / "live_preview_test")
    flags = os.environ.get("HOST_TEST_FLAGS", "").split()
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    *flags, "-I" + str(root / "firmware/src"),
                    str(root / "firmware/tests/live_preview_test.cpp"), "-o", binary], check=True)
    subprocess.run([binary], check=True)
