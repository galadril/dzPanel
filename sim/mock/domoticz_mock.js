#!/usr/bin/env node
// Mock Domoticz server for host-side development. No dependencies.
//
//   node domoticz_mock.js [--port 8080] [--latency 0] [--fail 0]
//
// Mirrors the shapes of a real 2026.4 server (see tests/fixtures/real_*.json and
// docs/research/domoticz-api.md section 7): pretty-printed JSON with sorted keys and "status"
// among them, ActTime/ServerTime/Sunrise/Sunset on getdevices, "app_version" there but "version"
// on getversion, a delta (lastupdate) with no changes OMITS "result", Color Switch dimmers,
// Setpoint thermostats, meters carrying SwitchTypeVal, selectors with Status "" and Data = level name.
// The server clock is UTC.
//
// Tokens:  viewer-token (read only), user-token (can switch), anything else = 401.
//
// Test controls (no auth, never present on a real server):
//   GET /__mock/set?latency=ms&fail=0..1&mode=<mode>   modes: none | html | http500 | http502 | http404 |
//        401 | 403 | drop | hang | truncate | huge | chunked | garbage | statuserr | nostatus | slowbody
//        (mode applies to /json.htm only; `count=N` limits it to the next N requests)
//   GET /__mock/set?units=imperial|metric   getconfig answers TempSign F + WindSign mph (and converts Temp, SetPoint, Chill,
//        DewPoint, min/max, Speed, Gust) or C + m/s (default). `climate=1` adds the climate device set below (also --climate).
//   GET /__mock/reset                       restore devices and settings (metric, no climate set)
//   GET /__mock/touch?idx=N&status=S        change a device behind the client's back (also &timeout=0|1, &protected=0|1)
//   POST /mcp                               the Domoticz MCP endpoint (JSON-RPC 2.0, plain JSON replies): initialize (answers a
//        Mcp-Session-Id header), notifications/initialized (202 + HTML body), tools/list (2 pages), tools/call for
//        set_switch_state, toggle_switch_state, set_dimmer_level, control_blinds, switch_scene, set_setpoint_value. Like the
//        real server it SKIPS the Protected check and never converts Fahrenheit. Needs a Bearer token; viewer-token gets -32002.
//        Modes (set with mode=): nomcp -> 503, nomcp404 -> 404 on /mcp only; the other modes hit /mcp too.
//   GET /__mock/set?push=skip|force         push buttons (switch types 9, 10): skip (default) answers 'already On. No action taken.' when the
//        stored state equals the request (the state stays On); force fires on every call (which MCP call forces a push is unknown)
//   GET /__mock/set?colour=1                add the synthetic colour lamps (also --colour): idx 200 RGBWW 'Spots' (orange, level 70, On), 201 RGB
//        'Led Strip' (blue), 202 RGBW 'Accent' (white), 203 WW 'Tunable' (m 2, t 157, On), 204 WW 'Reading Lamp' (Off, level 40), 205 RGBWW
//        'Ceiling' (Off, level 40, green). MCP set_color_brightness {idx, hue, brightness, iswhite?} and set_color_temperature {idx, kelvin}
//        follow what the Domoticz source does (read, not seen on a real lamp): hsb2rgb(hue,1,1), mode RGB (White with iswhite), the brightness
//        becomes the Level (0 = Off), kelvin 2700..6500 -> t = round(100*(6500-K)/3800*2.55), mode 2, Level and Status untouched. A hue on a
//        WW-only lamp answers 'Error ...'. Test switches: hue_drift=DEG (RGB read back that many degrees off), stuck=1 (colour tools answer
//        success and change nothing), protected via /__mock/touch.
//   GET /__mock/set?mcp=expire              forget every MCP session (next call: 'Session not found')
//   GET /__mock/stats                       {requests, lastAuth: "present|absent", urls:[...]}  (never echoes a token)
'use strict';
const http = require('http');
const { URL } = require('url');

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? Number(process.argv[i + 1]) : d; };
const PORT = arg('port', 8080);
const TOKENS = { 'viewer-token': 0, 'user-token': 1 };
const VERSION = '2026.4 (build 18568)';
const b64 = (s) => Buffer.from(s).toString('base64');
const pad = (n) => String(n).padStart(2, '0');
const stamp = (t = Date.now()) => new Date(t).toISOString().replace('T', ' ').slice(0, 19);
const epoch = (s) => Math.floor(Date.parse(s.replace(' ', 'T') + 'Z') / 1000);

const cfg = { latency: arg('latency', 0), fail: arg('fail', 0), mode: 'none', count: 0, push: 'skip' };
const stats = { requests: 0, lastAuth: 'absent', urls: [], mcpInits: 0, mcpCalls: [], mcpLastSession: 'absent' };
const sessions = new Set();

const hw = { HardwareDisabled: false, HardwareID: 1, HardwareType: 'Dummy (Does nothing, use for virtual switches only)', HardwareTypeVal: 15 };
const base = (idx, Name, Type, SubType, extra) => ({
  idx: String(idx), Name, Type, SubType, Favorite: 1, Protected: false, HaveTimeout: false, LastUpdate: stamp(),
  BatteryLevel: 255, SignalLevel: '-', PlanID: '1', PlanIDs: [1], Used: 1, Unit: 1, ...hw, ...extra,
});
const sw = (idx, Name, SwitchTypeVal, SwitchType, extra, Type = 'Light/Switch', SubType = 'Switch') =>
  base(idx, Name, Type, SubType, {
    SwitchTypeVal, SwitchType, Status: 'Off', Data: 'Off', Color: '', DimmerType: 'none', HaveDimmer: true,
    HaveGroupCmd: true, IsSubDevice: false, Level: 0, LevelInt: 0, MaxDimLevel: 100, UsedByCamera: false,
    StrParam1: '', StrParam2: '', ...extra,
  });

