'use strict';
const el = id => document.getElementById(id);
let token = '', status = null, working = false, awaiting = 0, disconnected = false, timer;
let loadedConfig = false, savingUrl = '', accessGeneration = 0;
let loadedSettings = false, savingSettings = null, formatChallenge = 0, formatExpires = 0;
let downloadButtons = [];
let settingsView = false, savingTarget = null, configRevision = 0, downloading = false;
let accessTrigger = null, pairing = false, pairingAttempt = 0;
function openAccess(trigger) {
  if (token) return;
  accessTrigger = trigger;
  el('token').value = '';
  el('token').setAttribute('aria-invalid','false');
  el('access-message').textContent = '';
  if (!el('access-dialog').open) el('access-dialog').showModal();
  el('token').focus();
}
function closeAccess() {
  ++pairingAttempt;
  el('token').value = '';
  if (pairing) { logout(true); pairing = false; }
  if (el('access-dialog').open) el('access-dialog').close();
  if (accessTrigger) accessTrigger.focus();
}
el('open-access').addEventListener('click', () => openAccess(el('open-access')));
for (const id of ['cancel-access','close-access']) el(id).addEventListener('click', closeAccess);
el('access-dialog').addEventListener('cancel', event => { event.preventDefault(); closeAccess(); });
el('access-dialog').addEventListener('close', () => { el('token').value = ''; if (accessTrigger) accessTrigger.focus(); });
el('access-dialog').addEventListener('keydown', event => {
  if (event.key !== 'Tab') return;
  const focusable = [...el('access-dialog').querySelectorAll('button:not(:disabled), input:not(:disabled)')];
  const first = focusable[0], last = focusable[focusable.length - 1];
  if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
  else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
});
el('access-dialog').addEventListener('click', event => {
  if (event.target !== el('access-dialog')) return;
  const rect = el('access-dialog').getBoundingClientRect();
  if (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom) closeAccess();
});
function navigateSettings(show) {
  settingsView = show;
  el('notebook-panel').hidden = show;
  el('settings-panel').hidden = !show;
  el('notebook-nav').setAttribute('aria-pressed', String(!show));
  el('settings-nav').setAttribute('aria-pressed', String(show));
  if (show && !token) message('Draft your server settings here. Connect with the six-digit PIN shown in Settings → Wi-Fi → Open web settings on the ESP32 to save changes.');
}
const names = {idle:'Ready to record',recording:'Recording',menu:'Device menu',syncing:'Transcribing'};
function message(text, error = false) { el('message').textContent = text; el('message').dataset.state = error ? 'error' : 'success'; }
function controls() {
  const ready = status && !working && !awaiting && !status.command_busy && !status.connecting && !downloading;
  el('record').disabled = (status && status.state === 'idle' && (status.mounted === false || status.audio_ready === false)) || !ready || (status.state !== 'idle' && status.state !== 'recording') || status.stopping || (status.state === 'idle' && status.recovery);
  el('sync').disabled = (status && status.mounted === false) || !ready || status.state !== 'idle' || !status.wifi || !status.pending;
  el('record').textContent = status && status.state === 'recording' ? 'Stop note' : 'Record';
  el('record').dataset.recording = String(!!status && status.state === 'recording');
  el('save-server').disabled = !ready || !loadedConfig || !['idle','menu'].includes(status.state) || status.stopping;
  el('server-url').disabled = working || !!awaiting;
  el('server-token').disabled = working || !!awaiting;
  el('clear-server-token').disabled = working || !!awaiting;
  const editable = ready && loadedSettings && ['idle','menu'].includes(status.state) && !status.stopping;
  for (const id of ['save-wifi','save-display','retry-storage','reconnect-wifi']) el(id).disabled = !editable;
  el('prepare-format').disabled = !editable || !status.mounted;
  el('erase-storage').disabled = !editable || !formatChallenge || Date.now() >= formatExpires;
  for (const slot of ['home','hotspot']) {
    el(slot+'-ssid').disabled = !loadedSettings || working || !!awaiting;
    el(slot+'-open').disabled = !loadedSettings || working || !!awaiting;
    el(slot+'-password').disabled = !loadedSettings || working || !!awaiting || el(slot+'-open').checked;
  }
  el('refresh-limit').disabled = !loadedSettings || working || !!awaiting;
  el('disconnect').disabled = !token;
  el('access-button').disabled = working;
  el('access').setAttribute('aria-busy', String(working));
  for (const button of downloadButtons) button.disabled = !ready || !status || !['idle','menu'].includes(status.state) || status.stopping;
}
function logout(preserveDraft = false) {
  const draftUrl = preserveDraft ? el('server-url').value : '';
  const draftToken = preserveDraft ? el('server-token').value : '';
  const draftClear = preserveDraft && el('clear-server-token').checked;
  ++accessGeneration;
  token = ''; status = null; awaiting = 0; savingUrl = ''; loadedConfig = false; working = false;
  loadedSettings = false; savingSettings = null; formatChallenge = 0;
  el('format-confirm').hidden = true; navigateSettings(settingsView);
  savingTarget = null; configRevision = 0; downloading = false;
  el('server-token').value = ''; el('clear-server-token').checked = false;
  el('token-state').textContent = ''; el('note-list').textContent = ''; downloadButtons = []; el('storage-gauge').hidden = true; el('storage-usage').textContent = 'Usage unknown';
  for (const id of ['home-ssid','hotspot-ssid','home-password','hotspot-password','refresh-limit']) el(id).value = '';
  for (const id of ['home-open','hotspot-open']) el(id).checked = false;
  for (const id of ['station-status','saved-networks','storage-status','storage-error']) el(id).textContent = '';
  clearTimeout(timer); el('token').value = ''; el('server-url').value = ''; el('server-panel').hidden = false;
  for (const id of ['text','note-id','ip','pending','wifi']) el(id).textContent = '';
  el('state').textContent = 'Not connected'; controls();
  if (preserveDraft) { el('server-url').value = draftUrl; el('server-token').value = draftToken; el('clear-server-token').checked = draftClear; }
  else closeAccess();
}
function normalizeServerUrl(url) {
  if (/^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+(?::[0-9]+)?$/.test(url)) return 'http://' + url + (url.includes(':') ? '' : ':8080');
  return url;
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
    let reason = '';
    if (response.status === 401 || response.status === 429) {
      try { reason = (await response.json()).error || ''; } catch (_) {}
      if (generation !== accessGeneration || credential !== token) throw Error('Access changed. Connect again.');
      logout(pairing);
    }
    const messages = {401:'Local access expired or code incorrect. Open web settings on the ESP32 to authorize a new session.',403:'Address rejected. Open this page using the device IP address.',409:'Device busy or action unavailable. Refresh and try again.',413:'Request too large.',503:'Device unavailable. Try again.'};
    const error = Error(messages[response.status] || 'Request rejected (' + response.status + ').');
    if (reason === 'pin_attempts_exhausted') error.message = 'Five incorrect PIN attempts. Access has been revoked. On the ESP32, reopen Settings → Wi-Fi → Open web settings for a new PIN.';
    error.authFailure = response.status === 401; throw error;
  }
  const data = await response.json();
  if (generation !== accessGeneration || credential !== token) throw Error('Access changed. Connect again.');
  return data;
}
function validNoteId(id) { return typeof id === 'string' && /^[A-Za-z0-9_-]{1,96}$/.test(id); }
function renderSavedNotes(notes) {
  el('note-list').textContent = ''; downloadButtons = [];
  for (const note of (Array.isArray(notes) ? notes : []).slice(0,40)) {
    if (!validNoteId(note.id)) continue;
    const row = document.createElement('li'), name = document.createElement('span');
    name.textContent = note.id + (note.transcribed ? ' · Transcribed' : ' · Audio only'); row.appendChild(name);
    for (const kind of ['audio','markdown']) {
      if (!(kind === 'audio' ? note.audio : note.transcribed)) continue;
      const button = document.createElement('button'); button.type = 'button';
      button.textContent = kind === 'audio' ? 'Download audio' : 'Download Markdown';
      button.disabled = downloading || working || !!awaiting || !status || !['idle','menu'].includes(status.state) || status.stopping || status.connecting;
      button.addEventListener('click', () => downloadNote(note.id,kind)); row.appendChild(button); downloadButtons.push(button);
    }
    el('note-list').appendChild(row);
  }
}
async function downloadNote(id, kind) {
  if (!validNoteId(id) || !['audio','markdown'].includes(kind) || !token || !status || working || awaiting || downloading || !['idle','menu'].includes(status.state) || status.stopping || status.connecting) return;
  const generation = accessGeneration, credential = token;
  downloading = true; controls(); message('Downloading from SD…');
  try {
    const response = await fetch('/api/download/'+kind+'?id='+encodeURIComponent(id), {headers:{Authorization:'Bearer '+credential},cache:'no-store',signal:AbortSignal.timeout(120000)});
    if (generation !== accessGeneration || credential !== token) return;
    if (!response.ok) { if(response.status === 401) logout(); throw Error(response.status === 404 ? 'This saved file is unavailable.' : 'Download unavailable. Check access, SD and device state.'); }
    const blob = await response.blob();
    if (generation !== accessGeneration || credential !== token) return;
    const url = URL.createObjectURL(blob), link = document.createElement('a');
    link.href = url; link.download = id+(kind === 'audio' ? '.wav' : '.md');
    document.body.appendChild(link); link.click(); link.remove();
    setTimeout(function revokeDownload() { URL.revokeObjectURL(url); },1000);
    message('Download received. Audio on the SD card is unchanged.');
  } catch(error) { if(generation === accessGeneration) message(error.message || 'Download interrupted. Original file retained.',true); }
  finally { if(generation === accessGeneration) { downloading = false; controls(); } }
}
async function refresh() {
  clearTimeout(timer);
  if (!token) return;
  const generation = accessGeneration, pendingAtStart = awaiting;
  try {
    const incoming = await request('/api/status');
    // A poll begun before enqueue cannot complete or supersede that new command.
    if (awaiting !== pendingAtStart) return;
    const settings = await request('/api/settings');
    if (awaiting !== pendingAtStart) return;
    status = incoming;
    if (!loadedSettings) {
      el('home-ssid').value = settings.home_ssid || ''; el('hotspot-ssid').value = settings.hotspot_ssid || '';
      el('refresh-limit').value = String(settings.partial_limit ?? 10); loadedSettings = true;
      navigateSettings(settingsView);
    }
    el('station-status').textContent = settings.ip ? 'Observed IP: '+settings.ip+' / Network: '+(settings.current_ssid || '—') : 'Offline. No observed station IP.';
    el('saved-networks').textContent = 'Saved Home: '+(settings.home_ssid || 'Disabled')+' / Hotspot: '+(settings.hotspot_ssid || 'Disabled');
    el('storage-status').textContent = (settings.mounted ? 'SD mounted.' : 'SD unavailable. Recording is disabled.') + (settings.audio_ready === false ? ' Audio unavailable; recording is disabled.' : '');
    el('storage-error').textContent = settings.storage_error || '';
    const known = settings.usage_known && settings.total_bytes > 0 && settings.free_bytes >= 0 && settings.free_bytes <= settings.total_bytes;
    el('storage-gauge').hidden = !known;
    el('storage-gauge').value = known ? (settings.total_bytes-settings.free_bytes)/settings.total_bytes : 0;
    el('storage-usage').textContent = known ? ((settings.total_bytes-settings.free_bytes)/1048576).toFixed(1)+' MiB used / '+(settings.total_bytes/1048576).toFixed(1)+' MiB total' : 'Usage unknown';
    if (formatChallenge && (!settings.format_challenge || Date.now() >= formatExpires)) cancelFormat();
    if (!loadedConfig) {
      const config = await request('/api/config/server');
      if (!el('server-url').value) el('server-url').value = config.base_url;
      configRevision = config.server_revision || 0;
      el('token-state').textContent = config.token_configured ? 'Transcription token configured.' : 'Transcription token missing. Audio is still saved locally.';
      loadedConfig = true; el('server-panel').hidden = false;
    }
    if (!downloading && !awaiting) {
      const notes = await request('/api/notes');
      if (awaiting !== pendingAtStart) return;
      renderSavedNotes(notes.notes);
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
          if (config.base_url === savingUrl && config.command_id === awaiting && config.command_result === 'ok' && !config.command_busy && savingTarget && config.server_revision > savingTarget.revision && (!savingTarget.replace || config.token_configured === savingTarget.configured))
            { configRevision = config.server_revision; el('token-state').textContent = config.token_configured ? 'Transcription token configured.' : 'Transcription token missing.'; message('Server address saved. Live and final transcription use this address.'); }
          else message('Server address not confirmed. Reload access and check the current address.', true);
        } else message(result === 'rejected' ? 'State changed: server address not saved.' : 'Server address not saved. Previous address remains active. Try again.', true);
        savingUrl = ''; savingTarget = null;
      } else if (savingSettings) {
        const saved = savingSettings;
        const exact = settings.command_id === awaiting && settings.command_result === 'ok' && !settings.command_busy;
        if (result !== 'ok') message(saved.action === 'format' ? 'Format failed or rejected. Data may be erased if formatting started; check the storage status.' : 'Setting or action not saved/completed. Check device state and retry.', true);
        else if (!exact) message('Settings not confirmed. Reload access and check the current values.', true);
        else if (saved.action === 'wifi') message(settings.home_ssid === saved.ssids[0] && settings.hotspot_ssid === saved.ssids[1] ? 'Networks saved. Choose Reconnect to use them; the device address may change.' : 'Networks not confirmed. Check saved names before reconnecting.', settings.home_ssid !== saved.ssids[0] || settings.hotspot_ssid !== saved.ssids[1]);
        else if (saved.action === 'display') message(settings.partial_limit === saved.partial_limit ? 'Display setting saved.' : 'Display setting not confirmed.', settings.partial_limit !== saved.partial_limit);
        else if (saved.action === 'prepare_format') {
          if (settings.format_challenge && settings.mounted) { formatChallenge = settings.format_challenge; formatExpires = Date.now()+60000; el('format-confirm').hidden = false; el('cancel-format').focus(); message('Confirmation ready. Cancel is the default; no data erased yet.'); }
          else message('Format confirmation not available. No data erased.', true);
        } else if (saved.action === 'format') message(settings.mounted && !settings.format_challenge ? 'SD formatted. All data erased; note directories recreated.' : 'Format not confirmed. Check the device and card.', !settings.mounted || !!settings.format_challenge);
        else if (saved.action === 'mount') message(settings.mounted ? 'SD mounted.' : 'SD still unavailable. Check the reported error.', !settings.mounted);
        else message('Reconnect completed. Observed IP: '+(settings.ip || 'none'));
        savingSettings = null;
      } else message(result === 'ok' ? 'Command completed.'  : result === 'stopping' ? 'Stop requested. Finalizing.' : result === 'rejected' ? 'State changed: command not executed.' : 'Command failed. Check the device and SD card.', result === 'rejected' || result === 'failed');
      awaiting = 0;
    } else if (awaiting && status.command_id !== awaiting) {
      awaiting = 0; savingUrl = ''; savingSettings = null; cancelFormat();
      message('Another session controlled the device. Check its current state.', true);
    } else if (!awaiting && disconnected) message('Connection restored.');
    else if (!awaiting && el('message').textContent === 'Connecting…') message('Connected.');
    disconnected = false;
    el('token').setAttribute('aria-invalid','false');
    return true;
  } catch (error) {
    if (generation !== accessGeneration && !error.authFailure) return;
    disconnected = true;
    status = null; el('storage-gauge').hidden = true; el('storage-usage').textContent = 'Usage unknown';
    for (const id of ['ip','wifi','station-status','saved-networks','storage-status','storage-error']) el(id).textContent = '';
    message(error.message === 'Failed to fetch' || error.message === 'offline' || error.name === 'TimeoutError' ? 'Connection lost. Check Wi-Fi and the device address.' : error.message, true);
    el('state').textContent = 'Connection unavailable';
    el('token').setAttribute('aria-invalid','true');
    if (pairing && el('access-dialog').open) { el('access-message').textContent = el('message').textContent; el('access-message').dataset.state = 'error'; }
    return false;
  } finally { controls(); if (token && generation === accessGeneration) timer = setTimeout(refresh, 1500); }
}
el('access').addEventListener('submit', async event => {
  event.preventDefault();
  if (working) return;
  const credential = el('token').value;
  if (!/^[0-9]{6}$/.test(credential)) { el('access-message').textContent = 'Enter exactly six digits from the ESP32 screen.'; el('token').setAttribute('aria-invalid','true'); return; }
  logout(true); pairing = true; disconnected = false;
  const attempt = ++pairingAttempt;
  token = credential; el('token').value = '';
  working = true; controls(); message('Connecting…');
  el('access-message').textContent = 'Connecting…';
  const generation = accessGeneration;
  const connected = await refresh();
  if (attempt !== pairingAttempt) return;
  if (connected && generation === accessGeneration) { pairing = false; closeAccess(); }
  if (!connected && generation === accessGeneration) { token = ''; clearTimeout(timer); }
  pairing = false; working = false; controls();
  if (!connected && el('access-dialog').open) el('token').focus();
});
async function command(action) {
  if (!status || working || awaiting) return;
  const generation = accessGeneration;
  working = true; controls(); message('Sending command…');
  try { const result = await request('/api/command/' + action, 'POST'); awaiting = result.id; message('Command waiting for the device.'); }
  catch (error) {if (generation === accessGeneration || error.authFailure) { message(error.message || 'Connection lost. Refresh before trying again.',true); status = null; }}
  finally {if (generation === accessGeneration) {working = false; controls();}}
}
el('server-config').addEventListener('submit', async event => {
  event.preventDefault();
  if (!status || working || awaiting || !loadedConfig) return;
  const url = normalizeServerUrl(el('server-url').value);
  const newToken = el('server-token').value, clearToken = el('clear-server-token').checked;
  if (newToken.length > 192 || /[^\x21-\x7e]/.test(newToken) || (clearToken && newToken)) { message('Use up to 192 printable non-space token characters. To clear, leave the token blank and select Clear.', true); return; }
  if (!validServerUrl(url)) {message('Enter http:// or https:// followed by the server IPv4 and optional port (1–65535), without a path. Do not use the device address.', true); return;}
  const generation = accessGeneration;
  working = true; controls();
  try {
    const body = {base_url:url};
    if (newToken) body.token = newToken;
    if (clearToken) body.clear_token = true;
    el('server-token').value = ''; el('clear-server-token').checked = false;
    savingTarget = {revision:configRevision, replace:!!newToken || clearToken, configured:!!newToken};
    const result = await request('/api/config/server', 'POST', body);
    savingUrl = url; awaiting = result.id; message('Server address waiting for the device.');
  } catch (error) {if (generation === accessGeneration || error.authFailure) message(error.message, true);}
  finally {if (generation === accessGeneration) {working = false; controls();}}
});

