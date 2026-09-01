const state = { stationIndex: 0, stationCount: 0, volume: 0, line1: '-', line2: '-', line3: '-', playing: false, streamConnected: false, station: null };
let ws = null;
let wsReady = false;
let manageMode = 'view';
let pendingButtons = [];

function markPressed(btn) { if (!btn) return; btn.classList.remove('done'); btn.classList.add('pressed'); pendingButtons.push(btn); }
function resolvePending() { pendingButtons.forEach((btn) => { btn.classList.remove('pressed'); btn.classList.add('done'); setTimeout(() => btn.classList.remove('done'), 400); }); pendingButtons = []; }
function send(type, data, btn) { markPressed(btn); if (ws && wsReady) { ws.send(JSON.stringify({ type, data })); } else { resolvePending(); setStatus('Not connected'); } }

//-- Halts once either the server explicitly reports a takeover, or the
//-- watchdog below decides the connection is stalled; either way, auto
//-- reconnect stops and the user must press Reconnect.
let connectionHalted = false;
//-- Timestamp of the last WS message received (any type, including pong),
//-- used by the stall watchdog. Reset on open too, so a connection that
//-- never answers getState is caught the same way as one that goes silent
//-- later. The server only broadcasts state on actual changes, so without
//-- the active ping/pong heartbeat below an idle-but-healthy connection
//-- would be wrongly flagged as stalled.
let lastMessageAt = Date.now();
const PING_INTERVAL_MS = 4000;
const STALL_TIMEOUT_MS = 9000;

function showConnectionModal(title) {
  connectionHalted = true;
  wsReady = false;
  lastMessageAt = Date.now();

  document.getElementById('stationName').textContent = 'Connection lost';
  document.getElementById('line1').textContent = '-';
  document.getElementById('line2').textContent = '-';
  document.getElementById('line3').textContent = '-';
  setStatus(title === 'Connection stalled' ? 'Reconnect required' : 'Connection lost');

  document.getElementById('takeoverModalTitle').textContent = title;
  document.getElementById('takeoverModal').classList.add('open');
}

function resetDisconnectedUi() {
  document.getElementById('stationName').textContent = 'No station';
  document.getElementById('line1').textContent = '-';
  document.getElementById('line2').textContent = '-';
  document.getElementById('line3').textContent = '-';
  setStatus('Waiting for connection');
}

const INFO_MODAL_AUTO_DISMISS_MS = 3000;
let infoModalTimer = null;

function showInfoModal(title) {
  document.getElementById('infoModalTitle').textContent = title;
  document.getElementById('infoModal').classList.add('open');
  if (infoModalTimer) clearTimeout(infoModalTimer);
  infoModalTimer = setTimeout(hideInfoModal, INFO_MODAL_AUTO_DISMISS_MS);
}

function hideInfoModal() {
  document.getElementById('infoModal').classList.remove('open');
  if (infoModalTimer) { clearTimeout(infoModalTimer); infoModalTimer = null; }
}

function connectWs() {
  ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
  ws.onopen = () => {
    wsReady = true;
    lastMessageAt = Date.now();
    connectionHalted = false;
    setStatus('Connected');
    send('getState');
  };
  ws.onclose = () => {
    wsReady = false;
    if (connectionHalted) return;
    resetDisconnectedUi();
    setStatus('Reconnecting…');
    setTimeout(connectWs, 1000);
  };
  ws.onerror = () => { ws.close(); };
  ws.onmessage = (event) => {
    lastMessageAt = Date.now();
    try {
      const msg = JSON.parse(event.data);
      if (msg.type === 'disconnected') {
        showConnectionModal('Connection lost or taken over');
        return;
      } else if (msg.type === 'error') {
        setStatus(msg.data && msg.data.message ? msg.data.message : 'Error');
      } else if (msg.data) {
        applyState(msg.data);
      }
    } catch (e) { /* ignore malformed frame */ }
    resolvePending();
  };
}