const makeDevices = () => [
  sw(1, 'Hall Lamp', 0, 'On/Off', {}),
  sw(2, 'Living Room', 7, 'Dimmer', { DimmerType: 'abs' }),
  sw(3, 'Bedroom Blinds', 13, 'Blinds Percentage', { Status: 'Open', Data: 'Open' }),
  sw(4, 'Bedroom Fan', 18, 'Selector',
    { Status: '', Data: 'Low', Level: 10, LevelInt: 10, LevelNames: b64('Off|Low|Med|High'), SelectorStyle: 0, LevelOffHidden: false },
    'Light/Switch', 'Selector Switch'),
  sw(5, 'Doorbell Sound', 9, 'Push On Button', { Status: '', Data: 'On', InternalState: 'On' }),
  sw(6, 'Front Door', 19, 'Door Lock', { Status: 'Locked', Data: 'Locked', InternalState: 'Locked' }),
  sw(7, 'Garage Door', 2, 'Contact', { Status: 'Closed', Data: 'Closed', Protected: true }),
  sw(8, 'Strip', 7, 'Dimmer', { Color: '{"b":0,"cw":98,"g":0,"m":2,"r":0,"t":157,"ww":157}', DimmerType: 'abs', Level: 1, LevelInt: 1 },
    'Color Switch', 'WW'),
  sw(9, 'Siren', 0, 'On/Off', { MaxDimLevel: 15 }, 'Lighting 2', 'AC'),
  base(10, 'Thermostat', 'Setpoint', 'SetPoint', {
    SetPoint: '21.5', Data: '21.5', HaveSetPoint: true, min: 14.0, max: 25.0, step: 0.5, vunit: '°C', TypeImg: 'override_mini',
  }),
  base(11, 'Outside', 'Temp + Humidity', 'THGN122/123', {
    Temp: 12.4, Humidity: 81, HumidityStatus: 'Wet', DewPoint: '9.4', Data: '12.4 C, 81 %', BatteryLevel: 90, SignalLevel: 7,
  }),
  base(12, 'Weather', 'Temp + Humidity + Baro', 'THB1 - BTHR918, BTHGN129', {
    Temp: 12.4, Humidity: 81, Barometer: 1014, Forecast: 2, ForecastStr: 'Partly Cloudy', Data: '12.4 C, 81 %, 1014 hPa', PlanIDs: [],
  }),
  base(13, 'Wind', 'Wind', 'TFA', { Direction: 225, DirectionStr: 'SW', Speed: '3.9', Gust: '6.1', Data: '3.9;SW;225;6.1;12.4;10.1', PlanIDs: [] }),
  base(14, 'Rain', 'Rain', 'RainByRate', { Rain: '0.0', RainRate: '0.0', Data: '0;0.0', PlanIDs: [] }),
  base(20, 'Home Power', 'Usage', 'Electric', { Data: '2100 Watt', Usage: '2100 Watt', PlanIDs: [] }),
  base(21, 'Smart Meter', 'P1 Smart Meter', 'Energy', {
    SwitchTypeVal: 0, Data: '10774134;8027203;1679414;3932977;15;0', Usage: '15 Watt', UsageDeliv: '0 Watt', Counter: '18801.337',
    CounterToday: '7.397 kWh', PlanIDs: [],
  }),
  base(22, 'Gas', 'P1 Smart Meter', 'Gas', { SwitchTypeVal: 1, Data: '6151.908', Counter: '6151.908', PlanIDs: [] }),
  base(23, 'Water', 'RFXMeter', 'RFXMeter counter', { SwitchTypeVal: 2, Data: '936.848 m3', PlanIDs: [] }),
  base(24, 'Energy kWh', 'General', 'kWh', { SwitchTypeVal: 4, Data: '22.034 kWh', Favorite: 0, PlanIDs: [] }),
  base(25, 'Visibility', 'General', 'Visibility', { SwitchTypeVal: 0, Data: '4.0 km', Favorite: 0, PlanIDs: [] }),
  base(26, 'UV', 'UV', 'UVN128,UV138', { Data: '4.0 UVI', PlanIDs: [] }),
  base(3, 'Group Device', 'Group', null, { Status: 'Off', Data: 'Off', TypeImg: 'group', PlanIDs: [0], PlanID: '' }),
  // Stale on purpose: HaveTimeout true and an old LastUpdate.
  base(30, 'Garden Sensor', 'Temp', 'LaCrosse TX3', { Temp: 8.1, Data: '8.1 C', HaveTimeout: true, LastUpdate: '2026-10-30 07:12:00', PlanIDs: [] }),
];
// Hand-written climate set, one device per Domoticz family, shaped after CWebServer::GetJSonDevices (read in
// vendor/domoticz, not captured from a server). Values are METRIC; output is converted when units = imperial.
// Plans: 7 = one temp sensor + a setpoint (NOW comes from the sensor); 8 = two temp sensors + a setpoint (NOW unknown);
// 9 = wind + UV only (their own thermometers are never "the temperature") + a setpoint.
const pl = (n) => ({ PlanID: String(n), PlanIDs: [n] });
const makeClimate = () => [
  base(100, 'Temp Sensor', 'Temp', 'LaCrosse TX3', { Temp: 21.5, Data: '21.5 C', trend: 2, BatteryLevel: 90, ...pl(7) }),
  base(101, 'Humidity Sensor', 'Humidity', 'LaCrosse TX3', { Humidity: 71, HumidityStatus: 'Wet', Data: 'Humidity 71 %', ...pl(0) }),
  base(102, 'Temp Hum Sensor', 'Temp + Humidity', 'THGN122/123', { Temp: 19.8, Humidity: 45, HumidityStatus: 'Comfortable', DewPoint: '7.90', Data: '19.8 C, 45 %', trend: 1, ...pl(0) }),
  base(103, 'Weather Station', 'Temp + Humidity + Baro', 'THB1 - BTHR918', {
    Temp: 20.4, Humidity: 52, HumidityStatus: 'Normal', DewPoint: '10.00', Barometer: 1009, Forecast: 1, ForecastStr: 'Sunny',
    Data: '20.4 C, 52 %, 1009 hPa', trend: 3, ...pl(0) }),
  base(104, 'Wind Sensor', 'Wind', 'TFA', { Direction: 315, DirectionStr: 'NW', Speed: '3.4', Gust: '6.1', Temp: 12.0, Chill: 9.5, trend: 1,
    Data: '315;NW;34;61;12.0;9.5', ...pl(9) }),
  base(105, 'Rain Gauge', 'Rain', 'RainByRate', { Rain: '12.5', RainRate: '0.4', Data: '0;12.5', ...pl(0) }),
  base(106, 'UV Sensor', 'UV', 'UV3', { UVI: '5', Temp: 20.1, Data: '5 UVI, Temp 20.1 C', ...pl(9) }),
  base(107, 'Lux Sensor', 'Lux', 'Lux', { Data: '123 Lux', ...pl(0) }),
  base(108, 'Air Sensor', 'Air Quality', 'Voltcraft CO-20', { Data: '850 ppm', Quality: 'Fair', ...pl(0) }),
  base(109, 'Percent Sensor', 'General', 'Percentage', { SwitchTypeVal: 0, Data: '47%', ...pl(0) }),
  base(110, 'Soil Sensor', 'General', 'Soil Moisture', { SwitchTypeVal: 0, Data: '35 cb', ...pl(0) }),
  base(111, 'Solar Sensor', 'General', 'Solar Radiation', { SwitchTypeVal: 0, Data: '512.0 Watt/m2', ...pl(0) }),
  base(112, 'Custom Sensor', 'General', 'Custom Sensor', { SwitchTypeVal: 0, Data: '42.5 kg', SensorUnit: 'kg', ...pl(0) }),
  base(113, 'Text Sensor', 'General', 'Text', { SwitchTypeVal: 0, Data: 'All good', ...pl(0) }),
  base(114, 'Pressure Sensor', 'General', 'Pressure', { SwitchTypeVal: 0, Data: '1.0 Bar', ...pl(0) }),
  base(115, 'Alert Sensor', 'General', 'Alert', { SwitchTypeVal: 0, Data: 'Server temperature high', ...pl(0) }),
  base(116, 'Unseen Sensor', 'Quantum Flux', 'Zorp', { Data: '7.5 zorp', ...pl(0) }),
  base(117, 'Thermostat Six', 'Thermostat 6', 'Temp/Hum/Setpoint', { Temp: 20.5, Humidity: 48, HumidityStatus: 'Comfortable', DewPoint: 9.1,
    SetPoint: 21.0, Data: '20.5 C (21.0 C) / 48%', HaveSetPoint: true, min: 5, max: 30, step: 0.5, vunit: '°C', trend: 2, ...pl(0) }),
  base(118, 'Plain Setpoint', 'Setpoint', 'SetPoint', { SetPoint: '19.0', Data: '19.0', HaveSetPoint: true, min: 5, max: 30, step: 0.5, vunit: '°C', ...pl(7) }),
  base(119, 'Cellar Temp', 'Temp', 'LaCrosse TX3', { Temp: 17.0, Data: '17.0 C', ...pl(8) }),
  base(120, 'Attic Temp', 'Temp', 'LaCrosse TX3', { Temp: 18.0, Data: '18.0 C', ...pl(8) }),
  base(121, 'Two Sensor Setpoint', 'Setpoint', 'SetPoint', { SetPoint: '20.0', Data: '20.0', HaveSetPoint: true, min: 5, max: 30, step: 0.5, vunit: '°C', ...pl(8) }),
  base(122, 'Wind Plan Setpoint', 'Setpoint', 'SetPoint', { SetPoint: '18.0', Data: '18.0', HaveSetPoint: true, min: 5, max: 30, step: 0.5, vunit: '°C', ...pl(9) }),
  base(123, 'Radiator Valve', 'Radiator 1', 'Smartwares', { SetPoint: '20.0', Data: '20.0', HaveSetPoint: true, ...pl(7) }),
  base(124, 'Zone Controller', 'Heating', 'Zone', { Temp: 19.5, SetPoint: 20.0, Status: 'Auto', Data: '19.5 C, (20.0 C), Auto', HaveSetPoint: true, ...pl(0) }),
  base(125, 'Mode Device', 'General', 'Thermostat Mode', { SwitchTypeVal: 0, Mode: 1, Modes: '0;Off;1;Heat;2;Cool;', Data: 'Heat', TypeImg: 'mode', ...pl(0) }),
];
// Colour lamps (see the header). Color is the JSON STRING Domoticz sends, keys sorted.
const colorStr = (m, t, r, g, b) => JSON.stringify({ b, cw: 0, g, m, r, t, ww: 0 });
const lamp = (idx, Name, SubType, color, level, on, extra = {}) => sw(idx, Name, 7, 'Dimmer',
  { Color: color, DimmerType: 'abs', Level: level, LevelInt: level, Status: on ? (level >= 100 ? 'On' : `Set Level: ${level} %`) : 'Off', Data: on ? 'On' : 'Off', ...extra },
  'Color Switch', SubType);
