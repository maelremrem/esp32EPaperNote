#!/usr/bin/env python3
"""Real HTML/CSS/JS in Chrome; only API responses are simulated (no device required)."""
import json
import os
from pathlib import Path
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from playwright.sync_api import sync_playwright

root = Path(__file__).resolve().parents[1] / 'src/web'
class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(root), **kwargs)
    def log_message(self, format, *args):
        pass

server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f'http://127.0.0.1:{server.server_port}'
fixture = dict(state='idle', wifi=True, ip='192.168.1.4', pending=2, text='An idea to keep.\n' + 'é' * 500,
               live_text='', note_id='note-de-test', stopping=False, recovery=False,
               command_busy=False, command_id=0, command_result='')
scratch = Path(os.environ.get('TMPDIR', str(Path.home() / '.hermes/cache/scratch')))
with sync_playwright() as p:
    browser = p.chromium.launch(executable_path=os.environ.get('CHROME', '/usr/bin/google-chrome'), args=['--no-sandbox'])
    page = browser.new_page(reduced_motion='reduce')
    errors = []
    page.on('pageerror', lambda error: errors.append(str(error)))
    server_config = dict(base_url='http://192.168.1.20:8000', command_id=0, command_result='', command_busy=False)
    def config_route(route):
        assert route.request.headers.get('authorization') == 'Bearer public-test-token'
        if route.request.method == 'POST':
            assert route.request.post_data_json == dict(base_url='http://192.168.1.21:8001')
            route.fulfill(status=202, content_type='application/json', body='{"id":8,"status":"queued"}')
        else: route.fulfill(content_type='application/json', body=json.dumps(server_config))
    page.route('**/api/config/server', config_route)
    page.route('**/api/status', lambda route: route.fulfill(content_type='application/json', body=json.dumps(fixture)))
    page.goto(base)
    page.fill('#token', 'public-test-token')
    page.click('#access-button')
    page.wait_for_function("!document.getElementById('record').disabled")
    border = page.locator('#token').evaluate("e=>getComputedStyle(e).borderTopColor")
    lightness = float(border.split('(')[1].split()[0])
    ratio = 1.05 / (lightness ** 3 + .05)
    assert ratio >= 3, ('Input border contrast', border, ratio)
    assert page.input_value('#server-url') == server_config['base_url']
    page.fill('#server-url', 'http://192.168.1.21:8001')
    page.evaluate('refresh()')
    assert page.input_value('#server-url') == 'http://192.168.1.21:8001'
    page.click('#save-server')
    page.wait_for_function("document.getElementById('message').textContent.includes('waiting')")
    fixture.update(command_id=8, command_result='pending', command_busy=True)
    page.evaluate('refresh()')
    assert page.locator('#save-server').is_disabled()
    fixture.update(command_result='failed', command_busy=False)
    page.evaluate('refresh()')
    assert 'not saved' in page.locator('#message').inner_text()
    page.click('#save-server')
    page.wait_for_function("document.getElementById('message').textContent.includes('waiting')")
    fixture.update(command_result='ok')
    server_config.update(base_url='http://192.168.1.21:8001',command_id=8,command_result='ok')
    page.evaluate('refresh()')
    assert 'saved' in page.locator('#message').inner_text()
    for width in (320, 375, 414, 768, 1440):
        page.set_viewport_size(dict(width=width, height=950))
        dimensions = page.evaluate('''() => ({width:innerWidth, scroll:document.documentElement.scrollWidth,
            overflowing:[...document.querySelectorAll('main *,header *,footer *')].filter(e=>e.getBoundingClientRect().right>innerWidth+1).map(e=>e.id||e.tagName),
            buttons:[...document.querySelectorAll('button')].map(e=>({height:e.getBoundingClientRect().height,scroll:e.scrollWidth,width:e.clientWidth}))})''')
        assert dimensions['scroll'] <= width, dimensions
        assert not dimensions['overflowing'], dimensions
        assert all(b['height'] >= 44 and b['scroll'] <= b['width'] for b in dimensions['buttons']), dimensions
        page.screenshot(path=str(scratch / f'carnet-esp32-{width}.png'), full_page=True)
        print(f'Chrome {width}px: no overflow, touch targets PASS')
    assert page.locator('#text').inner_text().startswith('An idea')
    page.route('**/api/command/start', lambda route: route.fulfill(status=202, content_type='application/json', body='{"id":1,"status":"queued"}'))
    page.click('#record')
    page.wait_for_function("document.getElementById('message').textContent.includes('waiting')")
    assert page.locator('#record').is_disabled()
    assert 'waiting' in page.locator('#message').inner_text()
    fixture.update(state='recording', command_id=1, command_result='ok', live_text='<script>not HTML</script>')
    page.wait_for_function("document.getElementById('record').textContent === 'Stop note'")
    assert page.locator('#text').inner_text() == '<script>not HTML</script>'
    assert not page.locator('#sync').is_enabled()
    page.route('**/api/status', lambda route: route.fulfill(status=401, content_type='application/json', body='{}'))
    page.wait_for_function("document.getElementById('message').textContent.includes('Incorrect token')")
    assert page.locator('#record').is_disabled()
    assert page.input_value('#server-url') == '' and page.input_value('#token') == ''
    assert page.locator('#server-panel').is_hidden() and page.locator('#text').inner_text() == ''
    assert not errors, errors
    page.locator('#token').focus()
    assert page.locator('#token').evaluate("e=>getComputedStyle(e).outlineStyle") != 'none'
    assert page.evaluate('localStorage.length + sessionStorage.length') == 0
    print('Chrome queued/start/live-text/auth/focus/no persistence: PASS')
    browser.close()
server.shutdown()
