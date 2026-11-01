// DOM glue for the installer. All logic that can be tested lives in lib.js.
// Rules: textContent only (never innerHTML), same-origin fetches only, the token never reaches a log or status line.
import { BOARDS, validateVersions, pickBuild, esptoolCommand, validateSetup, inspectToken, buildCfgLine, parseCfgReply, describeReply } from './lib.js';

const $ = (id) => document.getElementById(id);
const state = { board: BOARDS[0].id, channel: 'stable', versions: null, build: null };

function renderBoards() {
  const box = $('boards');
  box.replaceChildren();
  for (const b of BOARDS) {
    const card = document.createElement('div');
    card.className = 'card';
    card.setAttribute('role', 'radio');
    card.tabIndex = 0;
    card.setAttribute('aria-checked', String(b.id === state.board));
    const strong = document.createElement('strong');
    strong.textContent = b.name;
    const small = document.createElement('div');
    small.className = 'muted';
    small.textContent = `${b.size} px round AMOLED, ${b.chip}`;
    card.append(strong, small);
    const pick = () => { state.board = b.id; renderBoards(); update(); };
    card.addEventListener('click', pick);
    card.addEventListener('keydown', (e) => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); pick(); } });
    box.append(card);
  }
}

async function loadVersions() {
  try {
    const r = await fetch('versions.json', { cache: 'no-cache', credentials: 'omit' });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const doc = await r.json();
    const v = validateVersions(doc);
    if (!v.ok) throw new Error(v.error);
    state.versions = doc;
  } catch (e) {
    state.versions = null;
    $('version-info').textContent = `Could not load the version list (${e.message}). Try again later.`;
  }
}

function update() {
  state.channel = $('channel').value;
  const slot = $('install-slot');
  slot.replaceChildren();
  $('unsupported').hidden = true;
  state.build = state.versions ? pickBuild(state.versions, state.board, state.channel) : null;
  if (!state.versions) return;
  if (!state.build) {
    $('version-info').textContent = 'No release is published for this board and channel yet.';
    return;
  }
  const b = state.build;
  $('version-info').textContent = `Version ${b.version}${b.date ? ` (${b.date})` : ''}, ${(b.size / 1048576).toFixed(1)} MB`;
  $('sha').textContent = `SHA-256 ${b.sha256}`;
  $('bin-link').href = b.file;
  $('esptool-cmd').textContent = esptoolCommand(b);

  // Web Serial only exists in Chrome and Edge on desktop, and only on https or localhost.
  if (!('serial' in navigator)) { $('unsupported').hidden = false; return; }
  const btn = document.createElement('esp-web-install-button');
  btn.setAttribute('manifest', b.manifest);
  for (const [name, text] of [['unsupported', 'This browser cannot flash devices. Use Chrome or Edge on a computer.'], ['not-allowed', 'Flashing works only on https:// pages or localhost.']]) {
    const s = document.createElement('span');
    s.slot = name;
    s.textContent = text;
    btn.append(s);
  }
  slot.append(btn);
}

async function sendConfig(config) {
  const port = await navigator.serial.requestPort(); // needs the user gesture of the submit click
  await port.open({ baudRate: 115200 });
  const reader = port.readable.getReader();
  const writer = port.writable.getWriter();
  try {
    await writer.write(new TextEncoder().encode(buildCfgLine(config)));
    const dec = new TextDecoder();
    let buf = '';
    const deadline = Date.now() + 45000; // Wi-Fi join (up to 20 s) plus the connection test (up to 15 s)
    while (Date.now() < deadline) {
      const { value, done } = await Promise.race([reader.read(), new Promise((r) => setTimeout(() => r({ timeout: true }), 1000))]);
      if (done) break;
      if (value) buf += dec.decode(value, { stream: true });
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        const reply = parseCfgReply(buf.slice(0, i));
        buf = buf.slice(i + 1);
        if (reply) return reply; // ordinary log lines are ignored
      }
    }
    return { ok: false, code: 'timeout', message: 'The device did not answer. Is RoundOS installed and running?' };
  } finally {
    writer.releaseLock();
    await reader.cancel().catch(() => {});
    reader.releaseLock();
    await port.close().catch(() => {});
  }
}

function refreshConfigForm() {
  const url = $('dz-url').value;
  const t = $('dz-token').value;
  const info = $('token-info');
  const tok = t ? inspectToken(t) : null;
  info.textContent = tok && tok.ok && tok.rightsName ? `Token rights: ${tok.rightsName}` : '';
  const isHttp = url.trim() !== '' && !/^https:\/\//i.test(url.trim());
  $('insecure-row').hidden = !isHttp;
  $('wifi-open-row').hidden = !($('wifi-ssid').value && !$('wifi-pass').value);
}

async function onSubmit(ev) {
  ev.preventDefault();
  const list = $('cfg-problems');
  const status = $('send-status');
  list.replaceChildren();
  status.textContent = '';
  const res = validateSetup({ ssid: $('wifi-ssid').value, password: $('wifi-pass').value, openNetwork: $('wifi-open').checked,
    url: $('dz-url').value, token: $('dz-token').value, insecureLan: $('dz-insecure').checked });
  for (const [msgs, cls] of [[res.errors, ''], [res.warnings, 'warn']]) {
    for (const m of msgs) { const li = document.createElement('li'); li.textContent = m; if (cls) li.className = cls; list.append(li); }
  }
  if (!res.ok) return;
  $('send').disabled = true;
  status.textContent = res.config.wifi ? 'Sending to the device. Joining Wi-Fi can take up to 45 seconds…' : 'Sending to the device…';
  try {
    const reply = await sendConfig(res.config);
    if (reply.ok) { $('dz-token').value = ''; $('wifi-pass').value = ''; } // do not leave the secrets sitting in the page
    status.textContent = describeReply(reply);
  } catch (e) {
    status.textContent = e && e.name === 'NotFoundError' ? 'No device was selected.' : 'Could not talk to the device over USB. Close other programs using the port and try again.';
  } finally {
    $('send').disabled = false;
  }
}

async function main() {
  renderBoards();
  $('channel').addEventListener('change', update);
  $('dz-url').addEventListener('input', refreshConfigForm);
  $('wifi-ssid').addEventListener('input', refreshConfigForm);
  $('dz-token').addEventListener('input', refreshConfigForm);
  $('wifi-pass').addEventListener('input', refreshConfigForm);
  $('wifi-show').addEventListener('change', () => { $('wifi-pass').type = $('wifi-show').checked ? 'text' : 'password'; });
  $('cfg').addEventListener('submit', onSubmit);
  if (!('serial' in navigator)) { $('send').disabled = true; $('send-status').textContent = 'Sending needs Chrome or Edge on a computer. You can also enter these details on the device\'s setup page.'; }
  // ESP Web Tools is vendored at a pinned version (see tools/vendor_esp_web_tools.sh), never loaded from a CDN.
  import('./vendor/esp-web-tools/install-button.js').catch(() => { $('version-info').textContent += ' (installer component missing: run tools/vendor_esp_web_tools.sh)'; });
  await loadVersions();
  update();
}
main();