const makeColour = () => [
  lamp(200, 'Spots', 'RGBWW', colorStr(3, 0, 255, 128, 0), 70, true),
  lamp(201, 'Led Strip', 'RGB', colorStr(3, 0, 0, 0, 255), 60, true),
  lamp(202, 'Accent', 'RGBW', colorStr(1, 0, 0, 0, 0), 50, true),
  lamp(203, 'Tunable', 'WW', colorStr(2, 157, 0, 0, 0), 80, true),
  lamp(204, 'Reading Lamp', 'WW', colorStr(2, 157, 0, 0, 0), 40, false),
  lamp(205, 'Ceiling', 'RGBWW', colorStr(3, 0, 0, 255, 0), 40, false),
];
let hueDrift = 0, stuck = false;
// Domoticz hsb2rgb with saturation 1 and value 1
function hsb2rgb(hue) {
  const h = (((hue % 360) + 360) % 360) / 60, i = Math.floor(h), f = h - i;
  const [r, g, b] = [[1, f, 0], [1 - f, 1, 0], [0, 1, f], [0, 1 - f, 1], [f, 0, 1], [1, 0, 1 - f]][i % 6];
  return [r, g, b].map((x) => Math.round(x * 255));
}
function setColour(d, tool, a) {
  if (d.Type !== 'Color Switch') return `Error: "${d.Name}" is not a colour light.`;
  if (stuck) return null;
  const col = JSON.parse(d.Color || '{}');
  if (tool === 'set_color_temperature') {
    if (!isNum(a.kelvin)) return false;
    const K = Math.max(2700, Math.min(6500, a.kelvin)), L = 100 * (6500 - K) / 3800;
    Object.assign(col, { m: 2, t: Math.round(L * 255 / 100), r: 0, g: 0, b: 0 });
    d.Color = JSON.stringify(sorted(col)); touch(d);
    return null;
  }
  if (!isNum(a.hue) || !isNum(a.brightness)) return false;
  const white = a.iswhite === true;
  if (!white && d.SubType === 'WW') return `Error: "${d.Name}" has no colour.`;
  const hue = Math.max(0, Math.min(360, a.hue)), br = Math.max(0, Math.min(100, Math.round(a.brightness)));
  if (white) Object.assign(col, { m: 1, t: 0, r: 0, g: 0, b: 0 });
  else { const [r, g, b] = hsb2rgb(hue + hueDrift); Object.assign(col, { m: 3, t: 0, r, g, b }); }
  d.Color = JSON.stringify(sorted(col));
  d.Level = d.LevelInt = Math.round(br * d.MaxDimLevel / 100);
  d.Status = br ? levelText(d) : 'Off'; d.Data = br ? 'On' : 'Off';
  touch(d);
  return null;
}
const F = (c) => Math.round((c * 9 / 5 + 32) * 10) / 10;
const toImperial = (d) => {
  const o = { ...d };
  for (const k of ['Temp', 'Chill']) if (typeof o[k] === 'number') o[k] = F(o[k]);
  if (o.DewPoint !== undefined) o.DewPoint = typeof o.DewPoint === 'number' ? F(o.DewPoint) : F(Number(o.DewPoint)).toFixed(2);
  if (o.vunit === '°C') {
    o.vunit = '°F';
    for (const k of ['min', 'max']) o[k] = F(o[k]);
    if (o.SetPoint !== undefined) o.SetPoint = typeof o.SetPoint === 'number' ? F(o.SetPoint) : F(Number(o.SetPoint)).toFixed(1);
    if (o.Data === d.SetPoint) o.Data = o.SetPoint;
  } else if (o.SetPoint !== undefined && o.HaveSetPoint) {
    o.SetPoint = typeof o.SetPoint === 'number' ? F(o.SetPoint) : F(Number(o.SetPoint)).toFixed(1);
    if (o.Data === d.SetPoint) o.Data = o.SetPoint;
  }
  for (const k of ['Speed', 'Gust']) if (o[k] !== undefined) o[k] = (Number(o[k]) * 2.2369).toFixed(1);
  if (typeof o.Data === 'string') o.Data = o.Data.replace(/(-?\d+(?:\.\d+)?) C\b/g, (_, n) => F(Number(n)).toFixed(1) + ' F');
  return o;
};
const CONFIGS = {
  metric: { TempSign: 'C', TempScale: 1, WindSign: 'm/s', WindScale: 10, CurrencySign: '\u20ac' },
  imperial: { TempSign: 'F', TempScale: 1, WindSign: 'mph', WindScale: 22.369, CurrencySign: '$' },
};
let units = 'metric', withClimate = process.argv.includes('--climate'), withColour = process.argv.includes('--colour');

