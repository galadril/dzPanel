# Domoticz API research

Read from the Domoticz source (`vendor/domoticz`, shallow clone of `master`, 2026.1 era) on 2026-10-31. Wiki pages were unreachable, so everything below comes from code and the in-repo docs (`SECURITY_SETUP.md`, `docs/mcp.wiki`). Nothing here has been run against a live instance yet. Field names marked *(check)* come from reading the builder code and need a real capture.

## 1. Authentication: access tokens

Domoticz can mint **Access Tokens** (Setup, More Options, Access Tokens; admin only). This is the right credential for the device.

| Property | Detail | Source |
|---|---|---|
| Format | Signed JWT, subject `at:<id>`, role claim 0/1/2 | `Cmd_CreateAccessToken` in `WebServerCmds.cpp` |
| Rights | `0` Viewer (read), `1` User (also switch), `2` Admin | same, and `docs/mcp.wiki` |
| Expiry | 30, 90, 365 days or never (about 10 years) | same |
| Shown | **Once**, at creation. The server stores only a SHA-256 hash | same |
| Use | `Authorization: Bearer <jwt>` on any request | `docs/mcp.wiki`, `test/python/test_api_sweep.py` |
| Revoke | Delete it in the UI; every request re-checks it exists and has not expired (403 otherwise) | `GetJSonPage` in `WebServer.cpp` |
| No user account | The token carries its own rights | `docs/mcp.wiki` |

Consequences for the firmware:

- **One long-lived secret, no OAuth dance, no refresh.** The OAuth2 flows exist (authorization code with PKCE, password, refresh) but need a browser or a user password. Tokens suit a screen with no keyboard.
- **Viewer token = a display that physically cannot switch anything.** Offer this as the default. A User token is opt-in and enables controls.
- **A token is only a few hundred bytes**, but keep it in NVS, never in logs, never in the repo. Provisioning needs a field for it (Improv custom field or the phone web form).
- Expiry handling: a 403 means expired or revoked. Show a clear "token rejected" state and stop polling (already in `DESIGN.md` section 7).
- Trusted networks need no token, but the device should still prefer one.
- Use HTTPS if the instance has it, but expect many LAN installs to be plain HTTP on 8080. The UI must say so honestly when a token is sent over HTTP.

## 2. The API surface we need

Everything is `GET /json.htm?type=command&param=<name>&...`, with the Bearer header. Responses carry `"status":"OK"` or `"ERR"`.

### Read

| Need | Call | Notes |
|---|---|---|
| Version and reachability | `getversion` (registered at viewer rights; whether it needs a token is unchecked) | Cheap health check and version gate |
| All devices | `getdevices&filter=all&used=true&order=Name` | The core call |
| Only changes | `getdevices&...&lastupdate=<epoch>` | Handler takes `lastupdate`; returns only newer devices. Big saving on an ESP32 |
| One device | `getdevices&rid=<idx>` | For a detail screen refresh |
| Favourites only | `getdevices&...&favorite=1` | **Domoticz already has a user-curated watch list**: favourites. Use it, and no on-device editing is needed |
| Rooms | `getplans`, then `getdevices&plan=<id>` | Rooms (plans) give a natural grouping |
| Scenes and groups | `getscenes` | Separate namespace from device idx |
| Server time and sunrise | `getconfig` / `getsunrisesunset` *(check)* | For the clock and weather screens |

### Web UI tabs (which page a device is on)

The Domoticz web UI lists a device on a tab chosen from its JSON `Type` and `SubType` strings. dzPanel mirrors it in `components/int_domoticz/dz_tabs.c` (`dz_tabs_of`, mirrors `Helper.cpp` IsLightOrSwitch / IsTemp / IsWeather / IsUtility) and the parser stores the result in `dz_device_t.tabs`. A device can be on several tabs or on none. Scenes and groups are their own list (`getscenes`).

