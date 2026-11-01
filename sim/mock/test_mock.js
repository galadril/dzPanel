// Contract test for domoticz_mock.js. Run: node test_mock.js
'use strict';
const { spawn } = require('child_process');
const path = require('path');
const PORT = Number(process.env.MOCK_PORT || 18080);
const srv = spawn('node', [path.join(__dirname, 'domoticz_mock.js'), '--port', PORT], { stdio: 'ignore' });
const get = async (qs, tok) => {
  const r = await fetch(`http://127.0.0.1:${PORT}/json.htm?type=command&${qs}`, { headers: tok ? { Authorization: 'Bearer ' + tok } : {} });
  return { code: r.status, body: await r.json() };
};
let bad = 0;
const check = (name, cond) => { console.log((cond ? 'ok   ' : 'FAIL ') + name); if (!cond) bad++; };
(async () => {
 try {
  for (let i = 0; i < 50; i++) { try { await get('param=getversion', 'viewer-token'); break; } catch { await new Promise((r) => setTimeout(r, 100)); } }
  check('no token -> 401', (await get('param=getversion')).code === 401);
  check('bad token -> 401', (await get('param=getversion', 'nope')).code === 401);
  check('viewer reads version', (await get('param=getversion', 'viewer-token')).body.version.startsWith('2026.4'));
  const all = await get('param=getdevices&filter=all&used=true', 'viewer-token');
  check('getdevices returns list', all.body.result.length >= 10);
  check('viewer cannot switch', (await get('param=switchlight&idx=1&switchcmd=On', 'viewer-token')).code === 403);
  check('user switches on', (await get('param=switchlight&idx=1&switchcmd=On', 'user-token')).body.status === 'OK');
  const one = await get('param=getdevices&rid=1', 'viewer-token');
  check('state changed', one.body.result[0].Status === 'On');
  check('dimmer level', (await get('param=switchlight&idx=2&switchcmd=Set Level&level=60', 'user-token')).body.status === 'OK'
    && (await get('param=getdevices&rid=2', 'viewer-token')).body.result[0].Level === 60);
  check('dimmer rejects 500', (await get('param=switchlight&idx=2&switchcmd=Set Level&level=500', 'user-token')).body.status === 'ERROR');
  check('protected device refused', (await get('param=switchlight&idx=7&switchcmd=On', 'user-token')).body.status === 'ERROR');
  check('setpoint', (await get('param=setsetpoint&idx=10&setpoint=22.5', 'user-token')).body.title === 'SetSetpoint');
  check('setpoint range', (await get('param=setsetpoint&idx=10&setpoint=99', 'user-token')).body.status === 'ERR');
  check('scene', (await get('param=switchscene&idx=1&switchcmd=On', 'user-token')).body.title === 'SwitchScene');
  const t = Math.floor(Date.now() / 1000) + 5;
  check('lastupdate filters', ((await get('param=getdevices&lastupdate=' + t, 'viewer-token')).body.result || []).length === 0);
  check('stale fixture present', all.body.result.some((d) => d.HaveTimeout === true));

  // ---- real 2026.4 shapes ----
  const U = 'user-token', V = 'viewer-token';
  const raw = async (qs, tok) => { const r = await fetch(`http://127.0.0.1:${PORT}${qs}`, { headers: tok ? { Authorization: 'Bearer ' + tok } : {} }); return { code: r.status, text: await r.text() }; };
  const ctl = (qs) => fetch(`http://127.0.0.1:${PORT}/__mock/${qs}`).then((r) => r.json());
  const dev = async (idx) => (await get('param=getdevices&rid=' + idx, V)).body.result[0];
  const rawDev = await raw('/json.htm?type=command&param=getdevices&filter=all&used=true', V);
  check('pretty printed with tabs', rawDev.text.startsWith('{\n\t"ActTime"'));
  const dj = JSON.parse(rawDev.text);
  check('getdevices has ActTime/ServerTime/Sunrise/Sunset/app_version', dj.ActTime > 1.7e9 && /^\d{4}-/.test(dj.ServerTime) && dj.Sunrise && dj.Sunset && dj.app_version.includes('build'));
  const keys = Object.keys(dj); check('keys sorted, status not last', keys.join() === [...keys].sort().join() && keys.indexOf('status') < keys.length - 1);
  check('getversion real fields', (await get('param=getversion', V)).body.dzvents_version !== undefined);
  check('Color Switch dimmer present', all.body.result.some((d) => d.Type === 'Color Switch' && d.SwitchTypeVal === 7));
  check('Setpoint type thermostat', (await dev(10)).Type === 'Setpoint' && (await dev(10)).SubType === 'SetPoint');
  check('meters carry SwitchTypeVal', ['21', '22', '23', '24', '25'].every((i) => all.body.result.find((d) => d.idx === i).SwitchTypeVal !== undefined));
  check('Group row in getdevices', all.body.result.some((d) => d.Type === 'Group' && d.idx === '3'));
  check('selector: Status empty, Data is the name', (await dev(4)).Status === '' && (await dev(4)).Data === 'Low');
  check('selector set level 30', (await get('param=switchlight&idx=4&switchcmd=Set Level&level=30', U)).body.status === 'OK' && (await dev(4)).Data === 'High' && (await dev(4)).LevelInt === 30);
  check('selector rejects level 35 and 40', (await get('param=switchlight&idx=4&switchcmd=Set Level&level=35', U)).body.status === 'ERR' && (await get('param=switchlight&idx=4&switchcmd=Set Level&level=40', U)).body.status === 'ERR');
  check('lock: On = Locked', (await get('param=switchlight&idx=6&switchcmd=Off', U)).body.status === 'OK' && (await dev(6)).Status === 'Unlocked'
    && (await get('param=switchlight&idx=6&switchcmd=On', U)).body.status === 'OK' && (await dev(6)).Status === 'Locked');
  check('dimmer Off keeps stored Level', (await get('param=switchlight&idx=2&switchcmd=Off', U)).body.status === 'OK' && (await dev(2)).Level === 60 && (await dev(2)).Status === 'Off');
  check('dimmer On shows Set Level text', (await get('param=switchlight&idx=2&switchcmd=Set Level&level=40', U)).body.status === 'OK' && (await dev(2)).Status === 'Set Level: 40 %');
  check('Lighting 2 MaxDimLevel 15 enforced', (await get('param=switchlight&idx=9&switchcmd=Set Level&level=16', U)).body.status === 'ERROR');
  check('push stays stateless', (await get('param=switchlight&idx=5&switchcmd=On', U)).body.status === 'OK' && (await dev(5)).Status === '');
  check('blinds close', (await get('param=switchlight&idx=3&switchcmd=Close', U)).body.status === 'OK' && (await dev(3)).Status === 'Closed');
  check('sensor refuses switchlight', (await get('param=switchlight&idx=11&switchcmd=On', U)).body.status === 'ERR');
  check('group switchscene mirrors into getdevices row', (await get('param=switchscene&idx=3&switchcmd=On', U)).body.title === 'SwitchScene'
    && (await get('param=getscenes', V)).body.result.find((s) => s.idx === '3').Status === 'On'
    && (await get('param=getdevices&filter=all', V)).body.result.find((d) => d.Type === 'Group').Status === 'On');
  check('Scene has no Off', (await get('param=switchscene&idx=1&switchcmd=Off', U)).body.status === 'ERR');
  check('favorite filter', (await get('param=getdevices&filter=all&favorite=1', V)).body.result.every((d) => d.Favorite === 1)
    && (await get('param=getdevices&filter=all&favorite=1', V)).body.result.length < (await get('param=getdevices&filter=all', V)).body.result.length);
  // delta with ActTime cursor
  const cur = (await get('param=getdevices&filter=all', V)).body.ActTime;
  await new Promise((r) => setTimeout(r, 1100));
  await ctl('touch?idx=1&status=On');
  const dl = (await get('param=getdevices&filter=all&lastupdate=' + cur, V)).body;
  check('delta after external change: only the changed device', dl.result.length === 1 && dl.result[0].idx === '1' && dl.result[0].Status === 'On');
  check('empty delta omits result', (await get('param=getdevices&filter=all&lastupdate=' + (dl.ActTime + 5), V)).body.result === undefined);
  await ctl('touch?idx=11&timeout=1');
  check('touch HaveTimeout', (await dev(11)).HaveTimeout === true);
  // injectable failures
  await ctl('set?mode=html');
  let r = await raw('/json.htm?type=command&param=getversion', V);
  check('mode html: 200 + HTML page', r.code === 200 && r.text.startsWith('<!DOCTYPE'));
  await ctl('set?mode=403&count=1');
  check('mode 403 once then recovers', (await raw('/json.htm?type=command&param=getversion', V)).code === 403 && (await raw('/json.htm?type=command&param=getversion', V)).code === 200);
  await ctl('set?mode=http500');
  check('mode http500', (await raw('/json.htm?type=command&param=getversion', V)).code === 500);
  await ctl('set?mode=statuserr');
  check('mode statuserr', JSON.parse((await raw('/json.htm?type=command&param=getversion', V)).text).status === 'ERR');
  await ctl('set?mode=huge');
  check('mode huge > 256 kB', (await raw('/json.htm?type=command&param=getversion', V)).text.length > 250000);
  await ctl('set?mode=chunked');
  check('mode chunked is parseable', JSON.parse((await raw('/json.htm?type=command&param=getversion', V)).text).version === '2026.4');
  await ctl('set?mode=drop');
  let dropped = false; try { await raw('/json.htm?type=command&param=getversion', V); } catch { dropped = true; }
  check('mode drop closes the socket', dropped);
  await ctl('set?mode=none&latency=250');
  const t0 = Date.now(); await raw('/json.htm?type=command&param=getversion', V);
  check('latency injection', Date.now() - t0 >= 240);
  await ctl('set?latency=0');
  const st = await ctl('stats').catch(() => null);
  const stt = await (await fetch(`http://127.0.0.1:${PORT}/__mock/stats`)).json();
  check('stats never expose the token', stt.requests > 10 && !JSON.stringify(stt).includes('user-token'));
  await ctl('reset');
  check('reset restores devices', (await dev(1)).Status === 'Off');

  // ---- units and the climate set (hand-written, derived from the Domoticz source) ----
  const cfgM = (await get('param=getconfig', V)).body;
  check('getconfig metric: C and m/s', cfgM.TempSign === 'C' && cfgM.WindSign === 'm/s' && cfgM.status === 'OK');
  check('climate set is off by default', (await get('param=getdevices&rid=100', V)).body.result === undefined);
  await ctl('set?units=imperial&climate=1');
  const cfgI = (await get('param=getconfig', V)).body;
  check('getconfig imperial: F and mph', cfgI.TempSign === 'F' && cfgI.WindSign === 'mph');
  const wind = await dev(104), th6 = await dev(117), sp = await dev(118), evo = await dev(124), mode = await dev(125);
  check('wind converted: speed mph, Temp F, own thermometer present', wind.Speed === '7.6' && wind.Temp === 53.6 && wind.Chill === 49.1 && wind.Direction === 315);
  check('setpoint converted: F, vunit, range', sp.SetPoint === '66.2' && sp.vunit === '°F' && sp.min === 41 && sp.max === 86);
  check('thermostat 6 has Temp and SetPoint and range', th6.Temp === 68.9 && th6.SetPoint === 69.8 && th6.step === 0.5);
  check('evohome zone: no range', evo.Type === 'Heating' && evo.min === undefined && evo.Status === 'Auto');
  check('radiator: no range', (await dev(123)).min === undefined);
  check('mode device: Mode + Modes', mode.Mode === 1 && mode.Modes === '0;Off;1;Heat;2;Cool;' && mode.Data === 'Heat');
  check('setpoint in F converts back (68 F = 20 C accepted)', (await get('param=setsetpoint&idx=118&setpoint=68', U)).body.title === 'SetSetpoint' && (await dev(118)).SetPoint === '68.0');
  check('setpoint out of range refused', (await get('param=setsetpoint&idx=118&setpoint=120', U)).body.status === 'ERR');
  check('thermostatstate sets Mode', (await get('param=thermostatstate&idx=125&state=2', U)).body.title === 'Set Thermostat State' && (await dev(125)).Mode === 2 && (await dev(125)).Data === 'Cool');
  check('thermostatstate bad index refused', (await get('param=thermostatstate&idx=125&state=9', U)).body.status === 'ERR');
  check('thermostatstate needs user rights', (await get('param=thermostatstate&idx=125&state=1', V)).code === 403);
  check('lux/air/unseen carry Data only', (await dev(107)).Data === '123 Lux' && (await dev(108)).Quality === 'Fair' && (await dev(116)).Type === 'Quantum Flux');
  await ctl('reset');
  check('reset restores metric', (await get('param=getconfig', V)).body.TempSign === 'C' && (await get('param=getdevices&rid=100', V)).body.result === undefined);
  void st;
 } finally { srv.kill(); }
  srv.kill();
  process.exit(bad ? 1 : 0);
})();
