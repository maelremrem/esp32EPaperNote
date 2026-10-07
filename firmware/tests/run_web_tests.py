#!/usr/bin/env python3
"""Credential-free host checks for the real web command/security policy."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
scratch = Path(os.environ.get('TMPDIR', str(Path.home() / '.hermes/cache/scratch')))
scratch.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='web-tests-', dir=scratch) as directory:
    binary = str(Path(directory) / 'web_test')
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    *os.environ.get('HOST_TEST_FLAGS', '').split(), '-I' + str(root / 'firmware/src'),
                    str(root / 'firmware/tests/web_test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
