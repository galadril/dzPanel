# Domoticz device types and how to display them generically

Source: `vendor/domoticz` (`CWebServer::GetJSonDevices` in `main/WebServer.cpp`, `Cmd_SetSetpoint` / `Cmd_SetThermostatState` / `Cmd_GetConfig` in `main/WebServerCmds.cpp`), read 2026-11-01. The field table was extracted from the code, not from any one installation. Where a claim was also checked against a live 2026.4 server it says so. A user's own device list covers only some of these types, so **no design may be based on one server's devices**.

## 1. Every device has

`idx, Name, Description, Type, SubType, Data (display string), Status, LastUpdate, Favorite, Protected, PlanID/PlanIDs, Image, TypeImg, HaveTimeout, BatteryLevel, SignalLevel`. Sensors add `trend` on several types.

## 2. Families, by the fields they carry

| Family (Domoticz `Type`) | Fields beyond the common set | Notes |
|---|---|---|
| **Temp** (`Temp`, RFXSensor, Thermostat1, Evohome zone) | `Temp`, `trend` | Evohome zone also `Until`, `Status` (Auto / Override…) |
| **Humidity** (`Humidity`) | `Humidity`, `HumidityStatus` | alone |
| **Temp+Hum**, **+Baro** | `Temp`, `Humidity`, `HumidityStatus`, `DewPoint`, `Barometer`, `Forecast`, `ForecastStr`, `trend` | Baro-only variant too |
| **Wind** | `Speed`, `Gust`, `Direction` (deg), `DirectionStr`, `Chill`, `Temp` | Speed and Gust arrive **without a unit**; the unit is `WindSign` from `getconfig`. `Temp` here is a wind sensor's own temperature, not "the weather temperature" |
| **Rain** | `Rain` (total), `RainRate` | unit not in the field |
| **UV** | `UVI`, `Temp` | |
| **Lux** | `Data` only ("123 Lux") | |
| **Air quality** | `Quality` | ppm in `Data` |
| **General** (many subtypes) | `Data`, `SensorUnit`, `ValueQuantity/ValueUnits`, plus per subtype: `Visibility`, `Radiation`, `Pressure`, `Barometer`, `Voltage`, `Current`, `Counter*`, `Level`, `Mode`, `Modes`, `State` | subtypes: Percentage, Pressure, Visibility, Solar Radiation, Soil Moisture, Leaf Wetness, Distance, Custom Sensor, Alert, Text, Counter Incremental, Managed Counter, kWh, Voltage, Current, Fan, Thermostat Mode… **The only universal handle is `Data` plus the unit it contains** |
| **Energy / power** (`Usage`, `ENERGY`, P1 Smart Meter, YouLess, RFXMeter, CURRENT, CURRENTENERGY) | `Usage`, `UsageDeliv`, `Counter`, `CounterDeliv`, `CounterToday`, `CounterDelivToday`, `Divider`, `ValueQuantity/ValueUnits`, `displaytype` | `Data` of P1 is a `;`-separated string. Gas and water are counters, not power |
| **Setpoint** (`Setpoint`, `Thermostat6`, `Radiator1`, Evohome zone/water) | `SetPoint`, `min`, `max`, `step`, `vunit`, `HaveSetPoint` | `Thermostat6` also carries Temp/Humidity/Baro/Forecast. A plain `Setpoint` is **just a number**: no current temperature, no "heating on" |
| **Thermostat mode** (`General` / thermostat or fan mode) | `Mode` (index), `Modes` (names) | compiled only with OpenZWave. Set with `thermostatstate&idx=&state=` |
| **Switches** (Lighting 1-6, Light/Switch, Color Switch, Security, Blinds, Chime, Curtain…) | `SwitchType`, `SwitchTypeVal`, `Level`, `LevelInt`, `MaxDimLevel`, `LevelNames`, `Color`, `HaveDimmer`… | see `docs/research/domoticz-api.md` |
| **Scenes / Groups** | in `getscenes`, not `getdevices` | Scene = push; Group = On/Off/Mixed |

`SwitchTypeVal` also appears on **meters and sensors** with a different meaning (P1, RFXMeter, General). Only trust it for switch-family types.

## 3. Units: never print a unit you did not read

`GetJSonDevices` converts `Temp`, `SetPoint`, `Chill` and `DewPoint` to the **user's chosen temperature unit** before sending. The unit itself is published once in `getconfig`:

```
TempSign ("C" or "F"), TempScale, WindSign ("m/s", "km/h", "mph", "kn", "bft"), WindScale, CurrencySign
```

Checked live: `TempSign: "C"`, `WindSign: "m/s"` on the reference server. A display that hardcodes "°C" is wrong for every Fahrenheit user, and wind speed has no unit at all without `WindSign`. Rule: take units from `getconfig`; for anything generic, parse the unit out of `Data`; if neither exists, show the number with no unit.

## 4. Controlling heat: what the API actually offers

| Need | API | Status |
|---|---|---|
| Change a setpoint | `setsetpoint&idx=&setpoint=` (user rights; goes to `MainWorker::SetSetPoint`) | In the code. Live-tested only on the mock |
| Thermostat mode (OpenZWave) | `thermostatstate&idx=&state=<index>` using the names in `Modes` | In the code; **never exercised** |
| Evohome overrides, `switchmodal`, setpoint timers | other commands (`switchmodal`, `addsetpointtimer`…) | **Out of scope**: show their state read-only |
| "Is the heating on?" | **No generic field.** A setpoint has no heating state | Do not invent a "HEATING" label. A paired heater switch is the user's own wiring |
| Current room temperature for a plain Setpoint | Not in the device | Use the device's own `Temp` if it has one (Thermostat6, Evohome zone); else a temperature sensor in the **same Domoticz plan (room)** if there is exactly one; else show `--` |

## 5. Consequences for the design

1. Work from **readings, not types**: each device yields zero or more typed readings (temperature, humidity, pressure, wind, rain, UV, lux, power, energy, generic value with unit). Screens render readings, so a device type nobody has seen yet still displays through the generic path.
2. Unknown sensors are **never hidden**: a generic tile shows name, value and the unit taken from `Data`.
3. Units come from `getconfig`, fetched once and again on full refresh.
4. Heating control is limited to what is documented above and is behind a hold-to-confirm.
