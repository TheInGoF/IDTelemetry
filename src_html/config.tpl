<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>VW CAN · Config</title>
{{include:_head_common.html}}<style>
.container{max-width:800px;margin:0 auto;padding:20px}
.section-title{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:1.5px;margin-bottom:10px;margin-top:24px;display:flex;align-items:center;gap:8px}
.section-title::after{content:'';flex:1;height:1px;background:var(--border)}
.card{background:var(--bg2);border:1px solid var(--border);border-radius:6px;padding:16px 20px;margin-bottom:8px}
.lbl{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:1px;margin-bottom:4px}
input[type=text],input[type=number]{width:100%;background:var(--bg3);border:1px solid var(--border);color:var(--text);padding:6px 10px;border-radius:4px;font-family:var(--mono);font-size:12px}
.btn{font-size:11px;background:transparent;padding:6px 16px;border-radius:4px;cursor:pointer;font-family:var(--mono);border:1px solid var(--border);color:var(--muted);transition:all .2s}
.btn:hover{border-color:var(--accent);color:var(--accent)}
.btn.accent{border-color:var(--accent);color:var(--accent)}
.btn.accent:hover{background:rgba(0,212,255,.1)}
.btn.danger{border-color:var(--red);color:var(--red)}
.btn.danger:hover{background:rgba(255,69,96,.1)}
.btn.active{background:rgba(0,212,255,.1);border-color:var(--accent);color:var(--accent);font-weight:600}
.btn:disabled{opacity:.4;cursor:default}
.tx-box{background:var(--bg1);border:1px solid var(--border);border-radius:6px;padding:12px 20px;margin-bottom:20px;display:flex;align-items:center;gap:12px}
.wifi-net{display:flex;align-items:center;gap:10px;padding:8px 12px;border-radius:4px;margin-bottom:4px;border:1px solid transparent;transition:background .15s}
.wifi-net:hover{background:rgba(0,212,255,.05);border-color:var(--border)}
.wifi-net.is-guard{border-color:var(--green);background:rgba(0,255,157,.04)}
.wifi-net.is-guard .wifi-ssid{color:var(--green)}
.wifi-ssid{font-family:var(--mono);font-size:12px;font-weight:600;flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.wifi-meta{font-size:10px;color:var(--muted);flex-shrink:0;text-align:right;line-height:1.6}
.wifi-bar{height:8px;border-radius:2px;flex-shrink:0;min-width:4px}
.log-box{background:var(--bg3);border:1px solid var(--border);border-radius:4px;padding:12px;font-size:11px;color:var(--muted);max-height:220px;overflow-y:auto;line-height:1.7;font-family:var(--mono)}
.log-entry{border-bottom:1px solid rgba(255,255,255,.04);padding-bottom:4px;margin-bottom:4px}
.log-entry:last-child{border:none;margin:0}
.toast{position:fixed;bottom:20px;right:20px;background:var(--bg1);border:1px solid var(--accent);color:var(--accent);padding:10px 18px;border-radius:6px;font-size:12px;opacity:0;transition:opacity .3s;pointer-events:none}
.toast.show{opacity:1}
@keyframes spin{to{transform:rotate(360deg)}}
.spin{display:inline-block;width:12px;height:12px;border:2px solid var(--border);border-top-color:var(--accent);border-radius:50%;animation:spin .8s linear infinite;vertical-align:middle}

/* RTC time inputs */
.time-inputs{display:flex;align-items:center;gap:6px}
.time-inputs input{width:56px;text-align:center;font-size:16px;font-weight:700;padding:6px 4px}
.time-sep{font-size:20px;font-weight:700;color:var(--accent);line-height:1}
</style>
</head>
<body>
{{include:_header.html}}

<div class="container">

  <!-- Uhrzeit -->
  <div class="section-title">Uhrzeit (RTC DS1307)</div>
  <div class="card">
    <!-- Aktuelle RTC Zeit Anzeige -->
    <div style="display:flex;align-items:center;gap:16px;margin-bottom:16px;padding-bottom:16px;border-bottom:1px solid var(--border)">
      <div style="flex:1">
        <div class="lbl">RTC Zeit</div>
        <div id="rtc-display" style="font-size:28px;font-weight:700;color:var(--accent);letter-spacing:3px">--:--:--</div>
      </div>
      <div style="text-align:right">
        <div id="rtc-status" style="font-size:11px;color:var(--muted)">Lade...</div>
        <div id="rtc-ok-badge" style="font-size:11px;margin-top:4px"></div>
      </div>
    </div>
    <!-- Manuelle Zeiteinstellung -->
    <div>
      <div class="lbl" style="margin-bottom:8px">Zeit manuell setzen</div>
      <div style="display:flex;align-items:center;gap:12px;flex-wrap:wrap">
        <div class="time-inputs">
          <input type="number" id="set-hour" min="0" max="23" placeholder="HH" value="">
          <span class="time-sep">:</span>
          <input type="number" id="set-min" min="0" max="59" placeholder="MM" value="">
          <span class="time-sep">:</span>
          <input type="number" id="set-sec" min="0" max="59" placeholder="SS" value="0">
        </div>
        <button class="btn accent" onclick="setRtcTime()">✓ Setzen</button>
        <button class="btn" onclick="prefillBrowserTime()" style="font-size:10px">Browser-Zeit übernehmen</button>
      </div>
      <div style="font-size:10px;color:var(--muted);margin-top:8px">
        Zeit wird im RTC gespeichert und bleibt auch nach Stromausfall erhalten (CR2032 nötig).
      </div>
    </div>
  </div>

  <!-- CAN TX Status -->
  <div class="tx-box">
    <div style="font-size:11px;color:var(--muted)">CAN TX:</div>
    <div style="font-size:15px;font-weight:700" id="cantx-status">–</div>
    <div style="font-size:11px;color:var(--muted)" id="cantx-detail"></div>
    <button class="btn accent" id="btn-tx-unlock" style="margin-left:auto" onclick="txUnlock()">TX entsperren</button>
  </div>

  <!-- WiFi Umgebung -->
  <div class="section-title">Netzwerke in Reichweite</div>
  <div class="card">
    <div style="font-size:11px;color:var(--muted);margin-bottom:10px">Letzter automatischer Scan (alle 15s, nur wenn kein Client verbunden)</div>
    <div id="wifi-list"><div style="color:var(--muted);font-size:12px">Lade...</div></div>
  </div>

  <!-- WiFi Guard -->
  <div class="section-title">Fahrzeug-Wächter</div>
  <div class="card">
    <div style="display:flex;gap:8px;align-items:flex-end;flex-wrap:wrap;margin-bottom:12px">
      <div style="flex:1;min-width:160px">
        <div class="lbl">Fahrzeug WLAN-Name</div>
        <input id="wifi-ssid" type="text" placeholder="z.B. VW-ID7-12345">
      </div>
      <div style="width:110px">
        <div class="lbl">Min. RSSI (dBm)</div>
        <input id="wifi-rssi" type="number" value="-75" min="-100" max="-30">
      </div>
      <button class="btn accent" onclick="setWifiSsid()">Setzen</button>
    </div>
    <div style="display:flex;align-items:center;gap:12px;padding:10px 14px;background:var(--bg3);border-radius:4px;margin-bottom:10px">
      <div style="flex:1">
        <div class="lbl">Überwachtes Fahrzeug</div>
        <div id="wifi-active-ssid" style="font-size:14px;font-weight:700;color:var(--muted)">Kein Guard gesetzt</div>
        <div id="wifi-rssi-val" style="font-size:11px;color:var(--muted);margin-top:2px"></div>
      </div>
      <div style="text-align:right">
        <div id="wifi-state-val" style="font-size:13px;font-weight:700">–</div>
        <button class="btn danger" id="btn-clear-wifi" style="display:none;margin-top:6px;font-size:10px;padding:2px 10px" onclick="clearWifiSsid()">✕ entfernen</button>
      </div>
    </div>
  </div>

  <!-- Guard Mode -->
  <div class="section-title">Wächter-Modus</div>
  <div class="card">
    <div style="font-size:11px;color:var(--muted);margin-bottom:10px">Wann ist CAN TX erlaubt?</div>
    <div style="display:flex;gap:8px;flex-wrap:wrap" id="mode-btns">
      <button onclick="setMode(0)" class="btn" data-mode="0">Wächter aus</button>
      <button onclick="setMode(1)" class="btn" data-mode="1">Wächter aktiv</button>
    </div>
    <div style="margin-top:10px;font-size:11px;color:var(--muted)" id="mode-desc">–</div>
  </div>

  <!-- WiFi Log -->
  <div class="section-title">WiFi Scan Log
    <button class="btn danger" style="margin-left:auto;font-size:10px;padding:2px 10px" onclick="clearWifiLog()">✕ Löschen</button>
    <a href="/download-wifi-log" class="btn accent" style="font-size:10px;padding:2px 10px;text-decoration:none">↓ wifi.log</a>
  </div>
  <div style="font-size:10px;color:var(--muted);margin-bottom:6px">
    Wird nur geschrieben wenn kein Client verbunden und Guard SSID gesetzt ist.
  </div>
  <div class="log-box" id="wifi-log">Lade...</div>

  <!-- ELM Log -->
  <div class="section-title" style="margin-top:16px">ELM327 / ABRP Log
    <button class="btn danger" style="margin-left:auto;font-size:10px;padding:2px 10px" onclick="clearElmLog()">✕ Löschen</button>
    <a href="/download-elm-log" class="btn" style="border-color:var(--green);color:var(--green);font-size:10px;padding:2px 10px;text-decoration:none">↓ elm.log</a>
  </div>
  <div class="log-box" id="elm-log">Lade...</div>

  <!-- System Log -->
  <div class="section-title" style="margin-top:16px">System Log
    <button class="btn danger" style="margin-left:auto;font-size:10px;padding:2px 10px" onclick="clearSysLog()">&#x2715; Löschen</button>
    <a href="/download-sys-log" class="btn" style="border-color:var(--orange);color:var(--orange);font-size:10px;padding:2px 10px;text-decoration:none">&#x2193; sys.log</a>
  </div>
  <div style="font-size:10px;color:var(--muted);margin-bottom:6px">
    Neustarts &bull; Client-Events &bull; Guard-Status &bull; Zeit-Events
  </div>
  <div class="log-box" id="sys-log">Lade...</div>

</div>

<div class="toast" id="toast"></div>

<script>
{{include:_led_js.html}}
// BLE LED initial abfragen
(async()=>{
  try{
    const d=await(await fetch('/ble/status')).json();
    setLed('ble', d.guard_active&&d.guard_in_range?'blue':'');
  }catch(e){}
})();

// ── Toast ──────────────────────────────────────────────
function toast(msg, ok=true) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.style.borderColor = ok ? 'var(--green)' : 'var(--red)';
  t.style.color = ok ? 'var(--green)' : 'var(--red)';
  t.classList.add('show');
  setTimeout(() => t.classList.remove('show'), 2500);
}