| Tab (dzPanel page) | `getdevices` filter | Types (SubType decides where noted) |
|---|---|---|
| Lights (LIGHTS) = the full Switches tab | `light` | Light/Switch, Lighting 1-6, Color Switch, Fan, Security, Curtain, Blinds, Chime, RFY, Thermostat 2/3/4, Remote & IR, Home Confort, FS20, Hunter, Honeywell; Heating Evohome / Relay |
| Temperature (TEMPERATURE) | `temp` | Temp, Humidity, Temp + Humidity, Temp + Humidity + Baro, Temp + Baro, Thermostat 1, Thermostat 6, Heating Zone / Hot Water, RFXSensor Temperature, General System temperature, Wind TFA, UV TFA |
| Weather (WEATHER) | `weather` | Temp + Humidity + Baro, Temp + Baro, Rain, Wind, UV, General Visibility / Barometer / Solar Radiation |
| Utility (UTILITY) | `utility` | P1 Smart Meter, Lux, Current, Energy, Power, Usage, YouLess Meter, Air Quality, Weight, RFXMeter, Setpoint, General kWh / Voltage / Current / Text / Percentage / counters / Waterflow / Custom Sensor / Alert / Pressure / Soil Moisture / Leaf Wetness / Sound Level / Fan / Distance, RFXSensor Voltage / A/D |
| none | none | Type "Thermostat" (SubType SetPoint), unknown Type/SubType pairs: shown on Favourites only |

The ENERGY page of dzPanel is not a Domoticz tab: it is the energy devices (those with a usage or day counter) in a gauge view; they are also on UTILITY. Order inside every page is the server's own order (`dz_model_order`, the position in the last complete full response).

### Control: the MCP endpoint (needs User rights)

**dzPanel sends every command through the Domoticz MCP endpoint (`POST /mcp`). There is no legacy fallback for commands.** Reads (polling `getdevices`, `getscenes`, `getconfig`, `getversion`, the read-after-write verify) stay on `json.htm`, because MCP results are text only.

**Why not `json.htm`:** with a User-rights *access token* (`at:N`), `GET /json.htm?type=command&param=switchlight...` returns **HTTP 403**. `CWebServer::IsIdxForUser` looks the user up in `m_users`, which never contains access-token users (Admin tokens skip the check; `switchscene` has no such check). MCP works with the same token: verified live on 2026.4 (HTTP 200, `isError:false`, state changed, confirmed by re-reading) for `set_switch_state` On and Off and `set_dimmer_level` on a Philips Hue dimmer.

Handshake and calls (JSON-RPC 2.0 over Streamable HTTP; replies are plain `application/json`, not SSE):

1. `initialize`: `{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"dzPanel","version":"1"}}}` returns 200 and a response header `Mcp-Session-Id: <32 hex>`.
2. `notifications/initialized` (no id) returns 202 with an **HTML** body (not JSON, never parsed). The server ignores it, so a failure is not fatal.
3. Every later call sends `Mcp-Session-Id`. Without it: `Missing session`.
4. `tools/call`: `{"jsonrpc":"2.0","id":N,"method":"tools/call","params":{"name":"set_switch_state","arguments":{"idx":906,"state":"Off"}}}` returns `{"id":N,"jsonrpc":"2.0","result":{"content":[{"text":"Switch \"Kantoor 1\" set to Off successfully.","type":"text"}],"isError":false}}`.

Headers: `Content-Type: application/json`, `Accept: application/json, text/event-stream`, `Authorization: Bearer <token>`. **Never send `MCP-Protocol-Version`**: an older value than the server's gives HTTP 400.

| Command | Tool and arguments |
|---|---|
| On, Off | `set_switch_state {idx, state: "On"\|"Off"}` (a type 20 inverted lock gets the inverted word, as before) |
| Toggle | `toggle_switch_state {idx}` |
| Dim level | `set_dimmer_level {idx, level}` (MCP clamps to 0..100; a device with MaxDimLevel above 100 is refused at submit) |
| Selector | `set_dimmer_level {idx, level: <LevelInt>}` (level above 100 refused) |
| Blinds | `control_blinds {idx, command: "Open"\|"Close"\|"Stop"}` |
| Push button | `set_switch_state {idx, state: "On"}` (Push On) or `"Off"` (Push Off) |
| Scene, group | `switch_scene {scene_id: <JSON number>, command: "On"\|"Off"}` (`Off` only for groups; the id is a number, never a string) |
| Thermostat setpoint | `set_setpoint_value {idx, setpoint: <number>}`, **Celsius servers only** |
| Thermostat mode | **no tool: shown read-only** ("MODE: VIEW ONLY") |
| Colour | `set_color_brightness`, `set_color_temperature` exist but are not used yet |

