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
               live_text='', note_id='note-de-test', stopping=False, recovery=False, mounted=True, audio_ready=True,
               command_busy=False, command_id=0, command_result='')
scratch = Path(os.environ.get('TMPDIR', str(Path.home() / '.hermes/cache/scratch')))
with sync_playwright() as p:
    browser = p.chromium.launch(executable_path=os.environ.get('CHROME', '/usr/bin/google-chrome'), args=['--no-sandbox'])
    page = browser.new_page(reduced_motion='reduce')
    errors = []
    page.on('pageerror', lambda error: errors.append(str(error)))
    server_config = dict(base_url='http://192.168.1.20:8000', command_id=0, command_result='', command_busy=False)
    def config_route(route):
        assert route.request.headers.get('authorization') == 'Bearer 001234'
        if route.request.method == 'POST':
            assert route.request.post_data_json in (dict(base_url='http://192.168.1.21:8001'),dict(base_url='http://192.168.1.21:8001',token='fixture-runtime-token'))
            route.fulfill(status=202, content_type='application/json', body='{"id":8,"status":"queued"}')
        else: route.fulfill(content_type='application/json', body=json.dumps(server_config))
    device_settings = dict(home_ssid='Fixture Home', hotspot_ssid='Fixture Hotspot', current_ssid='Fixture Home',
        ip='192.168.1.4', mounted=True, audio_ready=True, storage_error='', partial_limit=10,
        format_challenge=0, command_id=0, command_result='', command_busy=False)
    settings_posts=[]
    def settings_route(route):
        assert route.request.headers.get('authorization') == 'Bearer 001234'
        if route.request.method == 'POST':
            settings_posts.append(route.request.post_data_json)
            route.fulfill(status=202, content_type='application/json', body='{"id":9,"status":"queued"}')
        else: route.fulfill(content_type='application/json', body=json.dumps(device_settings))
    page.route('**/api/settings', settings_route)
    page.route('**/api/config/server', config_route)
    pin_requests = []
    def status_route(route):
        pin_requests.append(route.request)
        assert '001234' not in route.request.url
        authorized = route.request.headers.get('authorization') == 'Bearer 001234'
        route.fulfill(status=200 if authorized else 401, content_type='application/json', body=json.dumps(fixture if authorized else {'error':'physical_pairing_required'}))
    page.route('**/api/status', status_route)
    notes_fixture={'notes':[{'id':'safe-note','audio':True,'transcribed':True},{'id':'cancelled-note','audio':True,'transcribed':False},{'id':'<img onerror=alert(1)>','audio':True,'transcribed':True}]}
    page.route('**/api/notes',lambda route: route.fulfill(content_type='application/json',body=json.dumps(notes_fixture)))
    audio_fixture=b'RIFF'+bytes(range(256))*35
    markdown_fixture=b'# Note safe-note\n\nComplete transcript\n\n---\n\n- ID: safe-note\n- Duration: 1.00 s\n- Language: en\n- STT: whistle\n- Status: synced\n'
    def download_route(route):
        assert route.request.headers.get('authorization')=='Bearer 001234'
        audio='/audio?' in route.request.url
        route.fulfill(body=audio_fixture if audio else markdown_fixture,content_type='audio/wav' if audio else 'text/markdown',headers={'Content-Disposition':'attachment; filename="safe-note.'+('wav' if audio else 'md')+'"'})
    page.route('**/api/download/*',download_route)
    page.route('**/api/session/close',lambda route: route.fulfill(content_type='application/json',body='{"status":"closed"}'))
    page.goto(base)
    page.click('#settings-nav'); assert page.locator('#settings-panel').is_visible() and page.locator('#save-server').is_disabled()
    assert page.locator('#access-dialog').is_visible()
    assert page.locator('#token').evaluate('e=>e===document.activeElement')
    page.fill('#token','001234'); page.keyboard.press('Escape')
    assert page.locator('#access-dialog').is_hidden() and page.input_value('#token')==''
    assert page.locator('#settings-nav').evaluate('e=>e===document.activeElement')
    assert page.locator('#server-url').is_enabled() and page.locator('#server-token').is_enabled()
    page.fill('#server-url','http://192.168.1.20:8000')
    page.fill('#server-token','fixture-draft-token')
    for width in (320,375,414,768,1440):
        page.set_viewport_size(dict(width=width,height=950))
        page.click('#open-access')
        assert page.locator('#token').evaluate('e=>e===document.activeElement')
        before=len(pin_requests)
        page.fill('#token','12345'); page.click('#access-button')
        assert len(pin_requests)==before
        page.fill('#token','111111'); page.click('#access-button')
        page.wait_for_function("document.getElementById('access-message').textContent.includes('code incorrect')")
        assert page.locator('#access-dialog').is_visible() and page.input_value('#token')==''
        for _ in range(8):
            page.keyboard.press('Tab')
            assert page.evaluate("document.getElementById('access-dialog').contains(document.activeElement)"), page.evaluate("({active:document.activeElement.outerHTML,open:document.getElementById('access-dialog').open})")
        page.locator('#token').focus()
        for _ in range(8):
            page.keyboard.press('Shift+Tab')
            assert page.evaluate("document.getElementById('access-dialog').contains(document.activeElement)")
        page.locator('#token').focus()
        bounds=page.locator('#access-dialog').evaluate('e=>({left:e.getBoundingClientRect().left,right:e.getBoundingClientRect().right,scroll:e.scrollWidth,width:e.clientWidth})')
        assert bounds['left']>=0 and bounds['right']<=width and bounds['scroll']<=bounds['width']
        buttons=page.locator('#access-dialog button').evaluate_all('es=>es.map(e=>({height:e.getBoundingClientRect().height,scroll:e.scrollWidth,width:e.clientWidth}))')
        assert all(b['height']>=44 and b['scroll']<=b['width'] for b in buttons), buttons
        page.click('#cancel-access'); page.click('#open-access')
        page.screenshot(path=str(scratch/f'carnet-esp32-pin-modal-{width}.png'))
        page.set_viewport_size(dict(width=width,height=360))
        short=page.locator('#access-dialog').evaluate('e=>({top:e.getBoundingClientRect().top,bottom:e.getBoundingClientRect().bottom})')
        assert short['top']>=0 and short['bottom']<=360
        page.click('#cancel-access')
        assert page.locator('#open-access').evaluate('e=>e===document.activeElement')
        assert page.input_value('#server-token')=='fixture-draft-token'
        assert page.input_value('#server-url')=='http://192.168.1.20:8000'
        page.set_viewport_size(dict(width=width,height=950))
        page.click('#open-access'); page.click('#close-access')
        page.click('#open-access'); page.mouse.click(2,2)
        assert page.locator('#access-dialog').is_hidden()
        print(f'Chrome PIN modal {width}px: validation, retry, focus trap/return, Escape/Cancel/close/backdrop, short viewport PASS')
    page.click('#notebook-nav')
    page.click('#open-access')
    page.context.grant_permissions(['clipboard-read','clipboard-write'])
    page.evaluate("navigator.clipboard.writeText('001234')")
    page.locator('#token').focus(); page.keyboard.press('Control+V')
    assert page.input_value('#token')=='001234'
    page.click('#access-button')
    page.wait_for_function("!document.getElementById('record').disabled")
    assert page.locator('#access-dialog').is_hidden()
    assert page.input_value('#server-token')=='fixture-draft-token'
    page.evaluate("document.getElementById('server-token').value=''")
    border = page.locator('#token').evaluate("e=>getComputedStyle(e).borderTopColor")
    lightness = float(border.split('(')[1].split()[0])
    ratio = 1.05 / (lightness ** 3 + .05)
    assert ratio >= 3, ('Input border contrast', border, ratio)
    page.click('#settings-nav')
    assert page.locator('#settings-panel').is_visible()
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
    server_config.update(base_url='http://192.168.1.21:8001',server_revision=1,command_id=8,command_result='ok')
    page.evaluate('refresh()')
    assert 'saved' in page.locator('#message').inner_text()
    page.fill('#server-token','fixture-runtime-token'); page.click('#save-server')
    page.wait_for_function("document.getElementById('message').textContent.includes('waiting')")
    assert page.input_value('#server-token')==''
    server_config.update(server_revision=2,token_configured=True); page.evaluate('refresh()')
    assert 'saved' in page.locator('#message').inner_text() and 'configured' in page.locator('#token-state').inner_text()
    assert page.evaluate('localStorage.length + sessionStorage.length')==0
    page.click('#notebook-nav')
    assert page.locator('#note-list li').count()==2
    for label,expected,filename in [('Download audio',audio_fixture,'safe-note.wav'),('Download Markdown',markdown_fixture,'safe-note.md')]:
        with page.expect_download() as download_info: page.locator('#note-list li').first.get_by_role('button',name=label).click()
        received=download_info.value
        assert received.suggested_filename==filename and Path(received.path()).read_bytes()==expected
    assert not page.locator('#note-list img').count()
    page.click('#settings-nav')
    page.fill('#home-ssid', 'Edited Home'); page.fill('#home-password', 'synthetic123')
    page.evaluate('refresh()')
    assert page.input_value('#home-ssid') == 'Edited Home' and page.input_value('#home-password') == 'synthetic123'
    page.click('#save-wifi'); page.wait_for_function("document.getElementById('message').textContent.includes('waiting')")
    assert page.input_value('#home-password') == ''
    assert settings_posts[-1]['profiles'][0] == dict(ssid='Edited Home',password='synthetic123',open=False)
    fixture.update(command_id=9, command_result='ok')
    device_settings.update(command_id=9, command_result='ok', home_ssid='Edited Home')
    page.evaluate('refresh()'); assert 'Networks saved' in page.locator('#message').inner_text()
    page.select_option('#refresh-limit','20'); page.click('#save-display')
    page.wait_for_function("document.getElementById('message').textContent.includes('waiting')")
    device_settings.update(partial_limit=20); page.evaluate('refresh()')
    assert 'Display setting saved' in page.locator('#message').inner_text()
    page.click('#prepare-format'); page.wait_for_function("document.getElementById('message').textContent.includes('waiting')")
    device_settings.update(format_challenge=123); page.evaluate('refresh()')
    assert page.locator('#format-confirm').is_visible()
    assert page.locator('#cancel-format').evaluate('e=>e===document.activeElement')
    page.click('#cancel-format'); assert page.locator('#format-confirm').is_hidden()
    assert not any(p['action']=='format' for p in settings_posts)
    # Honest SD error comes from the injected API fixture, never invented device output.
    device_settings.update(mounted=False, storage_error='SD mount: ESP_FAIL. Insert a FAT32 card and retry.')
    fixture.update(mounted=False)
    page.evaluate('refresh()'); assert 'ESP_FAIL' in page.locator('#storage-error').inner_text()
    assert page.locator('#prepare-format').is_disabled()
    for width in (320, 375, 414, 768, 1440):
        page.set_viewport_size(dict(width=width, height=950))
        dimensions = page.evaluate('''() => ({width:innerWidth, scroll:document.documentElement.scrollWidth,
            overflowing:[...document.querySelectorAll('main *,header *,footer *')].filter(e=>e.getBoundingClientRect().right>innerWidth+1).map(e=>e.id||e.tagName),
            buttons:[...document.querySelectorAll('button')].filter(e=>e.getClientRects().length).map(e=>({height:e.getBoundingClientRect().height,scroll:e.scrollWidth,width:e.clientWidth}))})''')
        assert dimensions['scroll'] <= width, dimensions
        assert not dimensions['overflowing'], dimensions
        assert all(b['height'] >= 44 and b['scroll'] <= b['width'] for b in dimensions['buttons']), dimensions
        page.screenshot(path=str(scratch / f'carnet-esp32-settings-{width}.png'), full_page=True)
        page.click('#notebook-nav')
        page.screenshot(path=str(scratch / f'carnet-esp32-{width}.png'), full_page=True)
        page.click('#settings-nav')
        print(f'Chrome {width}px: no overflow, touch targets PASS')
    page.click('#retry-storage'); page.wait_for_function("document.getElementById('message').textContent.includes('waiting')")
    assert settings_posts[-1] == dict(action='mount')
    fixture.update(mounted=True); device_settings.update(mounted=True, storage_error='')
    page.evaluate('refresh()'); assert page.locator('#message').inner_text() == 'SD mounted.'
    page.click('#notebook-nav')
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
    page.wait_for_function("document.getElementById('message').textContent.includes('Local access expired')")
    assert page.locator('#record').is_disabled()
    assert page.input_value('#server-url') == '' and page.input_value('#token') == ''
    assert page.locator('#text').inner_text() == ''
    assert page.input_value('#home-ssid') == '' and page.locator('#note-list li').count()==0
    assert not errors, errors
    page.click('#open-access')
    page.locator('#token').focus()
    assert page.locator('#token').evaluate("e=>getComputedStyle(e).outlineStyle") != 'none'
    assert page.evaluate('localStorage.length + sessionStorage.length') == 0
    page.route('**/api/status', lambda route: route.fulfill(status=401,content_type='application/json',body='{"error":"pin_attempts_exhausted"}'))
    page.fill('#token','111111'); page.click('#access-button')
    page.wait_for_function("document.getElementById('access-message').textContent.includes('Five incorrect PIN attempts')")
    assert page.locator('#access-dialog').is_visible() and page.input_value('#token')==''
    assert 'reopen Settings' in page.locator('#access-message').inner_text()
    page.keyboard.press('Escape')
    assert page.locator('#open-access').evaluate('e=>e===document.activeElement')
    assert not errors, errors
    print('Chrome queued/start/live-text/auth/focus/no persistence: PASS')
    browser.close()
server.shutdown()
