# Fixtures

- `real_getdevices.json`, `real_getscenes.json`: **SYNTHETIC.** Originally shaped from a capture of a Domoticz 2026.4 server, now rewritten: idx values are renumbered 1..31
  (one stable mapping for devices and scenes), names are `Device N` / `Scene N`, hardware is `Hardware A..L` or `Dummy`, `LastUpdate` times are fixed
  offsets from the file's `ServerTime` (quantised), meter totals and prices are round numbers. Type, SubType, SwitchType and Data patterns are kept because
  the parser tests depend on them. The file names keep the `real_` prefix; do not paste a new capture over them without the same scrubbing.
- `mock_getdevices.json`, `mock_getscenes.json`, `mock_getversion.json`: hand-written.
- `mock_climate_metric.json`, `mock_climate_imperial.json`, `mock_getconfig_metric.json`, `mock_getconfig_imperial.json`:
  **HAND-WRITTEN, not captures.** They are derived from reading `CWebServer::GetJSonDevices`, `Cmd_GetConfig` and
  `Cmd_SetThermostatState` in `vendor/domoticz` (see `docs/research/domoticz-device-types.md`) and cover families the
  reference server does not have: air quality, lux, wind, rain, UV, General sensors (percentage, soil moisture, solar radiation,
  custom, text, pressure, alert), Thermostat 6, Radiator 1, Evohome zone ("Heating"), thermostat mode, an unseen type.
  The imperial files carry TempSign `F` / WindSign `mph` and converted values, as the real server converts before sending.
  The `Latitude`, `Longitude` and `WebUserName` keys in the getconfig files are fake canaries: no code may keep them.
  Regenerate with `node tests/tools/regen_climate_fixtures.js` (the mock is the single source).
- `switch_types.json`: **HAND-WRITTEN.** One device per Domoticz Switches-tab class (every SwitchTypeVal 0-20 on Light/Switch, plus Lighting 1/2, Color Switch,
  Blinds, RFY, Curtain, Chime, Security, Fan, Thermostat 3, Remote & IR, Heating Relay). Used by test_apps_view and the simulator Lights test.
- `mock_lights.json`: **HAND-WRITTEN.** Colour and white lamps in the shape of a Domoticz `getdevices` (Type `Color Switch`, the `Color` JSON string): RGBWW orange/Off/white mode/protected, RGB, RGBW, WW On and Off, and a plain dimmer. Used by the light detail screens of the simulator and the app tests.
