<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>VW CAN · Live</title>
{{include:_head_common.html}}<style>
/* ── MAIN ── */
.main{padding:12px;max-width:600px;margin:0 auto}

/* ── STATUS BAR ── */
.status-bar{
  display:flex;align-items:center;gap:8px;padding:8px 12px;
  background:var(--bg2);border:1px solid var(--border);border-radius:6px;
  margin-bottom:12px;font-size:11px;color:var(--muted);
}
.status-dot{width:7px;height:7px;border-radius:50%;background:var(--muted);flex-shrink:0}
.status-dot.active{background:var(--green);box-shadow:0 0 6px var(--green)}
.status-dot.warn{background:var(--yellow);box-shadow:0 0 6px var(--yellow)}

/* ── ECU GRUPPE ── */
.ecu-group{margin-bottom:16px}
.ecu-label{
  font-size:10px;font-weight:700;letter-spacing:.1em;color:var(--muted);
  text-transform:uppercase;padding:0 2px 6px;border-bottom:1px solid var(--border);
  margin-bottom:8px;
}

/* ── DID GRID ── */
.did-grid{
  display:grid;grid-template-columns:1fr 1fr;gap:8px;
}
.did-card{
  background:var(--bg2);border:1px solid var(--border);border-radius:8px;
  padding:10px 12px;transition:border-color .2s;position:relative;overflow:hidden;
}
.did-card.updated{border-color:var(--accent);animation:flash .4s ease}
@keyframes flash{0%{background:rgba(0,229,255,.12)}100%{background:var(--bg2)}}
.did-name{font-size:10px;color:var(--muted);margin-bottom:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.did-value{font-size:18px;font-weight:700;color:var(--text);line-height:1.1}
.did-value.has-data{color:var(--accent)}
.did-value.no-data{color:var(--muted);font-size:13px}
.did-unit{font-size:10px;color:var(--muted);margin-top:2px}
.did-hex{font-size:9px;color:var(--border);margin-top:4px;font-family:monospace}
.did-id{
  position:absolute;top:6px;right:8px;font-size:9px;
  color:var(--border);font-family:monospace;
}

/* ── NO DATA STATE ── */
.no-data-msg{
  text-align:center;padding:40px 20px;color:var(--muted);font-size:12px;
}
.no-data-msg .icon{font-size:32px;margin-bottom:8px}
</style>
</head>
<body>
{{include:_header.html}}
<div class="main">

  <div class="status-bar">
    <div class="status-dot" id="status-dot"></div>
    <span id="status-text">Warte auf ABRP Verbindung…</span>
    <span style="flex:1"></span>
    <span id="last-update" style="font-size:10px"></span>
  </div>

  <!-- ECU FC007B -->
  <div class="ecu-group">
    <div class="ecu-label">ECU FC007B · Motor / Antrieb</div>
    <div class="did-grid">
      <div class="did-card" id="did-1E3D">
        <div class="did-id">22 1E 3D</div>
        <div class="did-name">Unbekannt 1E3D</div>
        <div class="did-value no-data" id="val-1E3D">—</div>
        <div class="did-unit" id="unit-1E3D"></div>
        <div class="did-hex" id="hex-1E3D"></div>
      </div>
      <div class="did-card" id="did-1E3B">
        <div class="did-id">22 1E 3B</div>
        <div class="did-name">Unbekannt 1E3B</div>
        <div class="did-value no-data" id="val-1E3B">—</div>
        <div class="did-unit" id="unit-1E3B"></div>
        <div class="did-hex" id="hex-1E3B"></div>
      </div>
      <div class="did-card" id="did-028C">
        <div class="did-id">22 02 8C</div>
        <div class="did-name">Batterie?</div>
        <div class="did-value no-data" id="val-028C">—</div>
        <div class="did-unit" id="unit-028C"></div>
        <div class="did-hex" id="hex-028C"></div>
      </div>
      <div class="did-card" id="did-7448">
        <div class="did-id">22 74 48</div>
        <div class="did-name">Temperatur?</div>
        <div class="did-value no-data" id="val-7448">—</div>
        <div class="did-unit" id="unit-7448"></div>
        <div class="did-hex" id="hex-7448"></div>
      </div>
      <div class="did-card" id="did-2A0B">
        <div class="did-id">22 2A 0B</div>
        <div class="did-name">Strom?</div>
        <div class="did-value no-data" id="val-2A0B">—</div>
        <div class="did-unit" id="unit-2A0B"></div>
        <div class="did-hex" id="hex-2A0B"></div>
      </div>
      <div class="did-card" id="did-F40D">
        <div class="did-id">22 F4 0D</div>
        <div class="did-name">Unbekannt F40D</div>
        <div class="did-value no-data" id="val-F40D">—</div>
        <div class="did-unit" id="unit-F40D"></div>
        <div class="did-hex" id="hex-F40D"></div>
      </div>
      <div class="did-card" id="did-1E32">
        <div class="did-id">22 1E 32</div>
        <div class="did-name">Unbekannt 1E32</div>
        <div class="did-value no-data" id="val-1E32">—</div>
        <div class="did-unit" id="unit-1E32"></div>
        <div class="did-hex" id="hex-1E32"></div>
      </div>
    </div>
  </div>

  <!-- ECU FC0076 -->
  <div class="ecu-group">
    <div class="ecu-label">ECU FC0076 · Batterie</div>
    <div class="did-grid">
      <div class="did-card" id="did-210E">
        <div class="did-id">22 21 0E</div>
        <div class="did-name">Unbekannt 210E</div>
        <div class="did-value no-data" id="val-210E">—</div>
        <div class="did-unit" id="unit-210E"></div>
        <div class="did-hex" id="hex-210E"></div>
      </div>
      <div class="did-card" id="did-295A">
        <div class="did-id">22 29 5A</div>
        <div class="did-name">Unbekannt 295A</div>
        <div class="did-value no-data" id="val-295A">—</div>
        <div class="did-unit" id="unit-295A"></div>
        <div class="did-hex" id="hex-295A"></div>
      </div>
      <div class="did-card" id="did-0364">
        <div class="did-id">22 03 64</div>
        <div class="did-name">Unbekannt 0364</div>
        <div class="did-value no-data" id="val-0364">—</div>
        <div class="did-unit" id="unit-0364"></div>
        <div class="did-hex" id="hex-0364"></div>
      </div>
    </div>
  </div>

  <!-- ECU 710 -->
  <div class="ecu-group">
    <div class="ecu-label">ECU 710 · BMS?</div>
    <div class="did-grid">
      <div class="did-card" id="did-2AB2">
        <div class="did-id">22 2A B2</div>
        <div class="did-name">Unbekannt 2AB2</div>
        <div class="did-value no-data" id="val-2AB2">—</div>
        <div class="did-unit" id="unit-2AB2"></div>
        <div class="did-hex" id="hex-2AB2"></div>
      </div>
    </div>
  </div>

  <!-- ECU 746 -->
  <div class="ecu-group">
    <div class="ecu-label">ECU 746</div>
    <div class="did-grid">
      <div class="did-card" id="did-2613">
        <div class="did-id">22 26 13</div>
        <div class="did-name">Unbekannt 2613</div>
        <div class="did-value no-data" id="val-2613">—</div>
        <div class="did-unit" id="unit-2613"></div>
        <div class="did-hex" id="hex-2613"></div>
      </div>
      <div class="did-card" id="did-2609">
        <div class="did-id">22 26 09</div>
        <div class="did-name">Unbekannt 2609</div>
        <div class="did-value no-data" id="val-2609">—</div>
        <div class="did-unit" id="unit-2609"></div>
        <div class="did-hex" id="hex-2609"></div>
      </div>
    </div>
  </div>

</div>

<script>
{{include:_led_js.html}}
// ── WebSocket ──────────────────────────────────────────────
let ws;
let bleConnected = false;

function connectWS() {
  ws = new WebSocket('ws://' + location.hostname + '/ws');
  ws.onopen  = () => {};
  ws.onclose = () => { setBleStatus(false); setTimeout(connectWS, 2000); };
  ws.onmessage = (e) => {
    const msg = JSON.parse(e.data);
    if (msg.type === 'log') {
      flashLed(msg.e.tx ? 'tx' : 'rx', msg.e.tx ? 'red' : 'blue');
      if (msg.e.src === 1) handleElmFrame(msg.e);
    } else if (msg.type === 'status') {
      document.getElementById('uptime').textContent = Math.floor(msg.uptime/1000)+'s';
    } else if (msg.type === 'ble_status') {
      setBleStatus(msg.connected);
    }
  };
}

// ── BLE Status ─────────────────────────────────────────────
function setBleStatus(connected) {
  bleConnected = connected;
  setLed('ble', connected ? 'blue' : '');
  const dot = document.getElementById('status-dot');
  const txt = document.getElementById('status-text');
  if (connected) {
    dot.className = 'status-dot active';
    txt.textContent = 'ABRP verbunden — empfange Daten…';
  } else {
    dot.className = 'status-dot';
    txt.textContent = 'Warte auf ABRP Verbindung…';
  }
}

// ── ELM Frame verarbeiten ──────────────────────────────────
function handleElmFrame(e) {
  if (e.tx) return;
  const info = e.dec || '';
  const m = info.match(/DID=0x22([0-9A-Fa-f]{4})/);
  if (!m) return;
  const did = m[1].toUpperCase();
  updateCard(did, e.hex);
}

function updateCard(did, hex) {
  const valEl  = document.getElementById('val-' + did);
  const hexEl  = document.getElementById('hex-' + did);
  const card   = document.getElementById('did-' + did);
  if (!valEl) return;

  if (hexEl) hexEl.textContent = hex;

  const decoded = decode(did, hex);
  if (decoded) {
    valEl.textContent = decoded.value;
    valEl.className = 'did-value has-data';
    const unitEl = document.getElementById('unit-' + did);
    if (unitEl) unitEl.textContent = decoded.unit || '';
  } else {
    valEl.textContent = hex || 'NO DATA';
    valEl.className = hex ? 'did-value has-data' : 'did-value no-data';
  }

  if (card) {
    card.classList.remove('updated');
    void card.offsetWidth;
    card.classList.add('updated');
  }

  const now = new Date();
  document.getElementById('last-update').textContent =
    now.getHours().toString().padStart(2,'0') + ':' +
    now.getMinutes().toString().padStart(2,'0') + ':' +
    now.getSeconds().toString().padStart(2,'0');

  setBleStatus(true);
}

// ── Dekodierung ────────────────────────────────────────────
function decode(did, hex) {
  if (!hex || hex.trim() === 'NO DATA') return null;
  const bytes = hex.trim().split(' ').map(b => parseInt(b, 16));
  if (bytes.length < 4) return null;
  const data = bytes.slice(3);

  switch(did) {
    // case '1E3D': return { value: (data[0] * 0.5).toFixed(1), unit: '%' };
    // case '028C': return { value: ((data[0]<<8|data[1]) * 0.1).toFixed(1), unit: 'V' };
    default: return null;
  }
}

connectWS();

(async () => {
  try {
    const d = await (await fetch('/status')).json();
    if (!d.rtc_ok) {
      const n = new Date();
      fetch('/wifi/set-time', {method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify({hour:n.getHours(), minute:n.getMinutes(), second:n.getSeconds()})});
    }
  } catch(e) {}
})();

window.addEventListener('pagehide', () => {
  if (ws) { ws.onclose = null; ws.close(); ws = null; }
});
window.addEventListener('pageshow', (e) => {
  if (e.persisted) {
    if (ws) { ws.onclose = null; ws.close(); ws = null; }
    setTimeout(connectWS, 300);
  }
});
</script>
</body>
</html>