function cancelFormat() { formatChallenge = 0; el('format-confirm').hidden = true; controls(); }
async function submitSettings(body, expected = {action:body.action}) {
  if (!status || working || awaiting || !loadedSettings) return;
  const generation = accessGeneration;
  working = true; controls();
  try {
    const result = await request('/api/settings', 'POST', body);
    savingSettings = expected; awaiting = result.id; message('Settings action waiting for the device.');
  } catch (error) { if (generation === accessGeneration) message(error.message, true); }
  finally { if (generation === accessGeneration) { working = false; controls(); } }
}
el('settings-nav').addEventListener('click', () => { navigateSettings(true); if (!token) openAccess(el('settings-nav')); });
el('notebook-nav').addEventListener('click', () => navigateSettings(false));
el('wifi-config').addEventListener('submit', async event => {
  event.preventDefault();
  const profiles = ['home','hotspot'].map(slot => ({ssid:el(slot+'-ssid').value, password:el(slot+'-password').value, open:el(slot+'-open').checked}));
  const bytes = text => new TextEncoder().encode(text).length;
  if (!profiles.some(p => p.ssid) || profiles.some(p => bytes(p.ssid)>32 || /[\x00-\x1f\x7f]/.test(p.ssid) || (p.password && (p.password.length<8 || p.password.length>63 || /[^\x20-\x7e]/.test(p.password))) || (p.open && p.password) || (!p.ssid && p.password))) {
    message('Keep at least one SSID (32 bytes maximum). Use an 8–63 character password or choose an open network explicitly.', true); return;
  }
  const promise = submitSettings({action:'wifi',profiles}, {action:'wifi',ssids:profiles.map(p=>p.ssid)});
  for (const slot of ['home','hotspot']) el(slot+'-password').value = '';
  await promise;
});
for (const slot of ['home','hotspot']) el(slot+'-open').addEventListener('change', () => { if (el(slot+'-open').checked) el(slot+'-password').value = ''; controls(); });
el('display-config').addEventListener('submit', event => { event.preventDefault(); const limit = Number(el('refresh-limit').value); if ([0,1,5,10,20,50,100].includes(limit)) return submitSettings({action:'display',partial_limit:limit}, {action:'display',partial_limit:limit}); });
el('retry-storage').addEventListener('click', () => submitSettings({action:'mount'}));
el('reconnect-wifi').addEventListener('click', () => { cancelFormat(); return submitSettings({action:'reconnect'}); });
el('prepare-format').addEventListener('click', () => { cancelFormat(); return submitSettings({action:'prepare_format'}); });
el('cancel-format').addEventListener('click', () => { cancelFormat(); el('prepare-format').focus(); message('Formatting cancelled. No erase request sent.'); });
el('erase-storage').addEventListener('click', () => {
  if (!formatChallenge || Date.now() >= formatExpires) { cancelFormat(); return; }
  const challenge = formatChallenge; cancelFormat(); return submitSettings({action:'format',challenge});
});

el('disconnect').addEventListener('click', () => {
  if (token) request('/api/session/close','POST').catch(() => {});
  logout(); message('Disconnected. Open web settings on the ESP32 for a new local code.');
});
el('record').addEventListener('click', () => command(status && status.state === 'recording' ? 'stop' : 'start'));
el('sync').addEventListener('click', () => command('sync'));
controls();