// ── RTC Zeit vom ESP holen und anzeigen ────────────────
let rtcOffset = null;
let rtcFetchedAt = null;

async function loadRtcTime() {
  try {
    const d = await (await fetch('/status')).json();
    const el    = document.getElementById('rtc-display');
    const stEl  = document.getElementById('rtc-status');
    const okEl  = document.getElementById('rtc-ok-badge');

    if (d.rtc_ok && d.rtc_time && d.rtc_time !== '--:--:--') {
      const parts = d.rtc_time.split(':').map(Number);
      const nowMs = Date.now();
      rtcOffset = (parts[0]*3600 + parts[1]*60 + parts[2]) * 1000 - (nowMs % 86400000);
      rtcFetchedAt = nowMs;
      okEl.innerHTML = '<span style="color:var(--green)">⬤ RTC läuft</span>';
      stEl.textContent = 'Gespeichert im DS1307';
    } else if (!d.rtc_ok) {
      rtcOffset = null;
      okEl.innerHTML = '<span style="color:var(--red)">⬤ RTC gestoppt — Batterie prüfen</span>';
      stEl.textContent = 'Browser-Zeit wird automatisch übernommen…';
      el.style.color = 'var(--red)';
      const n = new Date();
      fetch('/wifi/set-time', {method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify({hour:n.getHours(), minute:n.getMinutes(), second:n.getSeconds()})
      }).then(() => setTimeout(loadRtcTime, 500));
    } else {
      okEl.innerHTML = '<span style="color:var(--muted)">⬤ RTC nicht gefunden</span>';
      stEl.textContent = 'Verkabelung prüfen: SDA=GPIO8, SCL=GPIO9';
      el.textContent = '--:--:--';
    }
  } catch(e) {
    document.getElementById('rtc-status').textContent = 'Verbindungsfehler';
  }
}

