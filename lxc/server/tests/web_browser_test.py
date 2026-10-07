#!/usr/bin/env python3
"""Browser QA against the real API and a disposable SQLite fixture (no inference)."""
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import urllib.request
import wave

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[3]
SCRATCH = Path(os.environ.get('TMPDIR', Path.home() / '.hermes/cache/scratch'))
OUTPUT = Path(os.environ.get('WEB_QA_OUTPUT', SCRATCH / 'carnet-web-qa'))
OUTPUT.mkdir(parents=True, exist_ok=True)
API_PYTHON = os.environ.get('WEB_API_PYTHON', 'python3')
errors = []


def check_layout(page, width):
    page.set_viewport_size({'width': width, 'height': 1000})
    result = page.evaluate('''() => ({scroll: document.documentElement.scrollWidth,
      overflow: [...document.querySelectorAll('header *,main *,footer *')]
        .filter(e => e.getBoundingClientRect().width && (e.getBoundingClientRect().right > innerWidth + 1 || e.getBoundingClientRect().left < -1))
        .map(e => e.id || e.tagName),
      buttons: [...document.querySelectorAll('button:not([hidden]),a.button:not([hidden])')]
        .filter(e=>e.getBoundingClientRect().width)
        .map(e=>({id:e.id,height:e.getBoundingClientRect().height,scroll:e.scrollWidth,width:e.clientWidth}))})''')
    assert result['scroll'] <= width, result
    assert not result['overflow'], result
    assert all(b['height'] >= 44 and b['scroll'] <= b['width'] + 1 for b in result['buttons']), result


with tempfile.TemporaryDirectory(prefix='carnet-lxc-qa-', dir=SCRATCH) as directory:
    data = Path(directory)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    base = f'http://127.0.0.1:{port}'
    environment = dict(os.environ, VOICE_NOTES_DATA_DIR=str(data), VOICE_NOTES_API_TOKEN='public-browser-test-token')
    with (data / 'server.log').open('w+') as log:
        server = subprocess.Popen([API_PYTHON, '-m', 'uvicorn', 'app:app', '--app-dir', str(ROOT / 'lxc/server'), '--host', '127.0.0.1', '--port', str(port)], env=environment, stdout=log, stderr=log)
        try:
            for attempt in range(100):
                try:
                    with urllib.request.urlopen(base + '/health', timeout=1) as response:
                        assert json.load(response)['status'] == 'ok'
                    break
                except OSError:
                    if server.poll() is not None:
                        log.seek(0)
                        raise AssertionError(log.read())
                    time.sleep(.1)
            else:
                raise AssertionError('API did not become ready')
            with sync_playwright() as playwright:
                browser = playwright.chromium.launch(executable_path=os.environ.get('CHROME', '/usr/bin/google-chrome'), args=['--no-sandbox'])
                page = browser.new_page(reduced_motion='reduce')
                page.on('pageerror', lambda error: errors.append(str(error)))
                page.goto(base)
                page.wait_for_function("() => document.getElementById('health').textContent.includes('available')")
                for width in (320, 375, 414, 768, 1440):
                    check_layout(page, width)
                    page.screenshot(path=str(OUTPUT / f'lxc-locked-{width}.png'), full_page=True)
                page.fill('#token', 'wrong-public-fixture-token')
                page.click('#unlock')
                page.wait_for_function("() => document.getElementById('token').getAttribute('aria-invalid') === 'true'")
                page.fill('#token', 'public-browser-test-token')
                page.click('#unlock')
                page.wait_for_selector('#library', state='visible')
                assert 'No archived notes' in page.locator('#list-message').inner_text()
                # Clearly synthetic notes written into the real disposable database.
                audio_path = data / 'audio' / 'browser-fixture.wav'
                with wave.open(str(audio_path), 'wb') as audio:
                    audio.setnchannels(1)
                    audio.setsampwidth(2)
                    audio.setframerate(16000)
                    audio.writeframes(b'\0\0' * 16000)
                text = 'Browser test note: prepare the notebook.\n<script>window.fixtureInjected = true</script>\n' + 'é' * 500
                with sqlite3.connect(data / 'voice-notes.sqlite3') as connection:
                    connection.execute("INSERT INTO notes(note_id,status,text,audio_path,duration,language,model) VALUES(?,?,?,?,?,?,?)", ('browser-fixture', 'done', text, str(audio_path), 1.0, 'fr', 'whistle'))
                    connection.execute("INSERT INTO notes(note_id,status,text) VALUES(?,?,?)", ('browser-error', 'error', None))
                page.click('#refresh')
                page.wait_for_function("() => document.querySelectorAll('#notes li').length === 2")
                page.get_by_role('button').filter(has_text='browser-fixture').click()
                page.wait_for_function("() => document.getElementById('transcript').textContent.includes('prepare')")
                assert page.locator('#transcript').inner_text() == text
                assert not page.evaluate('Boolean(window.fixtureInjected)')
                for width in (320, 375, 414, 768, 1440):
                    check_layout(page, width)
                    page.screenshot(path=str(OUTPUT / f'lxc-library-{width}.png'), full_page=True)
                page.click('#load-audio')
                page.wait_for_function("() => document.getElementById('audio').readyState >= 1")
                assert page.locator('#audio').evaluate('e=>e.duration') == 1.0
                with page.expect_download() as downloaded:
                    page.click('#download-text')
                export = OUTPUT / 'fixture-export.txt'
                downloaded.value.save_as(export)
                assert export.read_text() == text
                page.fill('#search', 'no-match-fixture')
                page.locator('#search-form button').click()
                page.wait_for_function("() => document.getElementById('list-message').textContent.includes('No matching notes')")
                page.fill('#search', '')
                page.select_option('#status', 'error')
                page.wait_for_function("() => document.querySelectorAll('#notes li').length === 1")
                page.get_by_role('button').filter(has_text='browser-error').click()
                page.wait_for_function("() => document.getElementById('detail-message').textContent.includes('failed')")
                assert page.locator('#copy').is_disabled()
                page.click('#load-audio')
                page.wait_for_function("() => document.getElementById('audio-message').textContent.includes('missing')")
                page.click('#logout')
                assert not page.locator('#library').is_visible()
                assert page.locator('#transcript').text_content() == ''
                assert page.locator('#token').input_value() == ''
                assert page.evaluate('localStorage.length + sessionStorage.length') == 0
                page.locator('#token').focus()
                assert page.locator('#token').evaluate('e=>getComputedStyle(e).outlineStyle') != 'none'
                assert not errors, errors
                browser.close()
                print('LXC real API/SQLite browser QA: auth, empty, notes, XSS, WAV, export, search, filters, logout, focus PASS')
                print('Responsive layouts: 320/375/414/768/1440px PASS; screenshots:', OUTPUT)
        finally:
            server.terminate()
            server.wait(timeout=10)