The tools call the same `MainWorker::SwitchLight` as `switchlight`, so command words, the type 20 lock inversion and the blind inversion (done server-side from the device options, see below) behave like the legacy path.

Results are text only. `isError` is true only for "device or scene not found" and bad arguments. A switch that failed comes back `isError:false` with text starting `Error `. When the state already matches the text is `... was already X. No action taken.`: dzPanel accepts that (a device that is out of sync can no longer be forced), and the verify read decides. `tools/list` is paginated (50 per page, 55 tools); the firmware does not call it.

JSON-RPC error codes: -32600 invalid request (`Missing session`), -32601 method not found, -32602 invalid params, -32603 internal, -32000 server error, -32001 resource not found (`Session not found`), -32002 permission denied (a Viewer token), -32003 rate limit, -32004 timeout, -32700 parse error. `Missing session` and `Session not found` arrive as HTTP 200 JSON-RPC errors *before* the tool runs, so ONE re-initialize and ONE retry is safe. A transport error or timeout on `tools/call` is never retried (it may have run; a toggle would double): the verify read decides. No credentials: HTTP 401 with a JSON body. Server started with `-nomcp`: HTTP 503. Sessions are 32 hex characters, not tied to the IP or TCP connection, uncapped, pruned after about an hour idle; every request is still authenticated by its Bearer token. The firmware opens a session lazily before the first command, opens a new one after 55 minutes idle, and wipes the id like the token (never logged).

Failure mapping in the firmware (`DZ_FAIL_*`, shown in the feedback line): transport connect/TLS or a timeout in `initialize` = no connection; HTTP 401 = token rejected (and polling stops); HTTP 403 and -32002 = no switch rights; HTTP 404, 405, 503 and -32601 = MCP not available; other 5xx, -32000, -32003, -32004, -32603 and text `Error ...` = server error; a second session error after the retry, -32700, -32600, non-JSON 200 = bad response; `isError:true` and -32602 = rejected; otherwise sent, then verified.

**MCP skips Domoticz's Protected (passcode) check and its per-user device check.** dzPanel's own refusal is the only guard: protected devices are never controlled (risk class `LOCKED`, AGENT_CONTRACT rule 3), and a pre-flight re-read (one small `getdevices&rid=` or `getscenes`, right before the call) refuses a device that became protected since the last poll. It also refreshes the pre-state (lock type, push type).

Known gaps: thermostat **MODE** has no MCP tool, and **SETPOINT on a Fahrenheit server** has no safe path (the tool takes Celsius and Domoticz would store a Fahrenheit number as Celsius), so both are shown read-only. A push button that already reads the target state answers `No action taken`, the verify sees no new `LastUpdate` and reports NOT CONFIRMED (honest, accepted). Blinds: Domoticz applies inversion on the server (`ReverseState` / `ReversePosition` device options in `SwitchLightInt`; the old switch types 6 and 16 no longer exist in `RFXNames.h`, 21 is Blinds % + Stop and 22 Blinds + Stop), so the client sends plain Open / Close / Stop and never swaps them; the verify accepts `Open` or level 100 for open and `Closed` or level 0 for close. Not verified against a live inverted blind.

The Domoticz `json.htm` command URLs (`switchlight`, `switchscene`, `setsetpoint`, `thermostatstate`) are kept in the mock only, for tests of the read path and as documentation of what the server also offers.

Source for the switch commands: `WebServerCommands.cpp` (`switchlight`, `switchscene`) and the MCP tools (`SwitchLight` in `mainworker.cpp`).

**Safety already in the server:** on `json.htm` protected devices and scenes demand a passcode and a Viewer token is refused at the command level; **the MCP endpoint skips the passcode check**, so our own risk levels (`SAFE`, `CONFIRM`, `LOCKED`) and the protected refusal are the real guard.

### Live updates: WebSocket

