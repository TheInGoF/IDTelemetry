// ── Telemetry Stick — Shared JS ───────────────────────────

// ── Burger Menu ──────────────────────────────────────────
function toggleMenu(ev) {
  // Stop event from bubbling to the document-level close-handler below,
  // otherwise the same click that opens the menu would close it again.
  if (ev) { ev.stopPropagation(); ev.preventDefault(); }
  var m = document.getElementById('drop-nav');
  if (!m) return;
  var open = m.style.display !== 'none' && m.style.display !== '';
  m.style.display = open ? 'none' : 'block';
}
if (!window._menuListener) {
  window._menuListener = true;
  // Close on any outside click. Burger itself stops propagation so this
  // never fires for the burger button.
  document.addEventListener('click', function(e) {
    if (e.target.closest('.drop-nav')) return;   // link tap → let navigation happen
    if (e.target.closest('.burger-btn')) return; // burger handled by onclick
    var m = document.getElementById('drop-nav');
    if (m) m.style.display = 'none';
  });
}

// ── Modem Widget ─────────────────────────────────────────
var _modemLastOp = '';
function updateModem(sig, op, sim, conn) {
  var bars = !sim ? 0 : (sig < 0 || sig === 99 || sig === 0) ? 0 : sig < 5 ? 1 : sig < 10 ? 2 : sig < 15 ? 3 : sig < 20 ? 4 : 5;
  if (bars > 0 && op) _modemLastOp = op;
  for (var i = 1; i <= 5; i++) {
    var b = document.getElementById('mb' + i);
    if (b) b.classList.toggle('on', i <= bars);
  }
  var el = document.getElementById('modem-op');
  if (!el) return;
  if (!sim) { el.textContent = 'NO SIM'; el.style.color = 'var(--red)'; }
  else if (bars === 0) { el.textContent = _modemLastOp || 'NO NET'; el.style.color = 'var(--muted)'; }
  else { el.textContent = op || '--'; el.style.color = conn ? 'var(--green)' : 'var(--muted)'; }
}

// ── LED System ───────────────────────────────────────────
function setLed(id, cls) {
  var el = document.getElementById('led-' + id);
  if (el) el.className = 'led' + (cls ? ' ' + cls : '');
}

// ── Battery ──────────────────────────────────────────────
function setBatt(v, chg, vbus) {
  var el = document.getElementById('batt-pct');
  if (!el) return;
  if (v < 0) { el.textContent = 'off'; el.style.color = 'var(--muted)'; }
  else {
    el.textContent = (chg ? '\u26A1' : '') + (vbus && !chg ? '\u2197' : '') + v + '%';
    el.style.color = chg ? 'var(--yellow)' : v > 50 ? 'var(--green)' : v > 20 ? 'var(--yellow)' : 'var(--red)';
  }
}

// ── Password show / hide toggle ──────────────────────────
function togglePw(id, btn) {
  var el = document.getElementById(id);
  if (!el) return;
  var hidden = el.type === 'password';
  el.type = hidden ? 'text' : 'password';
  if (btn) btn.textContent = hidden ? '\u{1F648}' : '\u{1F441}';
}

// ── Toast ────────────────────────────────────────────────
function toast(msg, ok) {
  if (ok === undefined) ok = true;
  var el = document.getElementById('toast');
  el.textContent = msg;
  el.style.borderColor = ok ? 'var(--green)' : 'var(--red)';
  el.style.color = ok ? 'var(--green)' : 'var(--red)';
  el.classList.add('show');
  setTimeout(function() { el.classList.remove('show'); }, 2800);
}
