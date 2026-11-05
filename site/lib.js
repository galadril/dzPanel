// dzPanel installer: pure helpers (no DOM, no network) so they can be tested with node.
// Nothing here ever sends a token anywhere. See docs/provisioning.md.

export const BOARDS = [
  { id: 'amoled-175', name: 'Waveshare ESP32-S3-Touch-AMOLED-1.75', size: 466, shape: 'round', panel: 'AMOLED', chip: 'ESP32-S3', esptoolChip: 'esp32s3' },
  { id: 'amoled-216', name: 'Waveshare ESP32-S3-Touch-AMOLED-2.16', size: 480, shape: 'round', panel: 'AMOLED', chip: 'ESP32-S3', esptoolChip: 'esp32s3' },
  { id: 'guition-4848s040', name: 'Guition ESP32-S3-4848S040', size: 480, shape: 'square', panel: 'IPS LCD', chip: 'ESP32-S3', esptoolChip: 'esp32s3' },
];

export const CHANNELS = ['stable', 'nightly'];

// ---- versions ------------------------------------------------------------

// "1.2.3", "1.2.3-rc.1", "0.4.0-nightly.20261031" -> comparable object, or null
export function parseVersion(v) {
  const m = /^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$/.exec(String(v));
  if (!m) return null;
  return { major: +m[1], minor: +m[2], patch: +m[3], pre: m[4] ? m[4].split('.') : [] };
}

// semver precedence: a release outranks its own prereleases
export function compareVersions(a, b) {
  const x = parseVersion(a), y = parseVersion(b);
  if (!x || !y) throw new Error(`bad version: ${!x ? a : b}`);
  for (const k of ['major', 'minor', 'patch']) if (x[k] !== y[k]) return x[k] < y[k] ? -1 : 1;
  if (!x.pre.length || !y.pre.length) return x.pre.length === y.pre.length ? 0 : x.pre.length ? -1 : 1;
  for (let i = 0; i < Math.max(x.pre.length, y.pre.length); i++) {
    const p = x.pre[i], q = y.pre[i];
    if (p === undefined) return -1;
    if (q === undefined) return 1;
    const pn = /^\d+$/.test(p), qn = /^\d+$/.test(q);
    if (pn && qn) { if (+p !== +q) return +p < +q ? -1 : 1; }
    else if (pn !== qn) return pn ? -1 : 1;
    else if (p !== q) return p < q ? -1 : 1;
  }
  return 0;
}

// The layout tools/make_manifest.py writes: firmware/<board>/<channel>/<version>/<file>. Matching the raw string
// (no backslash, tab, space, "%", ".." or scheme can match) AND resolving it like a browser does, against two bases,
// so a parser difference cannot turn a "path" into another host.
const PATH_RE = /^firmware\/[a-z0-9-]+\/(stable|nightly)\/[0-9A-Za-z.+-]+\/[A-Za-z0-9._-]+$/;
export function sameOriginPath(p) {
  if (typeof p !== 'string' || !PATH_RE.test(p) || p.split('/').some((s) => s === '.' || s === '..')) return false;
  for (const base of ['https://installer.invalid/', 'http://localhost/']) {
    let u;
    try { u = new URL(p, base); } catch { return false; }
    if (u.origin !== new URL(base).origin || u.pathname !== `/${p}` || u.search || u.hash) return false;
  }
  return true;
}

// versions.json: { schema: 1, builds: [{board, channel, version, manifest, file, sha256, size, date}] }
export function validateVersions(doc) {
  if (!doc || doc.schema !== 1 || !Array.isArray(doc.builds)) return { ok: false, error: 'unsupported versions.json' };
  for (const b of doc.builds) {
    if (!BOARDS.some((x) => x.id === b.board)) continue; // unknown board: ignore, not fatal
    if (!CHANNELS.includes(b.channel) || !parseVersion(b.version)) return { ok: false, error: 'bad entry in versions.json' };
    // same-origin only: never let a metadata file point the flasher at another host
    for (const p of [b.manifest, b.file]) {
      if (!sameOriginPath(p) || !p.startsWith(`firmware/${b.board}/${b.channel}/${b.version}/`)) return { ok: false, error: 'unsafe path in versions.json' };
    }
    if (!/^[0-9a-f]{64}$/.test(b.sha256 || '')) return { ok: false, error: 'bad sha256 in versions.json' };
  }
  return { ok: true };
}

