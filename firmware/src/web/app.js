'use strict';
const el = id => document.getElementById(id);
let token = '', status = null, working = false, awaiting = 0, disconnected = false, timer;
let loadedConfig = false, savingUrl = '', accessGeneration = 0;
const names = {idle:'Ready to record',recording:'Recording',menu:'Device menu',syncing:'Transcribing'};
function message(text, error = false) { el('message').textContent = text; el('message').dataset.state = error ? 'error' : 'success'; }
function controls() {
  const ready = status && !working && !awaiting && !status.command_busy && !status.connecting;
  el('record').disabled = !ready || (status.state !== 'idle' && status.state !== 'recording') || status.stopping || (status.state === 'idle' && status.recovery);
  el('sync').disabled = !ready || status.state !== 'idle' || !status.wifi || !status.pending;
  el('record').textContent = status && status.state === 'recording' ? 'Stop note' : 'Record';
  el('record').dataset.recording = String(!!status && status.state === 'recording');
  el('save-server').disabled = !ready || !loadedConfig || !['idle','menu'].includes(status.state) || status.stopping;
  el('server-url').disabled = !loadedConfig || working || !!awaiting;
  el('disconnect').disabled = !token;
  el('access-button').disabled = working;
  el('access').setAttribute('aria-busy', String(working));
}
function logout() {
  ++accessGeneration;
  token = ''; status = null; awaiting = 0; savingUrl = ''; loadedConfig = false; working = false;
  clearTimeout(timer); el('token').value = ''; el('server-url').value = ''; el('server-panel').hidden = true;
  for (const id of ['text','note-id','ip','pending','wifi']) el(id).textContent = '';
  el('state').textContent = 'Not connected'; controls();
}
function validServerUrl(url) {
  if (url.length > 63) return false;
  const match = /^(https?):\/\/([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)(?::([0-9]+))?$/.exec(url);
  if (!match) return false;
  const parts = match[2].split('.');
  if (parts.some(p => Number(p) > 255 || (p.length > 1 && p[0] === '0'))) return false;
  const octets = parts.map(Number);
  if (octets[0] === 0 || octets[0] === 127 || octets[0] >= 224 || (octets[0] === 169 && octets[1] === 254) || (status && match[2] === status.ip)) return false;
  return !match[3] || (match[3][0] !== '0' && Number(match[3]) <= 65535);
}
async function request(path, method = 'GET', body) {
  const credential = token, generation = accessGeneration;
  const headers = {Authorization:'Bearer ' + credential};
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  const response = await fetch(path, {method,headers,body:body === undefined ? undefined : JSON.stringify(body),cache:'no-store',signal:AbortSignal.timeout(6000)});
  if (generation !== accessGeneration || credential !== token) throw Error('Access changed. Connect again.');
  if (!response.ok) {
    if (response.status === 401) logout();
    const messages = {401:'Incorrect token. Check your access.',403:'Address rejected. Open this page using the device IP address.',409:'Device busy or action unavailable. Refresh and try again.',413:'Request too large.',503:'Device unavailable. Try again.'};
    throw Error(messages[response.status] || 'Request rejected (' + response.status + ').');
  }
  const data = await response.json();
  if (generation !== accessGeneration || credential !== token) throw Error('Access changed. Connect again.');
  return data;
}
async function refresh() {
  clearTimeout(timer);
  if (!token) return;
  const generation = accessGeneration, pendingAtStart = awaiting;
  try {
    const incoming = await request('/api/status');
    // A poll begun before enqueue cannot complete or supersede that new command.
    if (awaiting !== pendingAtStart) return;
    status = incoming;
    if (!loadedConfig) {
      const config = await request('/api/config/server');
      el('server-url').value = config.base_url; loadedConfig = true; el('server-panel').hidden = false;
    }
    el('state').textContent = status.connecting ? 'Connecting to Wi-Fi' : status.stopping ? 'Finalizing note' : names[status.state] || 'Unknown state';
    el('wifi').textContent = status.wifi ? 'Wi-Fi connected' : 'Wi-Fi offline';
    el('ip').textContent = status.ip || '—';
    el('pending').textContent = String(status.pending);
    el('note-id').textContent = status.note_id || 'No note in this session';
    el('note-title').textContent = status.state === 'recording' ? 'Live text' : 'Latest note';
    el('text').textContent = (status.state === 'recording' ? status.live_text : status.text) || (status.state === 'recording' ? 'Speak near the microphone. Text will appear here.' : 'Your next idea starts with a note.');
    el('hint').textContent = status.recovery ? 'Audio needs recovery on the SD card. New notes are blocked.' : status.state === 'recording' ? 'Audio is saved on the SD card. Live text is provisional.' : 'The microphone is on the device, not in your browser.';
    if (awaiting && status.command_id === awaiting && status.command_result !== 'pending') {
      const result = status.command_result;
      if (savingUrl) {
        if (result === 'ok') {
          const config = await request('/api/config/server');
          if (config.base_url === savingUrl && config.command_id === awaiting && config.command_result === 'ok' && !config.command_busy)
            message('Server address saved. Live and final transcription use this address.');
          else message('Server address not confirmed. Reload access and check the current address.', true);
        } else message(result === 'rejected' ? 'State changed: server address not saved.' : 'Server address not saved. Previous address remains active. Try again.', true);
        savingUrl = '';
      } else message(result === 'ok' ? 'Command completed.' : result === 'stopping' ? 'Stop requested. Finalizing.' : result === 'rejected' ? 'State changed: command not executed.' : 'Command failed. Check the device and SD card.', result === 'rejected' || result === 'failed');
      awaiting = 0;
    } else if (awaiting && status.command_id !== awaiting) {
      awaiting = 0; savingUrl = '';
      message('Another session controlled the device. Check its current state.', true);
    } else if (!awaiting && disconnected) message('Connection restored.');
    else if (!awaiting && el('message').textContent === 'Connecting…') message('Connected.');
    disconnected = false;
    el('token').setAttribute('aria-invalid','false');
  } catch (error) {
    if (generation !== accessGeneration && token) return;
    disconnected = true;
    status = null;
    message(error.message === 'Failed to fetch' || error.message === 'offline' || error.name === 'TimeoutError' ? 'Connection lost. Check Wi-Fi and the device address.' : error.message, true);
    el('state').textContent = 'Connection unavailable';
    el('token').setAttribute('aria-invalid','true');
  } finally { controls(); if (token && generation === accessGeneration) timer = setTimeout(refresh, 1500); }
}
el('access').addEventListener('submit', async event => {
  event.preventDefault(); ++accessGeneration; loadedConfig = false; awaiting = 0; savingUrl = ''; token = el('token').value.trim();
  if (!token) {message('Enter the token to connect the device.',true); return;}
  working = true; controls(); message('Connecting…');
  await refresh(); working = false; controls();
});
async function command(action) {
  if (!status || working || awaiting) return;
  working = true; controls(); message('Sending command…');
  try { const result = await request('/api/command/' + action, 'POST'); awaiting = result.id; message('Command waiting for the device.'); }
  catch (error) {message(error.message || 'Connection lost. Refresh before trying again.',true); status = null;}
  finally {working = false; controls();}
}
el('server-config').addEventListener('submit', async event => {
  event.preventDefault();
  if (!status || working || awaiting || !loadedConfig) return;
  const url = el('server-url').value;
  if (!validServerUrl(url)) {message('Enter http:// or https:// followed by the server IPv4 and optional port (1–65535), without a path. Do not use the device address.', true); return;}
  working = true; controls();
  try {
    const result = await request('/api/config/server', 'POST', {base_url:url});
    savingUrl = url; awaiting = result.id; message('Server address waiting for the device.');
  } catch (error) {message(error.message, true);}
  finally {working = false; controls();}
});
el('disconnect').addEventListener('click', () => {logout(); message('Disconnected. Enter your token to access the device.');});
el('record').addEventListener('click', () => command(status && status.state === 'recording' ? 'stop' : 'start'));
el('sync').addEventListener('click', () => command('sync'));
controls();