const makeScenes = () => [
  ['1', 'Movie', 'Scene'], ['2', 'Night', 'Scene'], ['3', 'Away', 'Group'], ['4', 'All Off', 'Group'],
].map(([idx, Name, Type]) => ({ idx, Name, Type, Status: 'Off', Data: 'Off', Favorite: 1, Protected: false, LastUpdate: stamp(),
  OffAction: '', OnAction: '', Timers: 'false', UsedByCamera: false }));

let devices, scenes;
const reset = () => {
  units = 'metric'; withClimate = process.argv.includes('--climate'); withColour = process.argv.includes('--colour'); hueDrift = 0; stuck = false;
  devices = makeDevices(); scenes = makeScenes();
  if (withClimate) devices.push(...makeClimate());
  if (withColour) devices.push(...makeColour());
  devices.forEach((d) => { d._t = epoch(d.LastUpdate); });
  scenes.forEach((s) => { s._t = epoch(s.LastUpdate); });
  const g = devices.find((d) => d.Type === 'Group'); delete g.SubType;
  cfg.mode = 'none'; cfg.count = 0; cfg.push = 'skip'; cfg.latency = arg('latency', 0); cfg.fail = arg('fail', 0);
  stats.requests = 0; stats.urls = []; stats.mcpInits = 0; stats.mcpCalls = []; stats.mcpLastSession = 'absent'; sessions.clear();
};
reset();

const byIdx = (list, i) => list.find((d) => d.idx === String(i));
// LastUpdate always moves: two commands within one second still differ (the firmware's push verification compares the stamps)
const touch = (d) => {
  let n = stamp();
  if (n === d.LastUpdate) n = stamp(Date.parse(d.LastUpdate.replace(' ', 'T') + 'Z') + 1000);
  d.LastUpdate = n; d._t = Math.floor(Date.now() / 1000);
};
const clean = ({ _t, ...d }) => (units === 'imperial' ? toImperial(d) : d);
const sorted = (o) => Array.isArray(o) ? o.map(sorted)
  : o && typeof o === 'object' ? Object.fromEntries(Object.keys(o).sort().map((k) => [k, sorted(o[k])])) : o;
const pretty = (o) => JSON.stringify(sorted(o), null, '\t');
const send = (res, code, text, type = 'application/json') => { res.writeHead(code, { 'Content-Type': type }); res.end(text); return true; };
const reply = (res, code, obj) => send(res, code, pretty(obj));