export function pickBuild(doc, board, channel) {
  const list = doc.builds.filter((b) => b.board === board && b.channel === channel);
  if (!list.length) return null;
  return list.reduce((best, b) => (compareVersions(b.version, best.version) > 0 ? b : best));
}

// Offline fallback for browsers without Web Serial. UNVERIFIED against the current esptool CLI syntax.
export function esptoolCommand(build) {
  const b = BOARDS.find((x) => x.id === build.board);
  if (!b) throw new Error('unknown board');
  return `esptool.py --chip ${b.esptoolChip} --baud 460800 write_flash 0x0 ${build.file.split('/').pop()}`;
}

// ---- Domoticz configuration ---------------------------------------------

export function normaliseUrl(input) {
  let s = String(input || '').trim();
  if (!s) return { ok: false, error: 'Enter your Domoticz address.' };
  if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(s)) s = 'http://' + s; // people type "192.168.1.5:8080"
  let u;
  try { u = new URL(s); } catch { return { ok: false, error: 'That does not look like a web address.' }; }
  if (u.protocol !== 'http:' && u.protocol !== 'https:') return { ok: false, error: 'Use an http:// or https:// address.' };
  if (u.username || u.password) return { ok: false, error: 'Do not put a user name or password in the address. Use an access token.' };
  if (u.search || u.hash) return { ok: false, error: 'Use only the address, without ? or # parts.' };
  const path = u.pathname.replace(/\/+$/, '');
  return { ok: true, url: `${u.protocol}//${u.host}${path}`, https: u.protocol === 'https:' };
}

function b64urlDecode(s) {
  s = s.replace(/-/g, '+').replace(/_/g, '/');
  s += '='.repeat((4 - (s.length % 4)) % 4);
  return typeof atob === 'function' ? atob(s) : Buffer.from(s, 'base64').toString('binary');
}

// Reads the rights from a Domoticz access token locally. Domoticz puts them in the "roles" claim:
// 0 Viewer, 1 User, 2 Admin. The signature is NOT checked (only the server can); this is a hint for the user.
export function inspectToken(token) {
  const t = String(token || '').trim();
  if (!t) return { ok: false, error: 'Paste the access token.' };
  if (/\s/.test(t)) return { ok: false, error: 'The token must not contain spaces or line breaks.' };
  const parts = t.split('.');
  if (parts.length !== 3 || !parts.every((p) => /^[A-Za-z0-9_-]*$/.test(p)) || !parts[0] || !parts[1]) {
    return { ok: true, token: t, rights: null, warnings: ['This does not look like a Domoticz access token (a signed JWT). It may still work; the device will test it.'] };
  }
  const warnings = [];
  let rights = null, expires = null;
  try {
    const payload = JSON.parse(b64urlDecode(parts[1]));
    if (Array.isArray(payload.roles) && Number.isInteger(payload.roles[0])) rights = payload.roles[0];
    if (Number.isFinite(payload.exp)) expires = payload.exp;
  } catch { warnings.push('Could not read the token details; the device will test it.'); }
  if (rights === 2) warnings.push('This token has Admin rights. A wall display does not need them: create a Viewer token (read only) or a User token (can switch).');
  if (expires !== null && expires * 1000 < Date.now()) warnings.push('This token has expired. Create a new one in Domoticz.');
  return { ok: true, token: t, rights, rightsName: ['Viewer (read only)', 'User (can switch)', 'Admin'][rights] ?? null, expires, warnings };
}