function setStatus(msg) { document.getElementById('status').textContent = msg; }

function applyState(data) {
  if (!data) return;
  state.stationIndex = Number(data.stationIndex ?? 0);
  state.stationCount = Number(data.stationCount ?? 0);
  state.volume = Number(data.volume ?? 0);
  state.playing = !!data.playing;
  state.streamConnected = !!data.streamConnected;
  state.station = data.station || null;
  if (state.station && state.station.name) document.getElementById('stationName').textContent = state.station.name;
  state.line1 = data.line1 ?? state.line1;
  state.line2 = data.line2 ?? state.line2;
  state.line3 = data.line3 ?? state.line3;
  const line1El = document.getElementById('line1');
  if (line1El.textContent !== state.line1) line1El.textContent = state.line1;
  const line2El = document.getElementById('line2');
  if (line2El.textContent !== state.line2) line2El.textContent = state.line2;
  const line3El = document.getElementById('line3');
  if (line3El.textContent !== state.line3) line3El.textContent = state.line3;
  const slider = document.getElementById('volumeSlider');
  slider.value = String(state.volume);
  document.getElementById('volumeValue').textContent = state.volume + '%';
  setStatus(state.streamConnected ? 'Connected' : 'Waiting for stream');
  document.getElementById('playBtn').classList.toggle('primary', state.playing);
  document.getElementById('pauseBtn').classList.toggle('primary', !state.playing);
  if (manageMode === 'view') { fillManageFormFromCurrentStation(); }
}

function fillManageFormFromCurrentStation() {
  document.getElementById('stationNameInput').value = state.station ? state.station.name : '';
  document.getElementById('stationUrlInput').value = state.station ? state.station.url : '';
  document.getElementById('stationCodecInput').value = state.station && state.station.codec ? state.station.codec : 'mp3';
}

function setManageFieldsEnabled(enabled) {
  document.getElementById('stationNameInput').disabled = !enabled;
  document.getElementById('stationUrlInput').disabled = !enabled;
  document.getElementById('stationCodecInput').disabled = !enabled;
}

function setManageMode(mode) {
  manageMode = mode;
  const editing = mode !== 'view';
  document.getElementById('stationViewActions').style.display = editing ? 'none' : 'flex';
  document.getElementById('stationEditActions').style.display = editing ? 'flex' : 'none';
  setManageFieldsEnabled(editing);
  document.getElementById('stationModalTitle').textContent = mode === 'new' ? 'New Station' : mode === 'edit' ? 'Edit Station' : 'Manage Stations';
}