`DomoticzWebsocketHandler.cpp` implements a socket on the same server. The client sends JSON events (`request`, `subscribe`, `unsubscribe`, `unsubscribe_devices`) and gets pushed device and scene updates for the devices it subscribed to. This replaces polling and is the better path for the foreground screen. It is **unverified**: the URL path, auth on the socket, and the message shapes need a live capture. Plan: polling with `lastupdate` first (v0.4), WebSocket as an upgrade.

### MCP endpoint

Domoticz 2026.1 serves an MCP endpoint at `/mcp`: JSON-RPC over streamable HTTP, same Bearer tokens. **It is the control path of dzPanel** (see "Control: the MCP endpoint" above). Polling and parsing stay on `json.htm` (MCP results are text only).

## 3. Device model: what the JSON gives us

`GetJSonDevices` (in `WebServer.cpp`) builds each entry. Common fields seen in the builder: `idx`, `Name`, `Type`, `SubType`, `Data` (display string), `Status`, `LastUpdate`, `Favorite`, `Protected`, `HaveTimeout`, `BatteryLevel`, `SignalLevel`, `PlanIDs`, `HardwareName`.

Controllable switches add `SwitchType` (text), `SwitchTypeVal` (number), `Level`, `LevelInt`, `MaxDimLevel`, `LevelNames` (base64, `|` separated, selector switches), `HaveDimmer`, `HaveGroupCmd`, `IsSubDevice`, `CameraIdx`, `Color`, `ReverseState`, `ReversePosition`. Sensors add `Temp`, `Humidity`, `HumidityStatus`, `DewPoint`, `Barometer`, `Forecast`, `ForecastStr`, `Chill`, `Rain`, `RainRate`, `Speed`, `Gust`, `Direction`, `DirectionStr`, `UVI`, `Visibility`, `Radiation`, `Usage`, `UsageDeliv`, `Counter`, `CounterToday`, `CounterDelivToday`, `SetPoint`, `Mode`, `Modes`, `Voltage`, `Current`.

`SwitchTypeVal` maps to the `STYPE_*` enum in `main/RFXNames.h`:

| Val | Type | Screen |
|---:|---|---|
| 0 | On/Off | On/off |
| 7 | Dimmer | Dimmer |
| 9 / 10 | Push On / Push Off | Push button |
| 2 / 11 | Contact / Door contact | Read-only status |
| 3, 13, 21, 22 (and 14, 15) | Blinds, percentage, with stop, venetian | Blinds |
| 18 | Selector | Fan or mode selector |
| 19 / 20 | Door lock / inverted | Lock (hold) |
| 5, 8, 12 | Smoke, motion, dusk | Read-only status |
| 1 | Doorbell | Push-style |
| 17 | Media | Out of scope |

Sensors have no switch type, so the screen is chosen from `Type` and which fields exist: has `Temp` and `Humidity` means temp view; has `SetPoint` means thermostat; has `Forecast`, `Speed` or `Rain` means weather; has `Usage` or `Counter` means energy.

**Rule for the client:** pick the screen from a **capability** (`on_off`, `dimmer`, `blinds`, `selector`, `thermostat`, `temp`, `weather`, `energy`, `push`, `lock`, `scene`), never from a raw type. Unknown types fall back to a read-only tile showing `Name` and `Data`. This keeps us safe across Domoticz versions.

## 4. Fit with the design

- **Watch list = Domoticz favourites** (or a room). No editing UI on the device, which matches "settings and editing happen in Domoticz".
- **Honest values** map directly: `HaveTimeout=true` or an old `LastUpdate` means STALE; missing field means UNKNOWN; an energy string like `"1234 Watt"` goes through the unit-aware parser.
- **Polling budget:** `getdevices` with `lastupdate` plus `favorite=1` keeps the payload small. Response size on a big installation is the main RAM risk. Parse streaming, filter fields, never load the whole document. This is the first job for the fuzz target.
- Strings in the JSON are UTF-8 and may be long; clamp names to the display.

## 5. Open questions (need a live instance)

