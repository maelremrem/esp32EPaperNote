// Run with node --test lxc/server/tests/test_web.cjs (no dependencies).
const {test} = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
class Element {
  constructor() { this.value = ''; this.hidden = false; this.disabled = false; this.textContent = ''; this.children = []; this.listeners = {}; this.attributes = {}; }
  addEventListener(event, handler) { this.listeners[event] = handler; }
  setAttribute(key, value) { this.attributes[key] = value; }
  removeAttribute(key) { delete this.attributes[key]; }
  replaceChildren(...children) { this.children = children; }
  append(...children) { this.children.push(...children); }
  focus() { this.focused = true; }
  pause() {}
  load() {}
  click() {}
  remove() {}
}
function setup(fetchOverride) {
  const nodes = new Map(); const calls = []; const revoked = [];
  const node = id => { if (!nodes.has(id)) nodes.set(id, new Element()); return nodes.get(id); };
  const note = {id:'voice-1',status:'done',text:'<img src=x onerror=alert(1)>',created_at:'2026-01-01 00:00:00', updated_at:'2026-01-01 00:00:00',duration:4,language:'fr',model:'whistle'};
  let failAuth = false;
  const context = {
    document: {getElementById:node, createElement:() => new Element(), body:new Element()},
    fetch: async (url, options={}) => { calls.push({url,options}); if (fetchOverride && url !== '/health') return fetchOverride(url, options); const status = failAuth && url !== '/health' ? 403 : 200;
      return {ok:status===200,status,json:async()=>url === '/health' ? {status:'ok',model_loaded:false} : url.includes('?') ? {items:[note],total:1,limit:30,offset:0} : note, blob:async()=>new Blob(['wav'])}; },
    URL: {createObjectURL:()=> 'blob:test', revokeObjectURL:url=>revoked.push(url)},
    URLSearchParams, AbortController, DOMException, Blob, Intl, Date, setTimeout, clearTimeout, console,
    navigator: {clipboard: {writeText:async()=>{}}},
    window: {addEventListener() {}},
  };
  vm.runInNewContext(fs.readFileSync(path.join(__dirname,'../web/app.js'),'utf8'), context);
  return {node,calls,revoked,fail:()=>{failAuth=true;}};
}
async function settle() { for(let i=0;i<8;i++) await new Promise(resolve=>setImmediate(resolve)); }
test('unlock reads real notes as text and logout clears token, transcript and audio', async () => {
  const {node,calls,revoked} = setup();
  node('token').value = 'browser-test';
  await node('login-form').listeners.submit({preventDefault(){}}); await settle();
  assert.equal(node('token').value,'');
  assert.equal(node('library').hidden,false);
  assert.match(node('health').textContent,/Service available/);
  assert.ok(calls.some(call=>call.options.headers?.Authorization==='Bearer browser-test'));
  assert.equal(node('notes').children.length,1);
  await node('notes').children[0].children[0].listeners.click(); await settle();
  assert.equal(node('transcript').textContent,'<img src=x onerror=alert(1)>');
  assert.equal(node('reader-title').focused,true);
  await node('load-audio').listeners.click(); await settle();
  assert.equal(node('audio').src,'blob:test');
  node('download-text').listeners.click();
  node('logout').listeners.click();
  assert.equal(revoked.length,2);
  assert.equal(node('library').hidden,true);
  assert.equal(node('transcript').textContent,'');
  assert.equal(node('notes').children.length,0);
  assert.ok(revoked.includes('blob:test'));
});
test('an expired token returns to the locked shell and clears note data', async () => {
  const {node,fail} = setup(); node('token').value='browser-test';
  await node('login-form').listeners.submit({preventDefault(){}}); await settle();
  fail(); await node('refresh').listeners.click(); await settle();
  assert.equal(node('library').hidden,true);
  assert.equal(node('notes').children.length,0);
  assert.match(node('login-message').textContent,/Token/);
});
test('a stale failed request cannot overwrite the locked state', async () => {
  let reject;
  const {node} = setup(() => new Promise((resolve, fail) => { reject = fail; }));
  node('token').value = 'browser-test';
  const login = node('login-form').listeners.submit({preventDefault(){}});
  await settle(); node('logout').listeners.click();
  reject(new Error('late network failure')); await login; await settle();
  assert.match(node('login-message').textContent,/locked/);
});