document.getElementById('prevBtn').addEventListener('click', (e) => send('stationPrevious', null, e.currentTarget));
document.getElementById('nextBtn').addEventListener('click', (e) => send('stationNext', null, e.currentTarget));
document.getElementById('playBtn').addEventListener('click', (e) => send('play', null, e.currentTarget));
document.getElementById('pauseBtn').addEventListener('click', (e) => send('pause', null, e.currentTarget));
document.getElementById('manageBtn').addEventListener('click', (e) => { markPressed(e.currentTarget); resolvePending(); fillManageFormFromCurrentStation(); setManageMode('view'); document.getElementById('stationModal').classList.add('open'); });
document.getElementById('closeStationBtn').addEventListener('click', () => document.getElementById('stationModal').classList.remove('open'));
document.getElementById('editStationBtn').addEventListener('click', (e) => { markPressed(e.currentTarget); resolvePending(); fillManageFormFromCurrentStation(); setManageMode('edit'); });
document.getElementById('newStationBtn').addEventListener('click', (e) => { markPressed(e.currentTarget); resolvePending(); document.getElementById('stationNameInput').value = ''; document.getElementById('stationUrlInput').value = ''; document.getElementById('stationCodecInput').value = 'mp3'; setManageMode('new'); });
document.getElementById('cancelStationBtn').addEventListener('click', (e) => { markPressed(e.currentTarget); resolvePending(); fillManageFormFromCurrentStation(); setManageMode('view'); });
document.getElementById('deleteStationBtn').addEventListener('click', (e) => {
  if (!state.station) return;
  if (!confirm('Are you sure you want to delete \'' + state.station.name + '\'?')) return;
  send('stationDelete', null, e.currentTarget);
  document.getElementById('stationModal').classList.remove('open');
});
document.getElementById('saveStationBtn').addEventListener('click', (e) => {
  const name = document.getElementById('stationNameInput').value.trim();
  const url = document.getElementById('stationUrlInput').value.trim();
  const codec = document.getElementById('stationCodecInput').value;
  if (!name || !url) { setStatus('Name and URL are required'); return; }
  send(manageMode === 'new' ? 'stationAdd' : 'stationEdit', { name, url, codec }, e.currentTarget);
  setManageMode('view');
  document.getElementById('stationModal').classList.remove('open');
});
document.getElementById('downloadStationsBtn').addEventListener('click', (e) => {
  markPressed(e.currentTarget);
  const link = document.createElement('a');
  link.href = '/api/stations/export';
  link.download = 'stations.json';
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  resolvePending();
  showInfoModal('Stations downloaded successfully');
});
document.getElementById('infoModalOkBtn').addEventListener('click', hideInfoModal);
document.getElementById('uploadStationsBtn').addEventListener('click', (e) => { markPressed(e.currentTarget); resolvePending(); document.getElementById('stationsFileInput').click(); });
document.getElementById('stationsFileInput').addEventListener('change', async (event) => {
  const file = event.target.files[0];
  event.target.value = '';
  if (!file) return;
  try {
    const text = await file.text();
    const res = await fetch('/api/stations/import', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: text });
    const msg = await res.json();
    if (msg.type === 'state' && msg.data) {
      applyState(msg.data);
      setStatus('Stations imported');
      showInfoModal('Stations uploaded successfully');
    } else {
      setStatus((msg.data && msg.data.message) ? msg.data.message : 'Import failed');
    }
  } catch (e) {
    setStatus('Import failed');
  }
});
document.getElementById('volumeSlider').addEventListener('input', (event) => {
  const value = Number(event.target.value);
  document.getElementById('volumeValue').textContent = value + '%';
  send('volumeSet', { value });
});
document.addEventListener('keydown', (event) => {
  if (['INPUT', 'TEXTAREA', 'SELECT'].includes(document.activeElement.tagName)) return;
  if (event.key === 'ArrowUp') { event.preventDefault(); send('stationPrevious', null, document.getElementById('prevBtn')); }
  if (event.key === 'ArrowDown') { event.preventDefault(); send('stationNext', null, document.getElementById('nextBtn')); }
});
document.getElementById('reconnectBtn').addEventListener('click', () => {
  connectionHalted = false;
  wsReady = false;
  lastMessageAt = Date.now();
  if (ws) {
    try { ws.close(); } catch (e) { /* ignore */ }
  }
  document.getElementById('takeoverModal').classList.remove('open');
  resetDisconnectedUi();
  setStatus('Reconnecting…');
  location.reload();
});

//-- Active heartbeat: the server only broadcasts state on real changes, so
//-- during idle periods this is the only traffic keeping lastMessageAt
//-- fresh. Answered inline by the server (not queued), so it never competes
//-- with or delays the audio path.
setInterval(() => {
  if (wsReady && !connectionHalted) send('ping');
}, PING_INTERVAL_MS);

//-- Stall watchdog: covers both "never got a first reply" (stuck on Loading)
//-- and "went silent after connecting" (dead socket the browser hasn't
//-- noticed yet) - both leave wsReady/onclose unaware anything is wrong.
setInterval(() => {
  if (connectionHalted) return;
  if (Date.now() - lastMessageAt > STALL_TIMEOUT_MS) {
    showConnectionModal('Connection stalled');
    if (ws) { try { ws.close(); } catch (e) { /* already closed */ } }
  }
}, 2000);

connectWs();
