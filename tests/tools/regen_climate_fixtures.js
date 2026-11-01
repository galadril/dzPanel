// Regenerates tests/fixtures/mock_climate_*.json and mock_getconfig_*.json from sim/mock/domoticz_mock.js (--climate).
// These are HAND-WRITTEN (derived from the Domoticz source builder, not captured from a server). Run from the repo root:
//   node tests/tools/regen_climate_fixtures.js
'use strict';
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const PORT = 18097;
const root = path.join(__dirname, '..', '..');
const srv = spawn('node', [path.join(root, 'sim/mock/domoticz_mock.js'), '--port', PORT, '--climate'], { stdio: 'ignore' });
const get = async (qs) => (await fetch(`http://127.0.0.1:${PORT}/json.htm?type=command&${qs}`, { headers: { Authorization: 'Bearer viewer-token' } })).json();
const ctl = (qs) => fetch(`http://127.0.0.1:${PORT}/__mock/${qs}`).then((r) => r.json());
const STAMP = '2026-11-01 12:00:00';
(async () => {
  try {
    for (let i = 0; i < 50; i++) { try { await get('param=getversion'); break; } catch { await new Promise((r) => setTimeout(r, 100)); } }
    for (const u of ['metric', 'imperial']) {
      await ctl('set?units=' + u);
      const d = await get('param=getdevices&filter=all&used=true');
      const result = d.result.filter((x) => Number(x.idx) >= 100).map((x) => ({ ...x, LastUpdate: STAMP }));
      const out = { ActTime: 1793534400, ServerTime: STAMP, status: 'OK', title: 'Devices', result };
      fs.writeFileSync(path.join(root, 'tests/fixtures', `mock_climate_${u}.json`), JSON.stringify(out, null, '\t'));
      const c = await get('param=getconfig');
      const { status, title, TempSign, TempScale, WindSign, WindScale, CurrencySign, Latitude, Longitude, WebUserName } = c;
      fs.writeFileSync(path.join(root, 'tests/fixtures', `mock_getconfig_${u}.json`),
        JSON.stringify({ CurrencySign, Latitude, Longitude, TempScale, TempSign, WebUserName, WindScale, WindSign, status, title }, null, '\t'));
    }
  } finally { srv.kill(); }
})();
