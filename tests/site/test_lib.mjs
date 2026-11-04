import assert from 'node:assert/strict';
import { compareVersions, pickBuild, validateVersions, normaliseUrl, inspectToken, validateConfig, buildCfgLine, redact, parseCfgReply, describeReply, esptoolCommand, validateWifi, validateSetup } from '../../site/lib.js';

const b64u = (o) => Buffer.from(JSON.stringify(o)).toString('base64url');
const jwt = (payload) => `${b64u({ alg: 'HS256' })}.${b64u(payload)}.c2lnbmF0dXJl`; // synthetic, not a real token
let n = 0; const t = (name, f) => { f(); n++; };

t('version order', () => {
  assert.equal(compareVersions('1.2.3', '1.2.3'), 0);
  assert.equal(compareVersions('1.10.0', '1.9.0'), 1);
  assert.equal(compareVersions('1.0.0-rc.1', '1.0.0'), -1); // release beats its prerelease
  assert.equal(compareVersions('1.0.0-rc.2', '1.0.0-rc.10'), -1); // numeric, not text
  assert.equal(compareVersions('v0.4.0-nightly.20261031', '0.4.0-nightly.20261030'), 1);
  assert.throws(() => compareVersions('x', '1.0.0'));
});

const sha = 'a'.repeat(64);
const E = (o) => ({ board: 'amoled-175', channel: 'stable', version: '1.0.0', manifest: 'firmware/amoled-175/stable/1.0.0/manifest.json', file: 'firmware/amoled-175/stable/1.0.0/a-merged.bin', sha256: sha, size: 1, ...o });
t('pick latest per board and channel', () => {
  const doc = { schema: 1, builds: [E({ version: '1.0.0' }), E({ version: '1.1.0' }), E({ version: '2.0.0', channel: 'nightly' }), E({ version: '9.0.0', board: 'amoled-216' })] };
  assert.equal(pickBuild(doc, 'amoled-175', 'stable').version, '1.1.0');
  assert.equal(pickBuild(doc, 'amoled-175', 'nightly').version, '2.0.0');
  assert.equal(pickBuild({ schema: 1, builds: [] }, 'amoled-175', 'stable'), null);
});

t('versions.json cannot redirect the flasher off-site', () => {
  assert.ok(validateVersions({ schema: 1, builds: [E()] }).ok);
  for (const bad of ['https://evil.example/x.bin', '//evil.example/x.bin', 'javascript:alert(1)', '../../x.bin', 'data:text/plain,x'])
    assert.equal(validateVersions({ schema: 1, builds: [E({ file: bad })] }).ok, false, bad);
  assert.equal(validateVersions({ schema: 1, builds: [E({ manifest: 'http://evil/m.json' })] }).ok, false);
  assert.equal(validateVersions({ schema: 1, builds: [E({ sha256: 'xyz' })] }).ok, false);
  assert.equal(validateVersions({ schema: 2, builds: [] }).ok, false);
  assert.equal(validateVersions(null).ok, false);
});

t('same-origin bypass strings are refused', () => {
  const bypass = ['/\\evil.example/fw.bin', '\\\\evil.example/x', ' //evil.example/x', '/\t/evil.example/x', '\n//evil.example/x', '/\n/evil.example/x',
    '\\/evil.example/x', '/\\/evil.example/x', '//evil.example/x', 'firmware/\\evil.example/x', 'firmware/amoled-175/stable/1.0.0/..%2f..%2fx.bin',
    '%2e%2e/%2e%2e/x', 'firmware/%2e%2e/%2e%2e/x.bin', 'firmware/amoled-175/stable/%2e%2e/x.bin', 'firmware/amoled-175/stable/1.0.0/%2e%2e', 'firmware/amoled-175/stable/1.0.0/a.bin\t',
    ' firmware/amoled-175/stable/1.0.0/a.bin', 'firmware/amoled-175/stable/1.0.0/a.bin?x=1', 'firmware/amoled-175/stable/1.0.0/a.bin#x', 'firmware/amoled-175/stable/1.0.0/../../../x.bin',
    '/firmware/amoled-175/stable/1.0.0/a.bin', 'http:evil.example/x', 'HTTPS://evil.example/x', 'fw/a.bin', 'firmware/amoled-175/beta/1.0.0/a.bin', 'firmware/amoled-216/stable/1.0.0/a.bin', ''];
  for (const bad of bypass) for (const k of ['file', 'manifest'])
    assert.equal(validateVersions({ schema: 1, builds: [E({ [k]: bad })] }).ok, false, JSON.stringify(bad));
  assert.equal(validateVersions({ schema: 1, builds: [E({ file: 5 })] }).ok, false);
  // the accepted shape resolves to the page origin under any base
  for (const base of ['https://installer.invalid/', 'http://localhost/'])
    assert.equal(new URL(E().file, base).origin, new URL(base).origin);
});