setInterval(() => {
  const el = document.getElementById('rtc-display');
  if (rtcOffset === null) return;
  const totalSec = Math.floor((Date.now() % 86400000 + rtcOffset) / 1000 + 86400) % 86400;
  const h = Math.floor(totalSec / 3600);
  const m = Math.floor((totalSec % 3600) / 60);
  const s = totalSec % 60;
  const fmt = n => String(n).padStart(2,'0');
  el.textContent = fmt(h) + ':' + fmt(m) + ':' + fmt(s);
  el.style.color = 'var(--accent)';
}, 1000);

function prefillBrowserTime() {
  const now = new Date();
  document.getElementById('set-hour').value = now.getHours();
  document.getElementById('set-min').value  = now.getMinutes();
  document.getElementById('set-sec').value  = now.getSeconds();
}

async function setRtcTime() {
  const h = parseInt(document.getElementById('set-hour').value);
  const m = parseInt(document.getElementById('set-min').value);
  const s = parseInt(document.getElementById('set-sec').value) || 0;
  if (isNaN(h) || isNaN(m) || h < 0 || h > 23 || m < 0 || m > 59) {
    toast('Ungültige Zeit!', false); return;
  }
  try {
    const r = await fetch('/wifi/set-time', {
      method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({hour: h, minute: m, second: s})
    });
    const d = await r.json();
    if (d.ok) {
      toast('✓ Zeit gesetzt: ' + String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0'));
      await loadRtcTime();
    } else { toast('Fehler beim Setzen!', false); }
  } catch(e) { toast('Verbindungsfehler!', false); }
}

async function loadWifiResult() {
  try {
    const d = await (await fetch('/wifi/scan-result')).json();
    if (d.status === 'running' || !d.networks || d.status !== 'done') return;
    renderWifiNets(d.networks);
  } catch(e) { console.error(e); }
}

function renderWifiNets(nets) {
  nets.sort((a,b) => b.rssi - a.rssi);
  const container = document.getElementById('wifi-list');
  if (nets.length === 0) {
    container.innerHTML = '<div style="color:var(--muted);font-size:12px">Keine Netzwerke gefunden</div>';
    return;
  }
  container.innerHTML = nets.map(net => {
    const pct = Math.max(4, Math.min(44, (net.rssi + 100) * 0.88));
    const col = net.rssi > -55 ? 'var(--green)' : net.rssi > -72 ? 'var(--orange)' : 'var(--red)';
    const guard = net.is_guard;
    return `<div class="wifi-net${guard?' is-guard':''}">
      <div class="wifi-bar" style="width:${pct}px;background:${col}"></div>
      <div class="wifi-ssid">${net.ssid||'(versteckt)'}</div>
      <div class="wifi-meta">${net.rssi} dBm<br>${net.dist} · ch${net.channel}</div>
      <button class="btn${guard?' active':''}" style="font-size:10px;padding:3px 10px;flex-shrink:0"
        onclick="setWifiGuardFromScan('${net.ssid}',${net.rssi})">
        ${guard ? '✓ Guard' : '+ Guard'}
      </button>
    </div>`;
  }).join('');
}

async function loadWifiLog() {
  try {
    const r = await fetch('/download-wifi-log');
    const t = await r.text();
    const el = document.getElementById('wifi-log');
    if (!t.trim() || t.startsWith('# WiFi Log leer')) {
      el.textContent = 'Noch keine automatischen Scans · Guard SSID setzen und Seite schließen';
      return;
    }
    const lines = t.trim().split('\n').slice(-40);
    el.innerHTML = lines.map(l => {
      if (l.startsWith('---')) return `<div style="color:var(--accent);margin-top:4px">${l}</div>`;
      if (l.includes('[GUARD]')) return `<div style="color:var(--green)">${l}</div>`;
      return `<div>${l}</div>`;
    }).join('');
    el.scrollTop = el.scrollHeight;
  } catch(e) { document.getElementById('wifi-log').textContent = 'Fehler beim Laden'; }
}

async function clearWifiLog() {
  if (!confirm('WiFi Log löschen?')) return;
  await fetch('/clear-wifi-log', {method:'POST'});
  toast('Log gelöscht');
  loadWifiLog();
}

async function setWifiGuardFromScan(ssid, rssi) {
  const threshold = Math.max(-85, rssi - 15);
  document.getElementById('wifi-ssid').value = ssid;
  document.getElementById('wifi-rssi').value = threshold;
  await setWifiSsid();
}

async function setWifiSsid() {
  const ssid = document.getElementById('wifi-ssid').value.trim();
  const rssi = parseInt(document.getElementById('wifi-rssi').value);
  if (!ssid) { toast('SSID eingeben!', false); return; }
  await fetch('/wifi/set-ssid', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({ssid, rssi})
  });
  toast('✓ Guard: ' + ssid);
  refreshStatus();
  loadWifiResult();
}