export function validateConfig({ url, token, insecureLan }) {
  const u = normaliseUrl(url), t = inspectToken(token);
  const errors = [], warnings = [];
  if (!u.ok) errors.push(u.error);
  if (!t.ok) errors.push(t.error);
  if (errors.length) return { ok: false, errors, warnings };
  warnings.push(...t.warnings);
  if (!u.https && !insecureLan) errors.push('This address is plain http, so the token is sent unencrypted over your network. Tick "I understand" for a home network, or use https.');
  if (errors.length) return { ok: false, errors, warnings };
  return { ok: true, errors, warnings, config: { v: 1, domoticz: { url: u.url, token: t.token, insecure_lan: !u.https } }, rights: t.rights };
}

// ---- Wi-Fi (same rules as ru_wifi_ssid_check / ru_wifi_pass_check in components/ru_cfg; shared vectors in the tests) ----
// ssid: 1..32 BYTES of UTF-8, no control characters (U+0000..U+001F, U+007F..U+009F), no lone surrogates.
// password: empty (open network), 8..63 printable ASCII (WPA2 passphrase) or exactly 64 hex digits (raw PSK).
const CONTROL = /[\u0000-\u001f\u007f-\u009f]/;

export function validateWifi({ ssid, password }) {
  const errors = [];
  const s = typeof ssid === 'string' ? ssid : '';
  const p = typeof password === 'string' ? password : '';
  if (!s) errors.push('Enter the Wi-Fi network name.');
  else if (CONTROL.test(s) || !wellFormed(s)) errors.push('The network name contains characters that are not allowed.');
  else if (new TextEncoder().encode(s).length > 32) errors.push('The network name is too long (at most 32 bytes; some characters take more than one).');
  if (p) {
    if (/^[0-9a-fA-F]{64}$/.test(p)) { /* raw 64 digit key */ }
    else if (!/^[\x20-\x7e]*$/.test(p)) errors.push('The Wi-Fi password may only use letters, digits, spaces and the usual symbols (no accents or control characters).');
    else if (p.length < 8) errors.push('The Wi-Fi password is too short (8 to 63 characters).');
    else if (p.length > 63) errors.push('The Wi-Fi password is too long (8 to 63 characters, or exactly 64 hex digits).');
  }
  return errors.length ? { ok: false, errors } : { ok: true, errors, wifi: { ssid: s, password: p } };
}

function wellFormed(s) {
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff) { const d = s.charCodeAt(i + 1); if (d >= 0xdc00 && d <= 0xdfff) i++; else return false; }
    else if (c >= 0xdc00 && c <= 0xdfff) return false;
  }
  return true;
}

// Whole form -> config. Empty Wi-Fi name and password = no Wi-Fi block; empty address and token = no Domoticz block;
// at least one is required. An empty password is only sent when "open network" is ticked (never by accident).
export function validateSetup({ ssid, password, openNetwork, url, token, insecureLan }) {
  const errors = [], warnings = [];
  const config = { v: 1 };
  const wantWifi = !!(ssid || password);
  const wantDz = !!(String(url || '').trim() || String(token || '').trim());
  if (!wantWifi && !wantDz) return { ok: false, errors: ['Enter the Wi-Fi details, the Domoticz details, or both.'], warnings };
  if (wantWifi) {
    const w = validateWifi({ ssid, password });
    if (!w.ok) errors.push(...w.errors);
    else if (!w.wifi.password && !openNetwork) errors.push('Enter the Wi-Fi password, or tick "open network" if it has none.');
    else config.wifi = w.wifi;
  }
  let rights = null;
  if (wantDz) {
    const d = validateConfig({ url, token, insecureLan });
    if (!d.ok) errors.push(...d.errors);
    else { config.domoticz = d.config.domoticz; rights = d.rights; }
    warnings.push(...d.warnings);
  }
  if (errors.length) return { ok: false, errors, warnings };
  return { ok: true, errors, warnings, config, rights };
}

// ---- USB configuration line protocol (spec: docs/provisioning.md) ----