function levelText(d) { return d.Level >= d.MaxDimLevel ? 'On' : `Set Level: ${d.Level} %`; }

function doSwitch(d, cmd, level, mcp = false) {
  const v = d.SwitchTypeVal;
  if (d.Protected && !mcp) return { status: 'ERROR', message: 'WRONG CODE' };
  if (d.Type !== 'Light/Switch' && d.Type !== 'Color Switch' && !d.Type.startsWith('Lighting')) return { status: 'ERR' };
  if (cmd === 'Toggle') cmd = (d.Status === 'Off' || d.Status === 'Closed' || d.Status === 'Unlocked') ? 'On' : 'Off';
  if (cmd === 'On' || cmd === 'Off') {
    if (v === 9 || v === 10) { d.Status = ''; d.Data = cmd; touch(d); return { status: 'OK', title: 'SwitchLight' }; }
    if (v === 19) d.Status = cmd === 'On' ? 'Locked' : 'Unlocked';       // normal lock: On = Locked
    else if (v === 20) d.Status = cmd === 'On' ? 'Unlocked' : 'Locked';
    else if (v === 18) return { status: 'ERR' };
    else if (v === 3 || v === 13) return { status: 'ERR' };
    else if (v === 7 || v === 0) {
      d.Status = cmd;
      if (v === 7 && cmd === 'On') { if (!d.Level) d.Level = d.MaxDimLevel; d.LevelInt = d.Level; d.Status = levelText(d); }
      // Off leaves the stored Level alone (research 7.1)
    }
  } else if (cmd === 'Set Level') {
    const n = Number(level);
    if (level === null || level === '' || !Number.isFinite(n) || n < 0 || n > (d.MaxDimLevel || 100)) return { status: 'ERROR', message: 'bad level' };
    if (v === 18) {
      const names = Buffer.from(d.LevelNames, 'base64').toString().split('|');
      if (n % 10 || n / 10 >= names.length) return { status: 'ERR' };
      d.Level = d.LevelInt = n; d.Status = ''; d.Data = names[n / 10];
    } else if (v === 7) {
      d.Level = d.LevelInt = n; d.Status = n ? levelText(d) : 'Off';
    } else if (mcp && (v === 13 || v === 21)) {
      d.Level = d.LevelInt = n; d.Status = n >= 100 ? 'Open' : n === 0 ? 'Closed' : `Set Level: ${n} %`;
    } else return { status: 'ERR' };
  } else if (cmd === 'Open' || cmd === 'Close' || cmd === 'Stop') {
    if (v !== 3 && v !== 13 && v !== 14 && v !== 15 && v !== 21 && v !== 22) return { status: 'ERR' };
    if (cmd !== 'Stop') d.Status = cmd === 'Open' ? 'Open' : 'Closed';
  } else return { status: 'ERROR', message: 'unknown switchcmd' };
  if (v !== 18) d.Data = d.Status;
  touch(d);
  return { status: 'OK', title: 'SwitchLight' };
}

function doSetpoint(d, sp) {
  if (d.min !== undefined && (sp < d.min || sp > d.max)) return false;
  if (sp < 5 && d.min === undefined || sp > 35 && d.min === undefined) return false;
  d.SetPoint = d.Data = sp.toFixed(1); touch(d);
  return true;
}

function sunFields() {
  return { Sunrise: '07:35', Sunset: '17:14', DayLength: '09:38', SunAtSouth: '12:25', CivTwilightStart: '07:00', CivTwilightEnd: '17:50',
    NautTwilightStart: '06:19', NautTwilightEnd: '18:30', AstrTwilightStart: '05:40', AstrTwilightEnd: '19:09', AllowWidgetOrdering: false };
}

// getdevices filter=light|temp|weather|utility, written from Domoticz main/Helper.cpp (IsLightOrSwitch, IsTemp,
// IsWeather, IsUtility) and WebServer.cpp (the filter=light extras), on the JSON Type/SubType strings of RFXNames.cpp.
// Deliberately NOT shared with the C table in components/int_domoticz/dz_tabs.c: the two must agree (tests check it).
const TABS = { light: 1, temp: 1, weather: 1, utility: 1 };
const GENERAL_UTILITY = ['kWh', 'Voltage', 'Current', 'Text', 'Percentage', 'Counter Incremental', 'Managed Counter', 'Waterflow',
  'Custom Sensor', 'Alert', 'Pressure', 'Soil Moisture', 'Leaf Wetness', 'Sound Level', 'Fan', 'Distance'];
function tabOf(type, sub) {
  const t = new Set();
  const isType = (...names) => names.includes(type);
  // light: IsLightOrSwitch + Evohome controller/relay (Rego 6XX status cannot be told from the counter by string: see dz_tabs.c)
  if (isType('Lighting 1', 'Lighting 2', 'Lighting 3', 'Lighting 4', 'Lighting 5', 'Lighting 6', 'Fan', 'Color Switch', 'Security',
    'Curtain', 'Blinds', 'Chime', 'RFY', 'Thermostat 2', 'Thermostat 3', 'Thermostat 4', 'Remote & IR', 'Light/Switch', 'Home Confort',
    'FS20', 'Hunter', 'Honeywell')) t.add('light');
  if (type === 'Radiator 1' && sub === 'Smartwares Mode') t.add('light');
  if (type === 'Heating' && (sub === 'Evohome' || sub === 'Relay')) t.add('light');
  // temp
  if (isType('Temp', 'Humidity', 'Temp + Humidity', 'Temp + Humidity + Baro', 'Temp + Baro', 'Thermostat 1', 'Thermostat 6')) t.add('temp');
  if (type === 'Heating' && (sub === 'Zone' || sub === 'Hot Water')) t.add('temp');
  if ((type === 'Wind' || type === 'UV') && sub === 'TFA') t.add('temp');   // WIND4 and UV3 are both "TFA"
  if (type === 'General' && sub === 'System temperature') t.add('temp');
  if (type === 'RFXSensor' && sub === 'Temperature') t.add('temp');
  // weather
  if (isType('Wind', 'Rain', 'Temp + Humidity + Baro', 'Temp + Baro', 'UV')) t.add('weather');
  if (type === 'General' && ['Visibility', 'Barometer', 'Solar Radiation'].includes(sub)) t.add('weather');
  // utility
  if (isType('P1 Smart Meter', 'Lux', 'Current', 'Current/Energy', 'Energy', 'Power', 'YouLess Meter', 'Air Quality', 'Usage', 'Weight', 'RFXMeter')) t.add('utility');
  if (type === 'Setpoint' && sub === 'SetPoint') t.add('utility');
  if (type === 'General' && GENERAL_UTILITY.includes(sub)) t.add('utility');
  if (type === 'RFXSensor' && (sub === 'Voltage' || sub === 'A/D')) t.add('utility');
  if (type === 'Radiator 1' && sub === 'Smartwares') t.add('utility');
  return t;
}

