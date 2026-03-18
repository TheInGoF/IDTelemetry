<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>VW CAN Scanner</title>
{{include:_head_common.html}}<style>
body::before{content:'';position:fixed;inset:0;background:repeating-linear-gradient(0deg,transparent,transparent 2px,rgba(0,229,255,.015) 2px,rgba(0,229,255,.015) 4px);pointer-events:none;z-index:99}
.layout{display:grid;grid-template-columns:360px 1fr;height:calc(100vh - 53px);overflow:hidden}

/* LEFT PANEL */
.left{overflow-y:auto;padding:14px;display:flex;flex-direction:column;gap:10px;border-right:1px solid var(--border);background:var(--bg1)}
.section{background:var(--bg2);border:1px solid var(--border);border-radius:6px;padding:12px;display:flex;flex-direction:column;gap:8px}
.section h3{font-family:'Syne',sans-serif;font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:1.5px;border-bottom:1px solid var(--border);padding-bottom:6px;margin-bottom:2px}
label{font-size:11px;color:var(--muted);display:block;margin-bottom:3px}
input,select{width:100%;background:var(--bg0);border:1px solid var(--border);color:var(--text);padding:7px 10px;border-radius:4px;font-family:monospace;font-size:12px}
input:focus,select:focus{outline:none;border-color:var(--accent)}
.btn{font-family:'Space Mono',monospace;font-size:11px;padding:7px 12px;border-radius:4px;border:1px solid var(--border);background:var(--bg3);color:var(--text);cursor:pointer;transition:all .15s;white-space:nowrap;width:100%;text-align:center}
.btn:hover{background:var(--bg0);border-color:var(--accent);color:var(--accent)}
.btn.blue{background:#1f6feb;color:#fff;border-color:#1f6feb}
.btn.blue:hover{background:#388bfd}
.btn.green{background:#238636;color:#fff;border-color:#238636}
.btn.green:hover{background:#2ea043}
.btn.red{background:#da3633;color:#fff;border-color:#da3633}
.btn.red:hover{background:#f85149}
.btn.orange{background:#9c4d00;color:var(--yellow);border-color:#ff6b35}
.btn.orange:hover{background:#b35900}
.btn-row{display:flex;gap:6px}
.btn-row .btn{flex:1}

/* Preset tags */
.tags{display:flex;flex-wrap:wrap;gap:5px}
.tag{background:var(--bg0);border:1px solid var(--border);border-radius:3px;padding:3px 7px;font-size:10px;cursor:pointer;color:var(--muted);transition:all .15s}
.tag:hover{border-color:var(--accent);color:var(--accent)}
.tag.vag{border-color:#444;color:var(--yellow)}
.tag.vag:hover{border-color:var(--yellow)}

/* Scan Progress */
.progress-wrap{display:none}
.progress-track{height:6px;background:var(--bg0);border-radius:3px;overflow:hidden;margin:4px 0}
.progress-fill{height:100%;background:linear-gradient(90deg,var(--accent),var(--green));border-radius:3px;width:0%;transition:width .3s}
.scan-msg{font-size:11px;color:var(--accent);min-height:16px}

/* RIGHT PANEL: Log */
.right{display:flex;flex-direction:column;overflow:hidden;background:var(--bg0)}
.log-toolbar{padding:8px 14px;background:var(--bg1);border-bottom:1px solid var(--border);display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.log-toolbar .btn{width:auto;padding:5px 12px}
.log-count{font-size:11px;color:var(--muted);margin-left:auto}
.log-header{display:grid;grid-template-columns:80px 32px 70px 1fr 180px;gap:6px;padding:5px 10px;background:var(--bg2);border-bottom:1px solid var(--border);font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.8px}
.log-area{flex:1;overflow-y:auto;font-size:11px}
.log-row{display:grid;grid-template-columns:80px 32px 70px 1fr 180px;gap:6px;padding:5px 10px;border-bottom:1px solid rgba(42,58,74,.4);align-items:center;cursor:default}
.elm-row{background:rgba(255,180,0,0.06)}
.elm-tag{font-size:.65rem;color:#ffb400;background:rgba(255,180,0,.15);padding:1px 4px;border-radius:3px;font-weight:600}
.log-row:hover{background:var(--bg2)}
.ts{color:var(--muted)}
.dir-tx{color:#79c0ff;font-size:10px}
.dir-rx{color:var(--green);font-size:10px}
.cid{color:var(--yellow);font-weight:700}
.hex{letter-spacing:1px}
.dec{color:var(--accent);font-size:10px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}

::-webkit-scrollbar{width:5px}
::-webkit-scrollbar-track{background:var(--bg0)}
::-webkit-scrollbar-thumb{background:var(--border);border-radius:3px}
@keyframes fadeIn{from{opacity:0;transform:translateY(3px)}to{opacity:1;transform:none}}
.log-row{animation:fadeIn .08s ease}
</style>
</head>
<body>
{{include:_header.html}}
<div class="layout">
<!-- ===== LEFT ===== -->
<div class="left">

  <!-- 1: Manueller Frame -->
  <div class="section">
    <h3>Manueller CAN Frame</h3>
    <div>
      <label>Request CAN-ID (HEX)</label>
      <input id="tx-id" value="7DF">
    </div>
    <div>
      <label>Daten Bytes (HEX, Leerzeichen)</label>
      <input id="tx-hex" value="02 01 00 00 00 00 00 00">
    </div>
    <div>
      <label>Erwartete Response-ID (0 = alle)</label>
      <input id="rx-id" value="7E8">
    </div>
    <button class="btn blue" onclick="sendManual()">▶ Senden</button>
  </div>

  <!-- 2: UDS Anfrage -->
  <div class="section">
    <h3>UDS Service 0x22 — Read DID</h3>
    <div>
      <label>Steuergerät (VAG Adresse)</label>
      <select id="uds-module" onchange="onModuleChange()">
        <option value="">— Modul wählen —</option>
      </select>
    </div>
    <div class="btn-row">
      <div style="flex:1">
        <label>Request-ID</label>
        <input id="uds-req-id" value="7E6" style="font-size:12px">
      </div>
      <div style="flex:1">
        <label>Response-ID</label>
        <input id="uds-resp-id" value="7EE" style="font-size:12px">
      </div>
    </div>
    <div>
      <label>DID (2 Byte HEX, z.B. F190)</label>
      <input id="uds-did" value="F190">
    </div>
    <button class="btn blue" onclick="sendUDS()">▶ UDS Read DID senden</button>

    <!-- Schnellwahl VW MEB DIDs -->
    <label style="margin-top:4px">VW MEB Bekannte DIDs</label>
    <div class="tags" id="did-tags"></div>
  </div>

  <!-- 3: Tester Present -->
  <div class="section">
    <h3>Session Management</h3>
    <button class="btn" onclick="sendTesterPresent()">📡 Tester Present senden</button>
    <button class="btn" onclick="sendDiagSession()">🔑 Extended Diag Session (0x10 0x03)</button>
  </div>

  <!-- 4: Auto-Scan -->
  <div class="section">
    <h3>Auto-Scan</h3>
    <div class="progress-wrap" id="progress-wrap">
      <div class="scan-msg" id="scan-msg">--</div>
      <div class="progress-track"><div class="progress-fill" id="progress-fill"></div></div>
      <div style="font-size:10px;color:var(--muted)" id="progress-txt">0/0</div>
    </div>

    <button class="btn orange" onclick="startScan(1)">🔍 VAG Module-Scan<br><span style="font-size:10px;font-weight:400">Alle ECUs anpingen (VIN-Request)</span></button>
    <button class="btn orange" onclick="startScan(2)">📊 VW MEB DID-Scan<br><span style="font-size:10px;font-weight:400">Bekannte DIDs am gewählten Modul</span></button>
    <button class="btn orange" onclick="startScan(3)">🔢 OBD2 Brute-Force<br><span style="font-size:10px;font-weight:400">PID 0x00–0xFF (Mode 01, 7DF)</span></button>
    <button class="btn red" id="btn-abort" style="display:none" onclick="abortScan()">⛔ Scan abbrechen</button>
  </div>

  <!-- 5: OBD2 Schnellwahl -->
  <div class="section">
    <h3>OBD2 Standard (7DF)</h3>
    <div class="tags" id="obd-tags"></div>
  </div>

  <!-- 6: Log & CAN -->
  <div class="section">
    <h3>Werkzeuge</h3>
    <label>CAN Bitrate</label>
    <select id="bitrate">
      <option value="125">125 kbps</option>
      <option value="250">250 kbps</option>
      <option value="500" selected>500 kbps (OBD Standard)</option>
      <option value="1000">1 Mbps</option>
    </select>
    <button class="btn" onclick="restartCAN()">🔄 CAN Neustart</button>
    <div class="btn-row" style="margin-top:4px">
      <button class="btn green" onclick="downloadLog()">⬇ Log TXT</button>
      <button class="btn" onclick="clearLog()">🗑 Log löschen</button>
    </div>
    <label style="margin-top:4px">
      <input type="checkbox" id="monitor-cb" onchange="toggleMonitor()" style="width:auto;margin-right:6px">
      Monitor: alle Frames loggen
    </label>
    <label>
      <input type="checkbox" id="autoscroll" checked style="width:auto;margin-right:6px">
      Auto-Scroll
    </label>
  </div>

</div>

<!-- ===== RIGHT: LOG ===== -->
<div class="right">
  <div class="log-toolbar">
    <span style="font-size:11px;color:var(--muted)">Filter:</span>
    <input style="width:160px;padding:4px 8px;font-size:11px" id="log-filter" placeholder="ID / HEX / Info..." oninput="filterLog()">
    <button class="btn" onclick="setDir('all')" id="btn-all">Alle</button>
    <button class="btn" onclick="setDir('tx')" id="btn-tx">TX</button>
    <button class="btn" onclick="setDir('rx')" id="btn-rx">RX</button>
    <span class="log-count" id="log-count">0 Frames</span>
  </div>
  <div class="log-header">
    <span>Zeit (ms)</span><span>Dir</span><span>CAN-ID</span><span>Bytes (HEX)</span><span>Info / Decoded</span>
  </div>
  <div class="log-area" id="log-area"></div>
</div>
</div>

<script>
{{include:_led_js.html}}
// ===== VAG MODULE LISTE (für Dropdown) =====
const VAG_MODULES = [
  {name:"Motor/Drive ECU 1",         req:"7E0",resp:"7E8"},
  {name:"Motor/Drive ECU 2",         req:"7E2",resp:"7EA"},
  {name:"Getriebe/Trans",            req:"7E1",resp:"7E9"},
  {name:"Drive Motor Control",       req:"7E6",resp:"7EE"},
  {name:"Battery Energy Mgmt (BMS)", req:"7E5",resp:"7ED"},
  {name:"Battery Charger",           req:"765",resp:"7CF"},
  {name:"Battery Regulation",        req:"728",resp:"792"},
  {name:"Bremse/ABS",                req:"713",resp:"77D"},
  {name:"Lenkung/EPS",               req:"712",resp:"77C"},
  {name:"Airbag",                    req:"715",resp:"77F"},
  {name:"Gateway",                   req:"710",resp:"77A"},
  {name:"Kombiinstrument/Tacho",     req:"714",resp:"77E"},
  {name:"Zentralelektrik",           req:"70E",resp:"778"},
  {name:"Komfortsystem",             req:"70D",resp:"777"},
  {name:"Klimaanlage/AC",            req:"746",resp:"7B0"},
  {name:"ACC/Abstandsregelung",      req:"757",resp:"7C1"},
  {name:"Lenkwinkelsensor",          req:"751",resp:"7BB"},
  {name:"Feststellbremse/EPB",       req:"752",resp:"7BC"},
  {name:"Reifendruck/TPMS",          req:"70B",resp:"775"},
  {name:"Kamera Rückfahrt",          req:"769",resp:"7D3"},
  {name:"Spurhalteassistent",        req:"74E",resp:"7B8"},
  {name:"Einparkhilfe/PDC",          req:"70A",resp:"774"},
  {name:"Sitz Fahrer",               req:"74C",resp:"7B6"},
  {name:"Navigation",                req:"76C",resp:"7D6"},
  {name:"Telematik",                 req:"767",resp:"7D1"},
  {name:"Wegfahrsperre",             req:"711",resp:"77B"},
  {name:"Schlosselektronik",         req:"71E",resp:"788"},
  {name:"Klimakompressor",           req:"719",resp:"783"},
  {name:"Standheizung",              req:"76A",resp:"7D4"},
  {name:"Anhängerfunktion",          req:"747",resp:"7B1"},
];

const MEB_DIDS = [
  {did:"F190",name:"VIN"},
  {did:"F187",name:"Teilenummer"},
  {did:"F189",name:"SW Version"},
  {did:"F18C",name:"ECU Seriennr"},
  {did:"0280",name:"SoC %"},
  {did:"028C",name:"SoC real %"},
  {did:"01E4",name:"HV Spannung"},
  {did:"01E5",name:"HV Strom"},
  {did:"01E3",name:"Leistung kW"},
  {did:"01A4",name:"Zelltemp max"},
  {did:"01A5",name:"Zelltemp min"},
  {did:"0100",name:"Speed"},
  {did:"0101",name:"E-Motor RPM"},
  {did:"0102",name:"Drehmoment"},
  {did:"0168",name:"Kühlmittel°C"},
  {did:"0295",name:"Reichweite km"},
  {did:"1000",name:"Odometer"},
];

const OBD_PRESETS = [
  {pid:"0C",name:"RPM",       hex:"02 01 0C 00 00 00 00 00"},
  {pid:"0D",name:"Speed",     hex:"02 01 0D 00 00 00 00 00"},
  {pid:"05",name:"Kühlmittel",hex:"02 01 05 00 00 00 00 00"},
  {pid:"04",name:"Motorlast", hex:"02 01 04 00 00 00 00 00"},
  {pid:"11",name:"Drossel",   hex:"02 01 11 00 00 00 00 00"},
  {pid:"0F",name:"Ansaugluft",hex:"02 01 0F 00 00 00 00 00"},
  {pid:"2F",name:"Tank %",    hex:"02 01 2F 00 00 00 00 00"},
  {pid:"46",name:"Außentemp", hex:"02 01 46 00 00 00 00 00"},
  {pid:"5C",name:"Öl °C",     hex:"02 01 5C 00 00 00 00 00"},
  {pid:"00",name:"PIDs 0x00", hex:"02 01 00 00 00 00 00 00"},
  {pid:"20",name:"PIDs 0x20", hex:"02 01 20 00 00 00 00 00"},
  {pid:"40",name:"PIDs 0x40", hex:"02 01 40 00 00 00 00 00"},
  {pid:"02",name:"VIN (09)",  hex:"02 09 02 00 00 00 00 00"},
];

// Init Dropdowns & Tags
const modSel = document.getElementById('uds-module');
VAG_MODULES.forEach((m,i) => {
  const opt = document.createElement('option');
  opt.value = i; opt.text = m.name + '  (0x'+m.req+')';
  modSel.appendChild(opt);
});

const didTags = document.getElementById('did-tags');
MEB_DIDS.forEach(d => {
  const t = document.createElement('span');
  t.className = 'tag vag'; t.textContent = d.name;
  t.title = 'DID 0x'+d.did;
  t.onclick = () => { document.getElementById('uds-did').value = d.did; };
  didTags.appendChild(t);
});

const obdTags = document.getElementById('obd-tags');
OBD_PRESETS.forEach(p => {
  const t = document.createElement('span');
  t.className = 'tag'; t.textContent = p.name;
  t.onclick = () => {
    document.getElementById('tx-id').value  = '7DF';
    document.getElementById('tx-hex').value = p.hex;
    document.getElementById('rx-id').value  = '7E8';
  };
  obdTags.appendChild(t);
});

function onModuleChange() {
  const i = modSel.value;
  if (i === '') return;
  const m = VAG_MODULES[parseInt(i)];
  document.getElementById('uds-req-id').value  = m.req;
  document.getElementById('uds-resp-id').value = m.resp;
}

// ===== LOG STATE =====
let allLogs = [];
let dirFilter = 'all';

function setDir(d) {
  dirFilter = d;
  ['all','tx','rx'].forEach(x => document.getElementById('btn-'+x).classList.toggle('blue', x===d));
  renderLog();
}
setDir('all');

function filterLog() { renderLog(); }

function renderLog() {
  const q = document.getElementById('log-filter').value.toLowerCase();
  const area = document.getElementById('log-area');
  area.innerHTML = '';
  let shown = 0;
  for (let i = allLogs.length-1; i >= 0 && shown < 500; i--) {
    const e = allLogs[i];
    if (dirFilter === 'tx' && !e.tx) continue;
    if (dirFilter === 'rx' && e.tx)  continue;
    if (q && !(e.id+e.hex+e.dec).toLowerCase().includes(q)) continue;
    const row = document.createElement('div');
    row.className = 'log-row';
    row.innerHTML =
      `<span class="ts">${e.t}</span>`+
      `<span class="${e.tx?'dir-tx':'dir-rx'}">${e.tx?'TX':'RX'}</span>`+
      `<span class="cid">${e.id.toUpperCase()}</span>`+
      `<span class="hex">${e.hex}</span>`+
      `<span class="dec" title="${e.dec}">${e.dec}</span>`;
    area.appendChild(row);
    shown++;
  }
  document.getElementById('log-count').textContent = allLogs.length + ' Frames';
}

// ===== WEBSOCKET =====
let ws;
function connectWS() {
  ws = new WebSocket('ws://'+location.host+'/ws');
  ws.onopen  = () => {};
  ws.onclose = () => { setTimeout(connectWS,2000); };
  ws.onmessage = (e) => {
    const msg = JSON.parse(e.data);
    if (msg.type === 'log') {
      flashLed(msg.e.tx ? 'tx' : 'rx', msg.e.tx ? 'red' : 'blue');
      allLogs.push(msg.e);
      appendLogRow(msg.e);
    } else if (msg.type === 'scan') {
      updateScanUI(msg);
    } else if (msg.type === 'status') {
      document.getElementById('uptime').textContent = Math.floor(msg.uptime/1000)+'s';
    } else if (msg.type === 'ble_status') {
      setLed('ble', msg.connected ? 'blue' : '');
    }
  };
}

function appendLogRow(e) {
  const area = document.getElementById('log-area');
  const q = document.getElementById('log-filter').value.toLowerCase();
  if (dirFilter === 'tx' && !e.tx) return;
  if (dirFilter === 'rx' && e.tx)  return;
  if (q && !(e.id+e.hex+e.dec).toLowerCase().includes(q)) return;

  const row = document.createElement('div');
  row.className = 'log-row' + (e.src === 1 ? ' elm-row' : '');
  row.innerHTML =
    `<span class="ts">${e.t}</span>`+
    `<span class="${e.tx?'dir-tx':'dir-rx'}">${e.tx?'TX':'RX'}</span>`+
    `<span class="cid">${e.src===1?'<span class="elm-tag">ELM</span> ':''  }${e.id.toUpperCase()}</span>`+
    `<span class="hex">${e.hex}</span>`+
    `<span class="dec" title="${e.dec}">${e.dec}</span>`;

  if (document.getElementById('autoscroll').checked) {
    area.appendChild(row);
    area.scrollTop = area.scrollHeight;
  } else {
    area.insertBefore(row, area.firstChild);
  }
  document.getElementById('log-count').textContent = allLogs.length+' Frames';
}

function updateScanUI(msg) {
  const pw = document.getElementById('progress-wrap');
  pw.style.display = 'block';
  document.getElementById('scan-msg').textContent   = msg.msg;
  document.getElementById('progress-txt').textContent = msg.step+'/'+msg.total;
  const pct = msg.total > 0 ? (msg.step/msg.total*100) : 0;
  document.getElementById('progress-fill').style.width = pct+'%';
  document.getElementById('btn-abort').style.display = msg.run ? 'block' : 'none';
  if (!msg.run) {
    document.querySelectorAll('.btn.orange').forEach(b=>b.disabled=false);
  }
}

// ===== API CALLS =====
async function sendManual() {
  const id  = document.getElementById('tx-id').value.trim();
  const hex = document.getElementById('tx-hex').value.trim();
  const rxid = document.getElementById('rx-id').value.trim();
  await fetch('/send',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({id,hex,rxFilter:rxid})});
}

async function sendUDS() {
  const req  = document.getElementById('uds-req-id').value.trim();
  const resp = document.getElementById('uds-resp-id').value.trim();
  const did  = document.getElementById('uds-did').value.trim().padStart(4,'0');
  const b1 = did.substring(0,2); const b2 = did.substring(2,4);
  const hex = `03 22 ${b1} ${b2} CC CC CC CC`;
  document.getElementById('tx-id').value  = req;
  document.getElementById('tx-hex').value = hex;
  document.getElementById('rx-id').value  = resp;
  await fetch('/send',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({id:req,hex:hex,rxFilter:resp})});
}

async function sendTesterPresent() {
  const req = document.getElementById('uds-req-id').value.trim() || '7DF';
  await fetch('/send',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({id:req,hex:'02 3E 00 CC CC CC CC CC',rxFilter:'0'})});
}

async function sendDiagSession() {
  const req = document.getElementById('uds-req-id').value.trim() || '7E0';
  await fetch('/send',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({id:req,hex:'02 10 03 CC CC CC CC CC',rxFilter:'0'})});
}

async function startScan(mode) {
  document.querySelectorAll('.btn.orange').forEach(b=>b.disabled=true);
  document.getElementById('btn-abort').style.display='block';
  const req  = document.getElementById('uds-req-id').value.trim()||'7E6';
  const resp = document.getElementById('uds-resp-id').value.trim()||'7EE';
  const name = modSel.options[modSel.selectedIndex]?.text || 'Modul';
  await fetch('/scan',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({mode,reqId:req,respId:resp,name})});
}

async function abortScan() {
  await fetch('/scan-abort',{method:'POST'});
  document.getElementById('btn-abort').style.display='none';
  document.querySelectorAll('.btn.orange').forEach(b=>b.disabled=false);
}

async function toggleMonitor() {
  const en = document.getElementById('monitor-cb').checked;
  await fetch('/monitor',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({enable:en})});
}

async function restartCAN() {
  const br = document.getElementById('bitrate').value;
  setLed('can','');
  await fetch('/restart-can',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({bitrate:parseInt(br)})});
}

async function clearLog() {
  allLogs=[];
  document.getElementById('log-area').innerHTML='';
  document.getElementById('log-count').textContent='0 Frames';
  await fetch('/clear-log',{method:'POST'});
}

async function downloadLog() {
  const r = await fetch('/download-log');
  const t = await r.text();
  const a = Object.assign(document.createElement('a'),{
    href: URL.createObjectURL(new Blob([t],{type:'text/plain'})),
    download: 'vw_can_log_'+Date.now()+'.txt'
  });
  a.click();
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
