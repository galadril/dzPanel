# Fixtures

- `real_*.json`: sanitised captures from a live Domoticz 2026.4 server (home coordinates and internal URLs removed).
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