function modeFault(req, res, mcp) {
  const mode = cfg.mode;
  const active = mode !== 'none' && (mcp || !mode.startsWith('nomcp'));
  if (active && cfg.count > 0 && --cfg.count === 0) cfg.mode = 'none'; // `count` limits how many requests are hit
  if (active) {
    switch (mode) {
      case 'nomcp': return send(res, 503, 'MCP disabled', 'text/plain');
      case 'nomcp404': return send(res, 404, '<html><body>Not Found</body></html>', 'text/html');
      case 'html': return send(res, 200, '<!DOCTYPE html><html><head><title>Proxy</title></head><body><h1>Login required</h1></body></html>', 'text/html');
      case 'http500': return send(res, 500, '<html>Internal Server Error</html>', 'text/html');
      case 'http502': return send(res, 502, '<html><body>Bad Gateway</body></html>', 'text/html');
      case 'http404': return send(res, 404, '<html><body>Not Found</body></html>', 'text/html');
      case '401': return send(res, 401, 'Unauthorized', 'text/plain');
      case '403': return send(res, 403, 'Forbidden', 'text/plain');
      case 'drop': { req.socket.destroy(); return true; }
      case 'hang': return true;
      case 'truncate': res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': 5000 });
        res.write('{"ActTime": 1, "result": [{"idx": "1", "Name": "Cut'); setTimeout(() => req.socket.destroy(), 30); return true;
      case 'huge': return send(res, 200, '{"status":"OK","title":"Devices","pad":"' + 'x'.repeat(300000) + '"}');
      case 'garbage': return send(res, 200, '\x00\x01not json at all{{{[[');
      case 'statuserr': return reply(res, 200, { status: 'ERR', title: 'Devices' });
      case 'nostatus': return reply(res, 200, { title: 'Devices', result: [] });
      case 'chunked': res.writeHead(200, { 'Content-Type': 'application/json' });
        res.write('{"status":"OK",'); setTimeout(() => res.end('"title":"GetVersion","version":"2026.4"}'), 10); return true;
      case 'slowbody': res.writeHead(200, { 'Content-Type': 'application/json' }); res.write('{"status":"OK",'); return true;
      default: break;
    }
  }
  return false;
}

function handleJson(req, res, u) {
  const m = /^Bearer (.+)$/.exec(req.headers.authorization || '');
  stats.lastAuth = m ? 'present' : 'absent';
  const rights = m && m[1] in TOKENS ? TOKENS[m[1]] : -1;
  if (modeFault(req, res, false)) return undefined;
  if (rights < 0) return reply(res, 401, { status: 'ERR' });
  if (cfg.fail && Math.random() < cfg.fail) return reply(res, 500, { status: 'ERR' });
  const q = u.searchParams, p = q.get('param');
  if (q.get('type') !== 'command') return reply(res, 400, { status: 'ERR' });
  const ok = (o) => reply(res, 200, { status: 'OK', ServerTime: stamp(), ...o });

  if (p === 'getversion') return ok({ version: VERSION, dzvents_version: '3.1.8', python_version: '3.12.0', hash: 'mock', build_time: 'mock', title: 'GetVersion' });
  // getconfig: the real one also lists the home coordinates and many settings. The mock returns the five unit keys
  // plus a fake canary position, so clients can be tested to ignore everything else.
  if (p === 'getconfig') return ok({ title: 'GetConfig', ...CONFIGS[units], Latitude: '12.345678', Longitude: '98.765432', WebUserName: 'mock-user' });
  if (p === 'getdevices') {
    let r = devices;
    const tab = q.get('filter');
    if (TABS[tab]) r = r.filter((d) => tabOf(d.Type, d.SubType).has(tab)); // Domoticz GetJSonDevices: scenes only come with filter=all
    if (q.get('rid')) r = r.filter((d) => d.idx === q.get('rid') && d.Type !== 'Group');
    if (q.get('favorite') === '1') r = r.filter((d) => d.Favorite === 1);
    if (q.get('plan')) r = r.filter((d) => (d.PlanIDs || []).includes(Number(q.get('plan'))));
    const lu = Number(q.get('lastupdate') || 0);
    if (lu) r = r.filter((d) => d._t > lu);
    const out = { ActTime: Math.floor(Date.now() / 1000), ...sunFields(), app_version: VERSION, title: 'Devices' };
    if (r.length) out.result = r.map(clean);   // real server OMITS result when a delta is empty
    return ok(out);
  }
  if (p === 'getscenes') return ok({ ActTime: Math.floor(Date.now() / 1000), ...sunFields(), title: 'Scenes', result: scenes.map(clean) });
  if (p === 'getplans') return ok({ title: 'Plans', result: [{ Devices: 5, Name: 'Living', Order: 1, idx: '1' }].concat(withClimate ? [7, 8, 9].map((n) => ({ Devices: 2, Name: 'Room ' + n, Order: n, idx: String(n) })) : []) });

  // control: needs User rights
  if (['switchlight', 'switchscene', 'setsetpoint', 'thermostatstate'].includes(p)) {
    if (rights < 1) return reply(res, 403, { status: 'ERR' });
    if (p === 'thermostatstate') {
      // mirrors Cmd_SetThermostatState: only idx and state; no validation of the index on the server side either
      const d = byIdx(devices, q.get('idx')), st = Number(q.get('state'));
      if (!d || d.Modes === undefined || !Number.isInteger(st)) return ok({ status: 'ERR' });
      const names = d.Modes.split(';').filter((_, i) => i % 2 === 1);
      if (st < 0 || st >= names.length) return ok({ status: 'ERR' });
      d.Mode = st; d.Data = names[st]; touch(d);
      return ok({ title: 'Set Thermostat State' });
    }
    if (p === 'switchlight') {
      const d = byIdx(devices, q.get('idx'));
      return d ? ok(doSwitch(d, q.get('switchcmd'), q.get('level'))) : ok({ status: 'ERR' });
    }
    if (p === 'switchscene') {
      const s = byIdx(scenes, q.get('idx')), c = q.get('switchcmd');
      if (!s || !['On', 'Off'].includes(c) || (c === 'Off' && s.Type === 'Scene')) return ok({ status: 'ERR' });
      s.Status = s.Data = c; touch(s);
      const g = devices.find((d) => d.Type === 'Group' && d.idx === s.idx);
      if (g) { g.Status = g.Data = c; touch(g); }
      return ok({ title: 'SwitchScene' });
    }
    const d = byIdx(devices, q.get('idx'));
    let sp = Number(q.get('setpoint'));
    if (!d || d.SetPoint === undefined || !Number.isFinite(sp)) return ok({ status: 'ERR' });
    if (units === 'imperial' && d.vunit === '°C') sp = Math.round((sp - 32) * 5 / 9 * 10) / 10;  // the server converts F -> C itself
    return doSetpoint(d, sp) ? ok({ title: 'SetSetpoint' }) : ok({ status: 'ERR' });
  }
  return ok({ status: 'ERR', message: 'unknown command' });
}


