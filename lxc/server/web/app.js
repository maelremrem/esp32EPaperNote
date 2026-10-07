/* Carnet: bearer token lives only in this closure; all stored text is inert. */
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const statuses = {done: 'Transcribed', processing: 'Processing', error: 'Failed'};
  let token = '', session = 0, selected = null, offset = 0, audioUrl = null;
  const controllers = new Map();
  const downloadUrls = new Set();
  const date = value => {
    if (!value) return 'Unknown date';
    const parsed = new Date(value.includes('T') ? value : value.replace(' ', 'T') + 'Z');
    return Number.isNaN(parsed.getTime()) ? 'Unknown date' : new Intl.DateTimeFormat('en-GB', {dateStyle: 'medium', timeStyle: 'short'}).format(parsed);
  };
  function message(id, text, error = false) {
    $(id).textContent = text;
    $(id).setAttribute('data-error', String(error));
  }
  function abort(key) { controllers.get(key)?.abort(); controllers.delete(key); }
  function clearAudio() {
    abort('audio');
    $('audio').pause(); $('audio').removeAttribute('src'); $('audio').load();
    $('audio').hidden = true;
    $('download-audio').removeAttribute('href'); $('download-audio').hidden = true;
    if (audioUrl) URL.revokeObjectURL(audioUrl);
    audioUrl = null;
    $('load-audio').disabled = false;
    $('load-audio').textContent = 'Load audio';
    message('audio-message', 'Audio is loaded on demand using your token.');
  }
  function clearReader() {
    abort('detail'); clearAudio(); selected = null;
    $('note-content').hidden = true; $('reader-empty').hidden = false;
    for (const id of ['transcript', 'reader-title', 'metadata', 'note-status', 'detail-message']) $(id).textContent = '';
    $('copy').textContent = 'Copy text';
  }
  function lock(text = 'Library locked. Enter your token to reopen it.') {
    session++; token = '';
    for (const [key, controller] of controllers) {
      if (key !== 'health') { controller.abort(); controllers.delete(key); }
    }
    clearReader();
    for (const url of downloadUrls) URL.revokeObjectURL(url);
    downloadUrls.clear();
    $('notes').replaceChildren(); $('count').textContent = ''; $('page').textContent = '';
    $('search').value = ''; $('status').value = ''; offset = 0;
    message('list-message', '');
    $('token').value = ''; $('library').hidden = true; $('lock').hidden = false; $('logout').hidden = true;
    $('unlock').disabled = false; $('unlock').textContent = 'Open library';
    message('login-message', text);
  }
  async function request(url, key, privateRequest = true, asBlob = false) {
    abort(key);
    const controller = new AbortController(); controllers.set(key, controller);
    const generation = session;
    try {
      const response = await fetch(url, {signal: controller.signal, cache: 'no-store', headers: privateRequest ? {Authorization: 'Bearer ' + token} : {}});
      if (controller.signal.aborted || (privateRequest && generation !== session)) throw new DOMException('Cancelled', 'AbortError');
      if (privateRequest && (response.status === 401 || response.status === 403)) {
        lock('Token rejected or expired. Check the server token and try again.');
        $('token').setAttribute('aria-invalid', 'true'); $('token').focus();
        throw new DOMException('Cancelled', 'AbortError');
      }
      if (!response.ok) throw new Error(response.status === 404 ? 'Not found' : 'HTTP ' + response.status);
      const result = await (asBlob ? response.blob() : response.json());
      if (controller.signal.aborted || (privateRequest && generation !== session)) throw new DOMException('Cancelled', 'AbortError');
      return result;
    } catch (error) {
      if (controller.signal.aborted || (privateRequest && generation !== session)) throw new DOMException('Cancelled', 'AbortError');
      throw error;
    } finally { if (controllers.get(key) === controller) controllers.delete(key); }
  }
  async function health() {
    message('health', 'Checking service…');
    try {
      const result = await request('/health', 'health', false);
      message('health', result.status === 'ok' ? 'Service available · ' + (result.model_loaded ? 'Whistle ready' : 'Whistle not loaded') : 'Service degraded');
    } catch (error) { if (error.name !== 'AbortError') message('health', 'Service unreachable', true); }
  }
  function element(tag, text, className) {
    const node = document.createElement(tag); node.textContent = text;
    if (className) node.className = className;
    return node;
  }
  function renderNotes(items) {
    $('notes').replaceChildren();
    for (const note of items) {
      const item = document.createElement('li');
      const button = document.createElement('button'); button.type = 'button'; button.className = 'note';
      button.setAttribute('aria-pressed', 'false');
      const meta = element('span', '', 'note-meta');
      meta.append(element('span', date(note.created_at)), element('span', statuses[note.status] || note.status));
      button.append(meta, element('span', note.text?.trim() || (note.status === 'processing' ? 'Transcribing…' : note.status === 'error' ? 'Transcription failed.' : 'No transcribed text.'), 'excerpt'), element('span', note.id, 'note-id'));
      button.addEventListener('click', () => openNote(note.id, button));
      item.append(button); $('notes').append(item);
    }
  }
  async function list() {
    if (!token) return false;
    clearReader(); $('notes').replaceChildren(); $('count').textContent = '';
    $('previous').disabled = true; $('next').disabled = true; $('page').textContent = '';
    $('notes').setAttribute('aria-busy', 'true');
    message('list-message', 'Loading notes…');
    const params = new URLSearchParams({limit: '30', offset: String(offset), q: $('search').value.trim()});
    if ($('status').value) params.set('status', $('status').value);
    try {
      const result = await request('/api/v1/notes?' + params, 'list');
      $('library').hidden = false; $('lock').hidden = true; $('logout').hidden = false;
      renderNotes(result.items);
      $('count').textContent = result.total + (result.total === 1 ? ' note' : ' notes');
      message('list-message', result.total ? '' : ($('search').value || $('status').value ? 'No matching notes. Change your search or choose all statuses.' : 'No archived notes. Record a note on your Carnet, then refresh this page.'));
      $('previous').disabled = offset === 0;
      $('next').disabled = offset + result.items.length >= result.total || offset + 30 > 1000000;
      $('page').textContent = result.total ? (offset + 1) + '–' + (offset + result.items.length) + ' / ' + result.total : '';
      $('notes').setAttribute('aria-busy', 'false');
      return true;
    } catch (error) {
      if (error.name !== 'AbortError') {
        message($('library').hidden ? 'login-message' : 'list-message', 'Cannot load notes. Check the server connection, then refresh.', true);
        $('notes').setAttribute('aria-busy', 'false');
      }
      return false;
    }
  }
  async function openNote(id, button) {
    clearReader();
    for (const item of $('notes').children) item.children[0].setAttribute('aria-pressed', String(item.children[0] === button));
    $('reader-empty').hidden = true; $('note-content').hidden = false;
    $('reader-title').textContent = id;
    message('detail-message', 'Loading transcript…');
    try {
      const note = await request('/api/v1/notes/' + encodeURIComponent(id), 'detail');
      selected = note;
      $('reader-title').focus();
      $('note-status').textContent = statuses[note.status] || note.status;
      $('metadata').textContent = 'Created ' + date(note.created_at) + ' · Updated ' + date(note.updated_at) + (note.duration != null ? ' · ' + new Intl.NumberFormat('en-GB', {maximumFractionDigits: 1}).format(note.duration) + ' s' : '') + (note.language ? ' · ' + note.language : '') + (note.model ? ' · ' + note.model : '');
      $('transcript').textContent = note.text || '';
      $('copy').disabled = !note.text; $('download-text').disabled = !note.text;
      message('detail-message', note.status === 'processing' ? 'Transcribing. Refresh to check its status.' : note.status === 'error' ? 'Transcription failed. The recording may still be available.' : note.text ? '' : 'This note has no transcribed text.', note.status === 'error');
    } catch (error) { if (error.name !== 'AbortError') { message('detail-message', 'Note unavailable. Refresh the list and try again.', true); $('copy').disabled = true; $('download-text').disabled = true; } }
  }
  $('login-form').addEventListener('submit', async event => {
    event.preventDefault();
    const value = $('token').value.trim(); if (!value) return;
    lock(); token = value;
    $('token').removeAttribute('aria-invalid');
    $('unlock').disabled = true; $('unlock').textContent = 'Opening…';
    message('login-message', 'Checking access…');
    await list();
    $('unlock').disabled = false; $('unlock').textContent = 'Open library';
  });
  $('logout').addEventListener('click', () => { lock(); $('token').focus(); });
  $('refresh').addEventListener('click', async () => { await Promise.all([health(), list()]); });
  $('search-form').addEventListener('submit', event => { event.preventDefault(); offset = 0; list(); });
  $('status').addEventListener('change', () => { offset = 0; list(); });
  $('previous').addEventListener('click', () => { offset = Math.max(0, offset - 30); list(); });
  $('next').addEventListener('click', () => { offset += 30; list(); });
  $('copy').addEventListener('click', async () => {
    if (!selected?.text) return;
    const generation = session, id = selected.id;
    try {
      await navigator.clipboard.writeText(selected.text);
      if (generation === session && selected?.id === id) $('copy').textContent = 'Text copied';
    } catch { if (generation === session && selected?.id === id) message('detail-message', 'Copy unavailable. Use HTTPS or export the text as .txt.', true); }
  });
  function download(blob, filename) {
    const url = URL.createObjectURL(blob), link = document.createElement('a');
    downloadUrls.add(url);
    link.href = url; link.download = filename; link.click();
    setTimeout(() => { if (downloadUrls.delete(url)) URL.revokeObjectURL(url); }, 1000);
  }
  $('download-text').addEventListener('click', () => { if (selected?.text) download(new Blob([selected.text], {type: 'text/plain;charset=utf-8'}), selected.id + '.txt'); });
  $('load-audio').addEventListener('click', async () => {
    if (!selected) return;
    const id = selected.id;
    clearAudio(); $('load-audio').disabled = true; $('load-audio').textContent = 'Loading…';
    message('audio-message', 'Loading recording…');
    try {
      const blob = await request('/api/v1/notes/' + encodeURIComponent(id) + '/audio', 'audio', true, true);
      audioUrl = URL.createObjectURL(blob);
      $('audio').src = audioUrl; $('audio').hidden = false;
      $('download-audio').href = audioUrl; $('download-audio').download = id + '.wav'; $('download-audio').hidden = false;
      $('load-audio').textContent = 'Audio loaded';
      message('audio-message', 'The recording is ready to play.');
    } catch (error) {
      if (error.name !== 'AbortError') { $('load-audio').disabled = false; $('load-audio').textContent = 'Retry audio'; message('audio-message', error.message === 'Not found' ? 'Recording missing or unavailable on the server.' : 'Cannot load audio. Check the connection and try again.', true); }
    }
  });
  window.addEventListener('pagehide', () => lock());
  health();
})();