async function clearWifiSsid() {
  await fetch('/wifi/clear-ssid', {method:'POST'});
  toast('WiFi Guard entfernt');
  refreshStatus();
}

async function txUnlock() {
  await fetch('/wifi/tx-unlock', {method:'POST'});
  toast('✓ TX entsperrt');
  refreshStatus();
}

const MODE_DESC = {
  0: 'Wächter aus — Senden ist immer erlaubt, egal ob Fahrzeug in der Nähe.',
  1: 'Wächter aktiv — Senden nur erlaubt wenn Fahrzeug-WLAN in Reichweite (<15m).'
};

async function setMode(mode) {
  await fetch('/wifi/set-mode', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({mode})
  });
  toast('Mode ' + mode + ' gesetzt');
  refreshStatus();
}

async function refreshStatus() {
  try {
    const d = await (await fetch('/wifi/status')).json();
    document.querySelectorAll('#mode-btns .btn').forEach(b => {
      b.classList.toggle('active', parseInt(b.dataset.mode) === d.mode);
    });
    document.getElementById('mode-desc').textContent = MODE_DESC[d.mode] || '';
    const ssidEl  = document.getElementById('wifi-active-ssid');
    const stateEl = document.getElementById('wifi-state-val');
    const rssiEl  = document.getElementById('wifi-rssi-val');
    const btnCl   = document.getElementById('btn-clear-wifi');
    if (d.active && d.ssid) {
      ssidEl.textContent = d.ssid;
      ssidEl.style.color = d.in_range ? 'var(--green)' : 'var(--orange)';
      btnCl.style.display = 'inline-block';
    } else {
      ssidEl.textContent = 'Kein Fahrzeug konfiguriert — Senden immer erlaubt';
      ssidEl.style.color = 'var(--muted)';
      btnCl.style.display = 'none';
    }
    const STATE_TEXT = {
      'LOCKED':'✓ Fahrzeug in Reichweite','LOST':'✗ Fahrzeug nicht gefunden',
      'SCANNING':'⟳ Suche läuft…','IDLE':'— Kein Fahrzeug konfiguriert','CHECK':'⟳ Prüfe Reichweite…'
    };
    const STATE_COLOR = {
      'LOCKED':'var(--green)','LOST':'var(--red)',
      'SCANNING':'var(--orange)','CHECK':'var(--orange)','IDLE':'var(--muted)'
    };
    stateEl.style.color = STATE_COLOR[d.state] || 'var(--muted)';
    stateEl.textContent = d.active ? (STATE_TEXT[d.state] || d.state) : '–';
    rssiEl.textContent  = d.active && d.rssi > -999 ? d.rssi + ' dBm · Schwelle: ' + d.threshold + ' dBm' : '';
    const txEl     = document.getElementById('cantx-status');
    const detEl    = document.getElementById('cantx-detail');
    const btnUnlck = document.getElementById('btn-tx-unlock');
    txEl.textContent = d.can_tx ? '✓ Senden erlaubt' : '✗ Senden gesperrt';
    txEl.style.color = d.can_tx ? 'var(--green)' : 'var(--red)';
    detEl.textContent = (d.wifi_ok ? '✓ Fahrzeug sichtbar' : '✗ Fahrzeug nicht sichtbar') + '  ·  Guard: ' + (d.mode === 1 ? 'Aktiv' : 'Aus');
    if (btnUnlck) {
      btnUnlck.disabled = d.manual_unlock;
      btnUnlck.textContent = d.manual_unlock ? '✓ TX entsperrt' : 'TX entsperren';
      btnUnlck.style.borderColor = d.manual_unlock ? 'var(--green)' : '';
      btnUnlck.style.color       = d.manual_unlock ? 'var(--green)' : '';
    }
  } catch(e) { console.error(e); }
}