// ---------------------------------------------------------------- MCP (POST /mcp)
const TOOLS = ['set_switch_state', 'toggle_switch_state', 'set_dimmer_level', 'control_blinds', 'switch_scene', 'set_setpoint_value',
  'set_color_brightness', 'set_color_temperature']
  .map((name) => ({ name, description: name, inputSchema: { type: 'object' } }));
const TOOLS_FILLER = Array.from({ length: 4 }, (_, i) => ({ name: 'get_filler_' + i, description: 'x', inputSchema: { type: 'object' } }));

function mcpRespond(res, id, body) { return send(res, 200, JSON.stringify({ id, jsonrpc: '2.0', ...body })); }
const rpcErr = (code, message) => ({ error: { code, message } });
const textResult = (text, isError = false) => ({ result: { content: [{ text, type: 'text' }], isError } });
const isNum = (x) => typeof x === 'number' && Number.isFinite(x);
const isOnStatus = (d) => d.Status !== 'Off';

function mcpTool(name, a, rights) {
  if (!TOOLS.some((t) => t.name === name)) return rpcErr(-32601, 'Tool not found');
  if (rights < 1) return rpcErr(-32002, 'Permission denied');
  a = a && typeof a === 'object' ? a : {};
  if (name === 'switch_scene') {
    if (!isNum(a.scene_id) || !['On', 'Off'].includes(a.command)) return rpcErr(-32602, 'Invalid params');
    const sc = byIdx(scenes, a.scene_id);
    if (!sc) return textResult(`Scene ${a.scene_id} not found`, true);
    if (a.command === 'Off' && sc.Type === 'Scene') return textResult(`Error switching scene "${sc.Name}".`);
    sc.Status = sc.Data = a.command; touch(sc);
    const g = devices.find((d) => d.Type === 'Group' && d.idx === sc.idx);
    if (g) { g.Status = g.Data = a.command; touch(g); }
    return textResult(`Scene "${sc.Name}" switched ${a.command} successfully.`);
  }
  if (!isNum(a.idx)) return rpcErr(-32602, 'Invalid params');
  const d = byIdx(devices, a.idx);
  if (!d || d.Type === 'Group') return textResult(`Device with idx ${a.idx} not found`, true);
  let cmd, level = null, word = '';
  switch (name) {
    case 'set_switch_state':
      if (!['On', 'Off'].includes(a.state)) return rpcErr(-32602, 'Invalid params');
      cmd = a.state; word = a.state;
      if ((d.SwitchTypeVal === 0 || d.SwitchTypeVal === 7) && isOnStatus(d) === (cmd === 'On'))
        return textResult(`Switch "${d.Name}" was already ${cmd}. No action taken.`);
      // Push buttons keep their last state (a Push On button stays On). push=skip (default): the real tool's "already ... No action
      // taken" skip is assumed to apply to them too, so a second press does nothing; push=force: every call fires.
      if ((d.SwitchTypeVal === 9 || d.SwitchTypeVal === 10) && cfg.push === 'skip' && d.Data === cmd)
        return textResult(`Switch "${d.Name}" was already ${cmd}. No action taken.`);
      break;
    case 'toggle_switch_state': cmd = 'Toggle'; word = 'toggled'; break;
    case 'set_dimmer_level':
      if (!isNum(a.level)) return rpcErr(-32602, 'Invalid params');
      cmd = 'Set Level'; level = String(Math.max(0, Math.min(100, Math.round(a.level)))); word = `level ${level}`;
      break;
    case 'control_blinds':
      if (!['Open', 'Close', 'Stop'].includes(a.command)) return rpcErr(-32602, 'Invalid params');
      cmd = a.command; word = a.command; break;
    case 'set_color_brightness': case 'set_color_temperature': {
      const e = setColour(d, name, a);
      if (e === false) return rpcErr(-32602, 'Invalid params');
      if (e) return textResult(e);
      return textResult(`Color of "${d.Name}" set successfully.`);
    }
    default: { // set_setpoint_value: the value goes in as-is (no Fahrenheit conversion), like the real tool
      if (!isNum(a.setpoint)) return rpcErr(-32602, 'Invalid params');
      if (d.SetPoint === undefined || !doSetpoint(d, a.setpoint)) return textResult(`Error setting setpoint of "${d.Name}".`);
      return textResult(`Setpoint of "${d.Name}" set to ${a.setpoint} successfully.`);
    }
  }
  const r = doSwitch(d, cmd, level, true);   // mcp = true: Protected is NOT checked
  if (r.status !== 'OK') return textResult(`Error switching "${d.Name}".`);
  return textResult(`Switch "${d.Name}" set to ${word} successfully.`);
}

