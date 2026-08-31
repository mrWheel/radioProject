const state = { stationIndex: 0, stationCount: 0, volume: 0, line1: '-', line2: '-', line3: '-', playing: false, streamConnected: false, station: null };
let ws = null;
let wsReady = false;
let manageMode = 'view';
let pendingButtons = [];

function markPressed(btn) { if (!btn) return; btn.classList.remove('done'); btn.classList.add('pressed'); pendingButtons.push(btn); }
function resolvePending() { pendingButtons.forEach((btn) => { btn.classList.remove('pressed'); btn.classList.add('done'); setTimeout(() => btn.classList.remove('done'), 400); }); pendingButtons = []; }
function send(type, data, btn) { markPressed(btn); if (ws && wsReady) { ws.send(JSON.stringify({ type, data })); } else { resolvePending(); setStatus('Not connected'); } }

function connectWs() {
  ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
  ws.onopen = () => { wsReady = true; setStatus('Connected'); send('getState'); };
  ws.onclose = () => { wsReady = false; setStatus('Reconnecting…'); setTimeout(connectWs, 1000); };
  ws.onerror = () => { ws.close(); };
  ws.onmessage = (event) => {
    try {
      const msg = JSON.parse(event.data);
      if (msg.type === 'error') {
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
});
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

connectWs();
