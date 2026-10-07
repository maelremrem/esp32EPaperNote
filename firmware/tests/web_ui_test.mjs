import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const source = fs.readFileSync(new URL('../src/web/app.js', import.meta.url), 'utf8');
const elements = new Map();
function element(id) {
  if (!elements.has(id)) elements.set(id, {textContent:'',value:'',disabled:false,hidden:false,dataset:{},
    addEventListener(name, fn) { this[name] = fn; }, setAttribute(name,value) {this[name]=value;}});
  return elements.get(id);
}
let requests = [], response = {state:'idle',wifi:true,ip:'192.168.1.4',pending:2,text:'<script>safe</script>',live_text:'',note_id:'note',stopping:false,recovery:false,command_busy:false,command_id:0,command_result:''};
const context = vm.createContext({document:{getElementById:element},fetch:async (url,options)=>{
  requests.push([url, options]); return {ok:true,status:200,json:async()=>response};
},AbortSignal:{timeout:()=>null},setTimeout:()=>0,clearTimeout(){}});
vm.runInContext(source, context);
assert.equal(element('record').disabled,true);
element('token').value='test-token';
await element('access').submit({preventDefault(){}});
assert.equal(requests[0][1].headers.Authorization,'Bearer test-token');
assert.equal(element('text').textContent,'<script>safe</script>');
assert.equal(element('record').disabled,false);
assert.equal(element('sync').disabled,false);
response={id:1}; await element('record').click();
assert.equal(requests.at(-1)[0],'/api/command/start');
assert.match(element('message').textContent,/waiting/i);
assert.equal(element('record').disabled,true);
response={state:'idle',wifi:true,pending:1,stopping:false,recovery:false,command_busy:false,command_id:2,command_result:'ok'};
await vm.runInContext('refresh()', context);
assert.equal(element('record').disabled,false, 'another client must not leave our UI stuck waiting for an overwritten result');
assert.match(element('message').textContent,/another session/i);
context.fetch = async()=>{throw Error('offline');};
await element('access').submit({preventDefault(){}});
assert.equal(element('record').disabled,true);
assert.match(element('message').textContent,/connection/i);
context.fetch=async()=>({ok:true,status:200,json:async()=>response});
await vm.runInContext('refresh()', context);
assert.equal(element('record').disabled,false);
assert.match(element('message').textContent,/restored/i);
console.log('web UI auth, safe text, queued-not-success, offline: PASS');