1. Exact `getdevices` payload for each switch type (capture and sanitise fixtures).
2. WebSocket URL, token auth on the socket, and update message shape.
3. Whether `lastupdate` returns a `ServerTime` field to use as the next cursor.
4. Behaviour of Viewer tokens on `switchlight` (expect 403) and the exact error body.
5. Weather: many users have a virtual or hardware weather sensor, others none. The weather screen should be optional, driven by a chosen device or by the Domoticz *Temp+Hum+Baro* / *Wind* / *Rain* sensors. An external forecast service is out of scope.
6. Minimum Domoticz version. Access tokens are recent (2026.1 era), so older installs only have user and password or trusted networks. Decide whether to support Basic auth as a fallback.

## 6. Naming

**Decision: the product is named dzPanel** (unofficial display firmware for Domoticz on ESP32-S3 boards, round and square). Identifiers and file names use `dzpanel`. The analysis below is kept as the record of the options that were considered; it still calls the old name "RoundOS".

"RoundOS" suggests a general platform; the scope is now *a Domoticz round-screen remote*. Options:

| Name | Pro | Con |
|---|---|---|
| **Halo for Domoticz** (HaloDz) | We already have the Halo visual language | "Halo" is a heavily used word, trademark collisions likely |
| **DomoDial** | Says dial and control, round | Reads like a thermostat only |
| **Orbit** | Short, round, calm | Generic, many existing projects |
| **Domo Ring** | Plain and findable | Bland |
| **RoundOS** (keep) | Already used in docs | Implies a platform and OS we are not building |

Check the Domoticz project's guidance on use of its name before shipping. I have not looked up any trademark policy. "for Domoticz" as a suffix is a common pattern for third-party tools, but that is a convention, not legal advice. If the hardware list stays wider than Domoticz later (Home Assistant at v0.7), a neutral name is safer. This was the user's decision.

## 7. Verified against a live instance (2026.4, build 18568, 2026-10-31)

Read-only calls with a Bearer access token over HTTPS: `getversion`, `getdevices`, `getscenes`, `getplans` all returned `200` and `"status":"OK"`. Sanitised captures live in `tests/fixtures/real_*.json` (names, URLs and location data removed).

| Finding | Consequence |
|---|---|
| `getversion` returns `version` as `"2026.4 (build 18568)"`, plus `dzvents_version`, `python_version`, `hash` | Version gate must parse the leading `YYYY.N` only |
| `getdevices` also carries `ServerTime`, `ActTime` (epoch), `Sunrise`, `Sunset`, `DayLength`, twilight times | Open question 3 answered: use `ActTime` as the `lastupdate` cursor. Clock and sun data come free |
| **`SwitchTypeVal` is set on sensors and meters** (P1 Smart Meter, RFXMeter, General kWh/Visibility) with a different meaning | Trust it only for `Light/Switch`, `Color Switch` and `Lighting *` types. Found by the real capture, not the mock: it made energy meters look like switches |
| Thermostat is `Type: "Setpoint"`, not `"Thermostat"` | Match on `SubType: "SetPoint"` / `SetPoint` field |
| RGB and white lights are `Type: "Color Switch"` with `SwitchTypeVal: 7` | Treated as plain dimmers; colour control is out of scope for now |
| Responses are pretty-printed with tabs; `status` and `title` sit among alphabetically sorted keys | The parser must not assume key order |
| Per-device `forecast_url` (base64) and `StrParam1/2` can hold URLs and home coordinates | Never log or upload raw responses; the parser ignores these fields |

Still open: WebSocket path and auth, Viewer-token behaviour on `switchlight` (not tested; no control calls were made), and which token rights this instance's token has.

### 7.1 Control test (2026-10-31, user-rights token, group "Kantoor lampen")

`switchscene&idx=6&switchcmd=On` then `Off`: both returned `200 {"status":"OK","title":"SwitchScene"}` and the two member lamps followed within 3 s. Confirmed by eye and by re-reading `getdevices&rid=` and `getscenes`.

- The access token with user rights can switch scenes and groups. Open question 4 (Viewer behaviour) is still untested.
- Switching a group On set both member dimmers to **51 %**, and their stored `Level` stayed 51 after Off (it was 1 before). A stored level changes as a side effect, so the UI should show state from `Status`, not `Level`, when a device is Off.
- Read-after-write works: poll `getdevices&rid=` about 1 to 3 s after a command to confirm it, rather than trusting the `OK`.