t('esptool fallback command', () => {
  assert.match(esptoolCommand(E()), /^esptool\.py --chip esp32s3 .* write_flash 0x0 a-merged\.bin$/);
});

t('url normalisation', () => {
  assert.deepEqual(normaliseUrl('192.168.1.5:8080'), { ok: true, url: 'http://192.168.1.5:8080', https: false });
  assert.equal(normaliseUrl(' https://dz.example.org/ ').url, 'https://dz.example.org');
  assert.equal(normaliseUrl('http://host/domoticz/').url, 'http://host/domoticz'); // reverse-proxy subpath kept
  assert.equal(normaliseUrl('https://u:p@host').ok, false); // credentials in URL refused
  assert.equal(normaliseUrl('http://host/?x=1').ok, false);
  assert.equal(normaliseUrl('ftp://host').ok, false);
  assert.equal(normaliseUrl('').ok, false);
  assert.equal(normaliseUrl('http://').ok, false);
});

t('token rights are read locally', () => {
  const far = Math.floor(Date.now() / 1000) + 86400;
  assert.equal(inspectToken(jwt({ roles: [0], exp: far })).rights, 0);
  assert.match(inspectToken(jwt({ roles: [1], exp: far })).rightsName, /User/);
  const admin = inspectToken(jwt({ roles: [2], exp: far }));
  assert.ok(admin.warnings.some((w) => /Admin/.test(w)));
  assert.ok(inspectToken(jwt({ roles: [0], exp: 1000 })).warnings.some((w) => /expired/.test(w)));
  assert.equal(inspectToken('not-a-jwt').rights, null);
  assert.equal(inspectToken('not-a-jwt').warnings.length, 1); // soft warning, device will test
  assert.equal(inspectToken('has space.x.y').ok, false);
  assert.equal(inspectToken('').ok, false);
  assert.equal(inspectToken('aaa.%%%.ccc').rights, null); // garbage payload does not throw
});

t('config validation', () => {
  const tok = jwt({ roles: [0] });
  assert.equal(validateConfig({ url: 'http://192.168.1.5:8080', token: tok, insecureLan: false }).ok, false); // plain http needs consent
  const ok = validateConfig({ url: 'http://192.168.1.5:8080', token: tok, insecureLan: true });
  assert.ok(ok.ok); assert.equal(ok.config.domoticz.insecure_lan, true);
  const https = validateConfig({ url: 'https://dz.example.org', token: tok, insecureLan: false });
  assert.ok(https.ok); assert.equal(https.config.domoticz.insecure_lan, false);
  assert.equal(validateConfig({ url: '', token: '' }).errors.length, 2);
});

t('line protocol and redaction', () => {
  const cfg = validateConfig({ url: 'https://dz.example.org', token: jwt({ roles: [1] }) }).config;
  const line = buildCfgLine(cfg);
  assert.ok(line.startsWith('RU+CFG {') && line.endsWith('}\n') && line.indexOf('\n') === line.length - 1);
  assert.ok(!JSON.stringify(redact(cfg)).includes(cfg.domoticz.token)); // token never shown
  assert.equal(redact(cfg).domoticz.url, cfg.domoticz.url);
  assert.deepEqual(parseCfgReply('RU+CFG OK saved'), { ok: true, info: 'saved' });
  assert.equal(parseCfgReply('RU+CFG ERR token_rejected 403').code, 'token_rejected');
  assert.equal(parseCfgReply('I (123) wifi: connected'), null); // ordinary log lines ignored
});

t('reply wording is fixed text and distinguishes the cases', () => {
  assert.match(describeReply(parseCfgReply('RU+CFG OK saved')), /connected to Domoticz/);
  assert.match(describeReply(parseCfgReply('RU+CFG OK saved untested')), /could not check/); // forced save is NOT "connected"
  assert.match(describeReply(parseCfgReply('RU+CFG ERR unreachable not_domoticz')), /does not look like Domoticz/);
  assert.match(describeReply(parseCfgReply('RU+CFG ERR unreachable')), /cannot reach/);
  assert.match(describeReply(parseCfgReply('RU+CFG ERR store')), /storage/);
  assert.match(describeReply(parseCfgReply('RU+CFG ERR bad_version update_firmware_or_installer')), /disagree/);
  assert.match(describeReply(parseCfgReply('RU+CFG ERR something_new')), /refused/); // unknown code: safe default
  assert.match(describeReply(null), /did not answer/);
  const secret = 'SECRET-TOKEN-123';
  assert.ok(!describeReply(parseCfgReply(`RU+CFG ERR bad_json ${secret}`)).includes(secret)); // raw device text never shown
});