function handleMcp(req, res) {
  if (modeFault(req, res, true)) return;
  const m = /^Bearer (.+)$/.exec(req.headers.authorization || '');
  stats.lastAuth = m ? 'present' : 'absent';
  const rights = m && m[1] in TOKENS ? TOKENS[m[1]] : -1;
  if (rights < 0) return send(res, 401, JSON.stringify({ error: 'Unauthorized' }));
  if (req.headers['mcp-protocol-version']) return send(res, 400, JSON.stringify({ error: 'Unsupported protocol version' }));
  let raw = '';
  req.on('data', (c) => { raw += c; if (raw.length > 8192) req.destroy(); });
  req.on('end', () => {
    stats.requests++;
    let j;
    try { j = JSON.parse(raw); } catch (e) { return mcpRespond(res, null, rpcErr(-32700, 'Parse error')); }
    if (!j || j.jsonrpc !== '2.0' || typeof j.method !== 'string') return mcpRespond(res, j && j.id !== undefined ? j.id : null, rpcErr(-32600, 'Invalid Request'));
    const sid = req.headers['mcp-session-id'];
    stats.mcpLastSession = sid ? 'present' : 'absent';
    if (j.method === 'initialize') {
      const id = require('crypto').randomBytes(16).toString('hex');
      sessions.add(id); stats.mcpInits++;
      res.writeHead(200, { 'Content-Type': 'application/json', 'Mcp-Session-Id': id });
      return res.end(JSON.stringify({ id: j.id, jsonrpc: '2.0', result: { protocolVersion: '2025-06-18', capabilities: { tools: {} },
        serverInfo: { name: 'Domoticz', version: VERSION } } }));
    }
    if (j.method.startsWith('notifications/')) return send(res, 202, '<html><body>Accepted</body></html>', 'text/html');
    if (!sid) return mcpRespond(res, j.id, rpcErr(-32600, 'Missing session'));
    if (!sessions.has(sid)) return mcpRespond(res, j.id, rpcErr(-32001, 'Session not found'));
    if (j.method === 'tools/list') {
      const all = TOOLS.concat(TOOLS_FILLER), cur = j.params && j.params.cursor;
      return mcpRespond(res, j.id, cur === 'p2' ? { result: { tools: all.slice(3) } } : { result: { tools: all.slice(0, 3), nextCursor: 'p2' } });
    }
    if (j.method !== 'tools/call') return mcpRespond(res, j.id, rpcErr(-32601, 'Method not found'));
    const p = j.params || {};
    stats.mcpCalls.push({ tool: String(p.name), args: p.arguments });
    if (stats.mcpCalls.length > 200) stats.mcpCalls.shift();
    return mcpRespond(res, j.id, mcpTool(p.name, p.arguments, rights));
  });
}

http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  if (u.pathname.startsWith('/__mock/')) {
    const q = u.searchParams;
    if (u.pathname === '/__mock/set') {
      if (q.has('latency')) cfg.latency = Number(q.get('latency'));
      if (q.has('fail')) cfg.fail = Number(q.get('fail'));
      if (q.has('mode')) cfg.mode = q.get('mode');
      if (q.has('push') && ['skip', 'force'].includes(q.get('push'))) cfg.push = q.get('push');
      if (q.has('units') && CONFIGS[q.get('units')]) units = q.get('units');
      if (q.has('climate') && q.get('climate') === '1' && !withClimate) {
        withClimate = true;
        const c = makeClimate(); c.forEach((d) => { d._t = epoch(d.LastUpdate); }); devices.push(...c);
      }
      if (q.get('colour') === '1' && !withColour) {
        withColour = true;
        const c = makeColour(); c.forEach((d) => { d._t = epoch(d.LastUpdate); }); devices.push(...c);
      }
      if (q.has('hue_drift')) hueDrift = Number(q.get('hue_drift')) || 0;
      if (q.has('stuck')) stuck = q.get('stuck') === '1';
      if (q.get('mcp') === 'expire') sessions.clear();
      cfg.count = q.has('count') ? Number(q.get('count')) : 0;
    } else if (u.pathname === '/__mock/reset') reset();
    else if (u.pathname === '/__mock/touch') {
      const d = byIdx(devices, q.get('idx')) || byIdx(scenes, q.get('idx'));
      if (!d) return send(res, 404, '{}');
      if (q.has('status')) d.Status = d.Data = q.get('status');
      if (q.has('timeout')) d.HaveTimeout = q.get('timeout') === '1';
      if (q.has('protected')) d.Protected = q.get('protected') === '1';
      touch(d);
    } else if (u.pathname === '/__mock/stats') return send(res, 200, JSON.stringify(stats));
    return send(res, 200, '{"ok":true}');
  }
  const doit = () => {
    if (u.pathname === '/mcp') {
      if (req.method !== 'POST') return send(res, 405, 'Method Not Allowed', 'text/plain');
      return handleMcp(req, res);
    }
    if (u.pathname !== '/json.htm') return reply(res, 404, { status: 'ERR' });
    stats.requests++;
    stats.urls.push(u.search.replace(/token=[^&]*/g, 'token=<redacted>').slice(0, 160));
    if (stats.urls.length > 200) stats.urls.shift();
    handleJson(req, res, u);
  };
  cfg.latency ? setTimeout(doit, cfg.latency) : doit();
}).listen(PORT, () => console.log(`mock Domoticz on :${PORT}  tokens: viewer-token, user-token`));