async function loadElmLog() {
  try {
    const r = await fetch('/download-elm-log');
    const t = await r.text();
    const el = document.getElementById('elm-log');
    const lines = t.trim().split('\n').slice(-30);
    el.innerHTML = lines.map(l => `<div>${l}</div>`).join('') || 'Leer';
  } catch(e) {}
}

async function clearElmLog() {
  if (!confirm('ELM Log löschen?')) return;
  const r = await fetch('/clear-elm-log', {method:'POST'});
  const d = await r.json();
  toast(d.ok ? 'ELM Log gelöscht' : 'Fehler!', d.ok);
  loadElmLog();
}

async function loadSysLog() {
  try {
    const r = await fetch('/download-sys-log');
    const t = await r.text();
    const el = document.getElementById('sys-log');
    if (!t.trim() || t.startsWith('# Sys')) { el.textContent = 'Noch keine Events'; return; }
    const lines = t.trim().split('\n').slice(-60);
    el.innerHTML = lines.map(l => {
      const col = (l.includes('PANIC') || l.includes('CRASH') || l.includes('WDT')) ? 'var(--red)'
                : l.includes('BOOT')   ? 'var(--orange)'
                : l.includes('CLIENT') ? 'var(--accent)'
                : l.includes('GUARD')  ? 'var(--green)'
                : '';
      return col ? `<div style="color:${col}">${l}</div>` : `<div>${l}</div>`;
    }).join('');
    el.scrollTop = el.scrollHeight;
  } catch(e) {}
}

async function clearSysLog() {
  if (!confirm('System Log löschen?')) return;
  await fetch('/clear-sys-log', {method:'POST'});
  toast('Sys Log gelöscht');
  loadSysLog();
}

loadRtcTime();
refreshStatus();
loadWifiResult();
loadWifiLog();
loadElmLog();
loadSysLog();
setInterval(loadRtcTime,   30000);
setInterval(refreshStatus, 8000);
setInterval(loadWifiResult,20000);
setInterval(loadWifiLog,   35000);
setInterval(loadElmLog,    15000);
setInterval(loadSysLog,    20000);
</script>
</body>
</html>