// Same vectors as tests/test_cfg.c test_wifi_line (C checks the raw-byte UTF-8 cases, JS the lone surrogates).
const x = (c, n) => c.repeat(n);
const SSID_OK = ['home', 'a', x('x', 32), 'Café', '日本語wifi', x('\u{1F600}', 8), ' with spaces ', 'quo"te\'\\'];
const SSID_BAD = ['', x('x', 33), x('\u{1F600}', 8) + 'x', 'tab\there', 'nl\n', 'del\x7f', 'c1\u0085x', '\ud800', 'a\ud800b', '\udc00x', 'nul\u0000x', 'bel\u0007'];
const PASS_OK = ['', '12345678', x('a', 63), '0123456789abcdefABCDEF0123456789abcdefABCDEF0123456789abcdefABCD', 'pass word with spaces', 'p@$$w0rd!#%^&*()~`', '"quoted\\'];
const PASS_BAD = ['1234567', '0123456789abcdefABCDEF0123456789abcdefABCDEF0123456789abcdefABCg', x('a', 65), 'pässwörd1', 'pass\tword12', 'del\x7fpass12', 'line\nbreak12'];

t('wifi vectors', () => {
  assert.equal(PASS_OK[3].length, 64);
  for (const s of SSID_OK) assert.ok(validateWifi({ ssid: s, password: '' }).ok, JSON.stringify(s));
  for (const s of SSID_BAD) assert.equal(validateWifi({ ssid: s, password: '' }).ok, false, JSON.stringify(s));
  for (const p of PASS_OK) assert.ok(validateWifi({ ssid: 'net', password: p }).ok, p);
  for (const p of PASS_BAD) assert.equal(validateWifi({ ssid: 'net', password: p }).ok, false, p);
  assert.equal(validateWifi({}).ok, false);
  assert.equal(validateWifi({ ssid: 5, password: null }).ok, false);
  // ssid is never trimmed: spaces are part of the name
  assert.equal(validateWifi({ ssid: ' a ', password: '' }).wifi.ssid, ' a ');
  // errors never repeat the password
  const e = validateWifi({ ssid: 'net', password: 'Sup3rSecretä' }).errors.join(' ');
  assert.ok(!e.includes('Sup3rSecret'));
});

t('setup form: wifi alone, domoticz alone, both, neither', () => {
  const tok = jwt({ roles: [0] });
  const both = validateSetup({ ssid: 'home', password: 'hunter2hunter2', url: 'https://dz.example.org', token: tok });
  assert.ok(both.ok);
  assert.deepEqual(Object.keys(both.config), ['v', 'wifi', 'domoticz']);
  assert.deepEqual(both.config.wifi, { ssid: 'home', password: 'hunter2hunter2' });
  const w = validateSetup({ ssid: 'home', password: 'hunter2hunter2', url: '', token: '' });
  assert.ok(w.ok && !('domoticz' in w.config) && w.config.v === 1);
  const d = validateSetup({ ssid: '', password: '', url: 'https://dz.example.org', token: tok });
  assert.ok(d.ok && !('wifi' in d.config));
  assert.deepEqual(d.config, validateConfig({ url: 'https://dz.example.org', token: tok }).config); // identical to the old line
  assert.equal(validateSetup({}).ok, false);
  assert.equal(validateSetup({ ssid: '', password: '', url: '  ', token: '' }).ok, false);
  // open network needs an explicit tick, then sends an empty password
  assert.equal(validateSetup({ ssid: 'Guest', password: '' }).ok, false);
  const open = validateSetup({ ssid: 'Guest', password: '', openNetwork: true });
  assert.ok(open.ok && open.config.wifi.password === '');
  // password without a name, bad password, half a Domoticz block: all refused, secrets not echoed
  assert.equal(validateSetup({ ssid: '', password: 'hunter2hunter2' }).ok, false);
  const bad = validateSetup({ ssid: 'home', password: 'short1', url: 'https://dz.example.org', token: tok });
  assert.equal(bad.ok, false);
  assert.ok(!JSON.stringify(bad).includes('short1'));
  assert.equal(validateSetup({ ssid: 'home', password: 'hunter2hunter2', url: 'https://dz.example.org', token: '' }).ok, false);
});

