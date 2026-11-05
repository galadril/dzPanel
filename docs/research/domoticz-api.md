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

### Control (needs User rights)

| Action | Call |
|---|---|
| On, Off, Toggle | `switchlight&idx=N&switchcmd=On\|Off\|Toggle` |
| Dim level | `switchlight&idx=N&switchcmd=Set Level&level=0..MaxDimLevel` |
| Blinds | `switchlight&idx=N&switchcmd=Open\|Close\|Stop` |
| Push button | `switchlight&idx=N&switchcmd=On` (Push On) or `Off` (Push Off) |
| Selector (fan, mode) | `switchlight&idx=N&switchcmd=Set Level&level=<LevelInt>` |
| Scene | `switchscene&idx=N&switchcmd=On` (groups also accept `Off`) |
| Thermostat setpoint | `setsetpoint&idx=N&setpoint=21.5` |
| Protected device | add `&passcode=...`; the server compares an MD5 of it. The device must not store this. Treat protected devices as view-only |

Source for the switch commands: `WebServerCommands.cpp` (`switchlight`, `switchscene`).

**Safety already in the server:** protected devices and scenes demand a passcode; a Viewer token is refused at the command level. Our own risk levels (`SAFE`, `CONFIRM`, `LOCKED`) sit on top.

### Live updates: WebSocket

`DomoticzWebsocketHandler.cpp` implements a socket on the same server. The client sends JSON events (`request`, `subscribe`, `unsubscribe`, `unsubscribe_devices`) and gets pushed device and scene updates for the devices it subscribed to. This replaces polling and is the better path for the foreground screen. It is **unverified**: the URL path, auth on the socket, and the message shapes need a live capture. Plan: polling with `lastupdate` first (v0.4), WebSocket as an upgrade.

### MCP endpoint (not for the device)

Domoticz 2026.1 also serves an MCP endpoint at `/mcp`: JSON-RPC over streamable HTTP plus SSE, same Bearer tokens. It is designed for AI clients. It would cost the ESP32 more RAM and parsing than `json.htm`, so we skip it. It is still useful to us: it lists devices and tools, so it can describe the API on a live server.

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
