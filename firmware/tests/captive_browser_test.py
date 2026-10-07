#!/usr/bin/env python3
"""Chrome runs actual handler-generated HTML/CSP; loopback scan API is a fixture.
No RF, phone captive detection, NVS or real ESP32 HTTP socket claims.
"""
import json, os, subprocess, sys, tempfile, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs
from playwright.sync_api import sync_playwright
ROOT=Path(__file__).resolve().parents[2]
NETWORKS=[{'ssid':'Maison – 東京','rssi':-34,'channel':6,'authmode':3},
 {'ssid':'<img src=x onerror=alert(1)>','rssi':-51,'channel':1,'authmode':0},
 {'ssid':'X'*32,'rssi':-65,'channel':11,'authmode':7}]

def main():
 with tempfile.TemporaryDirectory(prefix='captive-browser-',dir=os.environ.get('TMPDIR')) as tmp:
  output=Path(tmp)/'portal.html'
  subprocess.run([os.environ.get('HOST_PYTHON',sys.executable),str(ROOT/'firmware/tests/test_wifi_provisioning.py')],env={**os.environ,'CAPTIVE_HTML_OUTPUT':str(output)},check=True)
  html=output.read_bytes();csp=Path(str(output)+'.csp').read_text()
  state={'polls':0,'mode':'complete','urls':[],'saved':None}
  class Handler(BaseHTTPRequestHandler):
   def log_message(self,format,*args):pass
   def respond(self,code,body,kind='text/html'):
    self.send_response(code);self.send_header('Content-Type',kind);self.send_header('Content-Security-Policy',csp);self.send_header('Cache-Control','no-store');self.end_headers();self.wfile.write(body)
   def do_GET(self):
    state['urls'].append(self.path)
    if self.path=='/':return self.respond(200,html)
    if self.path=='/scan/results':
     assert self.headers.get('X-CSRF-Token')
     state['polls']+=1
     stage='queued' if state['polls']==1 else 'scanning' if state['polls']==2 else state['mode']
     body={'state':stage,'error':'Native scan failed (7). Retry.' if stage=='error' else '', 'networks':NETWORKS if stage=='complete' else []}
     return self.respond(200,json.dumps(body).encode(),'application/json')
    self.respond(404,b'Not found')
   def do_POST(self):
    state['urls'].append(self.path)
    body=self.rfile.read(int(self.headers['Content-Length']));fields=parse_qs(body.decode(),keep_blank_values=True)
    assert fields['csrf'][0]
    if self.path=='/scan':state['polls']=0;return self.respond(202,b'Queued')
    state['saved']=fields;self.respond(202,b'Queued. Check device screen.')
  server=ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
  try:
   with sync_playwright() as p:
    browser=p.chromium.launch(headless=True)
    for width in (320,375,1000):
     context=browser.new_context(viewport={'width':width,'height':900});page=context.new_page();errors=[];dialogs=[]
     page.on('pageerror',lambda e:errors.append(str(e)));page.on('dialog',lambda d:(dialogs.append(d.message),d.dismiss()))
     page.goto(f'http://127.0.0.1:{server.server_port}/');assert not page.locator('[name=csrf]').is_visible(),'CSRF field must stay hidden'
     page.locator('#h').fill('Hidden draft');page.locator('#hp').fill('synthetic123');page.locator('#scan').click()
     assert page.locator('#scan').is_disabled()
     page.wait_for_function("() => document.querySelector('#status').textContent.startsWith('Choose a network')")
     assert page.locator('#h').input_value()=='Hidden draft' and page.locator('#hp').input_value()=='synthetic123'
     assert page.locator('#h-pick option').count()==4
     assert 'WPA2' in page.locator('#h-pick option').nth(1).inner_text() and '-34 dBm' in page.locator('#h-pick option').nth(1).inner_text()
     page.locator('#h-pick').select_option(NETWORKS[0]['ssid']);assert page.locator('#h').input_value()==NETWORKS[0]['ssid'] and page.locator('#hp').input_value()==''
     page.locator('#hp').fill('synthetic123');page.locator('#s-pick').select_option(NETWORKS[1]['ssid']);page.locator('#so').check()
     assert page.locator('img').count()==0 and not dialogs
     assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
     assert page.evaluate("[...document.querySelectorAll('button')].every(x=>x.scrollWidth<=x.clientWidth)")
     if os.environ.get('CAPTIVE_SCREENSHOTS'):
      directory=ROOT/'docs/screens/web';directory.mkdir(parents=True,exist_ok=True)
      page.locator('#s').fill('Phone hotspot');page.screenshot(path=str(directory/f'captive-{width}.png'),full_page=True);page.locator('#s').fill(NETWORKS[1]['ssid'])
     page.locator('#scan').click();page.wait_for_function("() => !document.querySelector('#scan').disabled")
     assert page.locator('#h').input_value()==NETWORKS[0]['ssid'] and page.locator('#hp').input_value()=='synthetic123' and page.locator('#so').is_checked()
     state['mode']='error';page.locator('#scan').click();page.wait_for_function("() => document.querySelector('#status').textContent.includes('Native scan failed (7)')")
     assert page.locator('#scan').is_enabled();state['mode']='complete'
     page.locator('#h').fill('é'*17);page.locator('button[type=submit]').click();assert page.locator('#h').evaluate('(x)=>!x.validity.valid')
     page.locator('#h').fill(NETWORKS[0]['ssid']);page.locator('#hp').fill('');page.locator('button[type=submit]').click();assert page.locator('#h').evaluate('(x)=>!x.validity.valid')
     page.locator('#hp').fill('synthetic123');page.locator('button[type=submit]').click();page.wait_for_url('**/save',timeout=3000)
     assert state['saved']['h']==[NETWORKS[0]['ssid']] and state['saved']['s']==[NETWORKS[1]['ssid']] and state['saved']['sp']==[''] and state['saved']['so']==['1']
     assert not errors,errors
     context.close()
    context=browser.new_context(java_script_enabled=False);page=context.new_page();page.goto(f'http://127.0.0.1:{server.server_port}/')
    assert not page.locator('#scan').is_visible();page.locator('#h').fill('Manual hidden');page.locator('#hp').fill('synthetic123');page.locator('button[type=submit]').click();page.wait_for_url('**/save');assert state['saved']['h']==['Manual hidden'];context.close();browser.close()
    assert all('csrf' not in url and '?' not in url for url in state['urls'])
    print('PASS Chrome 320/375/1000: actual portal HTML+CSP; draft/scan/error/manual/no-JS/UTF8/XSS/no token URLs/no overflow (fixture API)')
  finally:server.shutdown();server.server_close();thread.join()
if __name__=='__main__':main()