t('wifi line: shape, redaction, size', () => {
  const secret = 'Sup3rS3cretPass!';
  const cfg = validateSetup({ ssid: 'MyHomeNet', password: secret, url: 'https://dz.example.org', token: jwt({ roles: [1] }) }).config;
  const line = buildCfgLine(cfg);
  assert.ok(line.startsWith('RU+CFG {"v":1,"wifi":{"ssid":"MyHomeNet","password":"Sup3rS3cretPass!"},"domoticz":{') && line.indexOf('\n') === line.length - 1);
  const red = JSON.stringify(redact(cfg));
  assert.ok(!red.includes(secret) && !red.includes(cfg.domoticz.token) && red.includes('MyHomeNet'));
  // the biggest legal line fits the device's 2048 byte cap (including the "RU+CFG " prefix, excluding the newline)
  const big = validateSetup({ ssid: x('"', 32), password: x('\\', 63), url: 'https://dz.example.org', token: x('T', 767) });
  assert.ok(big.ok, big.errors.join());
  const bytes = new TextEncoder().encode(buildCfgLine(big.config)).length - 1;
  assert.ok(bytes <= 2048, `line is ${bytes} bytes`);
  const jbig = validateSetup({ ssid: x('\u{1F600}', 8), password: x('a', 63), url: 'https://dz.example.org', token: x('T', 767) });
  assert.ok(new TextEncoder().encode(buildCfgLine(jbig.config)).length - 1 <= 2048);
});

t('wifi replies: fixed text, never raw device text', () => {
  const d = (l) => describeReply(parseCfgReply(l));
  assert.match(d('RU+CFG OK wifi_saved'), /joined the Wi-Fi network and saved it/);
  assert.match(d('RU+CFG OK wifi_saved saved 2026.4'), /joined the Wi-Fi.*connected to Domoticz/);
  assert.match(d('RU+CFG OK wifi_saved saved untested'), /joined the Wi-Fi.*could not check/);
  assert.match(d('RU+CFG OK saved'), /^Done. The device saved the settings and connected/); // unchanged
  assert.match(d('RU+CFG ERR bad_wifi'), /Wi-Fi details as invalid/);
  assert.match(d('RU+CFG ERR wifi_failed'), /could not join/);
  assert.match(d('RU+CFG ERR wifi_failed no_ap'), /could not find/);
  assert.match(d('RU+CFG ERR wifi_failed busy'), /busy with another/);
  assert.match(d('RU+CFG ERR wifi_failed auth'), /password is probably wrong/);
  assert.match(d('RU+CFG ERR wifi_failed timeout'), /address/);
  assert.match(d('RU+CFG ERR token_rejected wifi_saved'), /^The Wi-Fi network was joined and saved\. Domoticz rejected/);
  assert.match(d('RU+CFG ERR unreachable not_domoticz wifi_saved'), /joined and saved.*does not look like Domoticz/);
  assert.match(d('RU+CFG ERR token_rejected'), /^Domoticz rejected/); // no Wi-Fi claim without wifi_saved
  assert.match(d('RU+CFG ERR no_wifi'), /not on Wi-Fi/);
  const secret = 'Sup3rS3cretPass!';
  for (const l of [`RU+CFG ERR wifi_failed ${secret}`, `RU+CFG ERR bad_wifi ${secret}`, `RU+CFG OK ${secret}`, `RU+CFG ERR weird ${secret}`])
    assert.ok(!d(l).includes(secret), l);
  assert.ok(!/wifi_saved|wifi_failed|bad_wifi/.test(d('RU+CFG ERR wifi_failed no_ap'))); // raw words never appear
});

t('wifi inputs: random and hostile strings never throw and never leak into messages', () => {
  let seed = 7; const rnd = () => (seed = (seed * 1664525 + 1013904223) >>> 0);
  for (let i = 0; i < 3000; i++) {
    const len = rnd() % 90;
    let ssid = '', pass = '';
    for (let k = 0; k < len; k++) { ssid += String.fromCharCode(rnd() % 0x10000); pass += String.fromCharCode(32 + (rnd() % 100)); }
    const r = validateSetup({ ssid, password: 'PW' + pass, openNetwork: !!(i & 1), url: i % 3 ? '' : 'https://h.example', token: i % 5 ? '' : 'tok' });
    assert.equal(typeof r.ok, 'boolean');
    assert.ok(!r.errors.join(' ').includes('PW' + pass) || pass === '');
    if (r.ok && r.config.wifi) { // anything accepted obeys the device rules
      assert.ok(new TextEncoder().encode(r.config.wifi.ssid).length <= 32 && r.config.wifi.ssid.length > 0);
      assert.ok(r.config.wifi.password === '' || /^[0-9a-fA-F]{64}$/.test(r.config.wifi.password) || /^[\x20-\x7e]{8,63}$/.test(r.config.wifi.password));
    }
  }
});

console.log(`${n} site tests passed`);