export const CFG_PREFIX = 'RU+CFG ';

export function buildCfgLine(config) {
  const body = JSON.stringify(config);
  if (/[\r\n]/.test(body)) throw new Error('config must be one line');
  return `${CFG_PREFIX}${body}\n`;
}

// Never show or log the token: use this for anything user visible.
export function redact(config) {
  return JSON.parse(JSON.stringify(config, (k, v) => (k === 'token' || k === 'password' ? '••••••••' : v)));
}

// Fixed, human wording per reply. Never shows raw device text and never includes the config, the password or the token.
// Words are matched, never copied. "wifi_saved" in a reply means the Wi-Fi was joined and stored.
export function describeReply(r) {
  if (!r) return 'The device did not answer.';
  const words = String(r.ok ? r.info : r.message || '');
  const wifiSaved = /\bwifi_saved\b/.test(words);
  if (r.ok) {
    const dz = /\bsaved\b/.test(words.replace(/\bwifi_saved\b/g, ''));
    if (wifiSaved && !dz) return 'Done. The device joined the Wi-Fi network and saved it. Add the Domoticz details when you are ready.';
    const dzText = /\buntested\b/.test(words)
      ? 'Saved, but the device could not check it yet (no connection to Domoticz). It will use the settings once the network is up.'
      : 'Done. The device saved the settings and connected to Domoticz.';
    return wifiSaved ? `The device joined the Wi-Fi network and saved it. ${dzText}` : dzText;
  }
  const detail = String(r.message || '');
  const pre = wifiSaved ? 'The Wi-Fi network was joined and saved. ' : '';
  switch (r.code) {
    case 'bad_wifi': return 'The device refused the Wi-Fi details as invalid. Check the network name (at most 32 bytes) and the password (8 to 63 characters, or 64 hex digits).';
    case 'wifi_failed':
      if (/\bno_ap\b/.test(detail)) return 'The device could not find that Wi-Fi network. Check the name and that it is in range. Only 2.4 GHz networks work.';
      if (/\bbusy\b/.test(detail)) return 'The device is still busy with another Wi-Fi attempt. Wait a moment and try again.';
      if (/\bauth\b/.test(detail)) return 'The Wi-Fi network refused the connection. The password is probably wrong.';
      if (/\btimeout\b/.test(detail)) return 'The device did not get an address from the Wi-Fi network in time. Try again, closer to the router.';
      return 'The device could not join that Wi-Fi network. Check the name and the password. Nothing was changed.';
    case 'token_rejected': return pre + 'Domoticz rejected the token. Check that it was copied completely and has not been deleted.';
    case 'unreachable': return pre + (/\bnot_domoticz\b/.test(detail)
      ? 'That address answered, but it does not look like Domoticz. Check the address and port.'
      : 'The device cannot reach Domoticz. Check the address and that the device has Wi-Fi.');
    case 'no_wifi': return 'The device is not on Wi-Fi yet. Enter the Wi-Fi name and password above, or use the install dialog.';
    case 'tls': return pre + 'The secure connection failed. Check the https address and certificate.';
    case 'timeout': return 'The device did not answer. Is dzPanel installed and running?';
    case 'store': return pre + 'The device could not save the settings (storage error). Try again; if it repeats, reinstall and choose erase.';
    case 'bad_version': return 'This installer and the firmware disagree on the settings format. Update the firmware or reload this page.';
    case 'too_long': return 'The address, password or token is too long for the device.';
    case 'bad_json': case 'bad_url': case 'bad_token': return 'The device refused these settings as invalid. Check the address and the token.';
    default: return 'The device refused the settings.';
  }
}

export function parseCfgReply(line) {
  const m = /^RU\+CFG (OK|ERR) ?(.*)$/.exec(String(line).trim());
  if (!m) return null;
  return m[1] === 'OK' ? { ok: true, info: m[2] } : { ok: false, code: m[2].split(' ')[0] || 'unknown', message: m[2] };
}
