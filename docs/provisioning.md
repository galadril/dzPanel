# Provisioning: getting Wi-Fi and Domoticz onto the device

Status: **specification, installer-side code, and a host-tested firmware implementation.** The installer page (`site/`) and the device-side handlers (`components/ru_net`: `ru_cfgline`, `ru_improv`, `ru_console`; `components/ru_cfg`) have tests on the host. The SoftAP captive portal (`components/ru_portal`) is host-tested with its IDF glue compile-checked. **Nothing here has run on a device**: the USB console glue, the Wi-Fi manager and the portal glue are compile-checked only.

## The three things a device needs

| Need | How it gets there | Who handles it |
|---|---|---|
| Firmware | Web flasher (ESP Web Tools) | `site/`, `tools/make_manifest.py` |
| Wi-Fi | **`RU+CFG` with a `wifi` block** from the installer page (no Improv dialog needed), or **Improv Serial** over USB in the install dialog right after flashing | `site/app.js`, firmware `RU+CFG` handler; ESP Web Tools (verified: `improv-wifi-serial-sdk` 2.8.1) |
| Domoticz address and token | **Configure step** on the installer page, over USB | `site/app.js`, firmware `RU+CFG` handler (host-tested, glue compile-checked) |

Fallback paths, for people without Chrome or Edge:

1. **SoftAP captive portal** with a QR code on the round screen: Wi-Fi first, then the Domoticz details on the same page. Random password, shown on screen, times out. Status: **host-tested, glue compile-checked** (section "SoftAP portal" below). The on-screen 'Set up this device' page with the QR is not built yet (request to the apps owner in `docs/progress/portal.md`).
2. **Hold-to-reset gesture** (long press on the rim, `ru_gesture` already emits it) starts the portal again. Not wired yet: the glue exposes `ru_portal_esp_start(RU_PORTAL_SRC_USER)`.

The Domoticz details are **never typed on the round screen** and never stored by the installer.

## SoftAP portal (`components/ru_portal`)

For browsers without Web Serial (iPhone, Firefox, Safari). Pure C in `ru_http`, `ru_captive`, `ru_portal` (host-tested under ASAN/UBSAN, `ctest -R portal`); `ru_portal_esp.c` is the IDF glue (compile-checked, not run).

**When it runs.** Never by itself. Only when asked: the Wi-Fi manager reached `AP_REQUESTED` (no credentials or repeated join failures, via `ru_portal_start_from_wifi`), or an explicit start (long-press gesture, Settings). It stops after 10 minutes without a page request (parameter), 15 s after success, or on request. A failed start rolls everything back.

**Network.** The device brings up a WPA2 AP named `RoundOS-XXXX` with a random 12 character password (unambiguous alphabet, about 60 bits) generated per session and shown on the round screen as a Wi-Fi QR (`WIFI:T:WPA;S:...;P:...;;`) plus the portal URL. The password is never logged and never in a page. DHCP hands out the AP as the DNS server and a small UDP responder answers every A query with the AP address, so phones raise their captive-portal sheet; the probe paths (`/generate_204`, `/hotspot-detect.html`, `/ncsi.txt`, `/connecttest.txt`, `/redirect`, anything else) get a 302 to `http://192.168.4.1/`.

**Flow** (server-side state, plain HTML, no JavaScript, no external resources, strict CSP):

1. Choose the Wi-Fi network from a scan taken before the AP came up (or type a hidden name) and enter its password. The device joins it (AP+STA).
2. Domoticz address, access token (password field, never echoed), and the 'I understand' box that is required for `http://`. A visible warning explains the plain-http risk.
3. The device tests the connection and saves. Validation, test and save are the **same code as the USB path** (`ru_cfgline_handle` with the `dz_service_cfgline_test` callback), so both paths accept and reject exactly the same input and store exactly the same thing.
4. Done page; the portal switches off.

**Hardening.** Per-session CSRF token on every POST (constant-time compare); one client at a time (others get a busy page, the slot frees after 2 minutes of silence); at most 5 failed posts, then locked until the portal is restarted; body capped at 2 KiB, one request per connection, 5 s total deadline for a request body; every dynamic value HTML-escaped; responses carry `Cache-Control: no-store`, a CSP that forbids scripts and external loads, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`.

**Honest limits.** The portal page is plain http. It is protected only by the WPA2 password of the private AP, so anyone who has that password (the person holding the device) could read the token in transit. Do not use it where strangers can see the screen. If the home router is on another channel than the AP, joining it moves the AP's channel too (ESP32 hardware limit) and the phone may briefly drop; reload the page. Neither has been exercised on a device.

## Why USB for the token

A browser on an `https://` installer page cannot call a plain-`http` Domoticz on the LAN (mixed content blocked, CORS closed), so **the page cannot test the connection itself**. Sending the settings over the USB cable avoids a LAN web page that would carry the token over plain http, and lets the **device** do the real connection test and report back.

## Configuration schema (version 1)

`config.example.json` is the safe template. A real file is `config.local.json` (git-ignored).

```json
{ "v": 1, "domoticz": { "url": "http://192.168.1.5:8080", "token": "…", "insecure_lan": true } }
```

| Field | Rule |
|---|---|
| `v` | `1`. A device that sees a higher number must refuse and say "update the firmware". |
| `domoticz.url` | `http` or `https`, host required, **no user info, no query, no fragment**, trailing slash removed. A sub-path is kept for reverse proxies. |
| `domoticz.token` | A Domoticz access token (signed JWT). Anything else is accepted with a warning; the server decides. |
| `domoticz.insecure_lan` | `true` only when the URL is plain `http`. The installer demands an explicit tick, and the device shows a warning icon while it is set. |

Token rights are read **locally, in the browser** from the `roles` claim (`0` Viewer, `1` User, `2` Admin; confirmed in `Cmd_CreateAccessToken`). The page warns on Admin and on an expired token. The signature is not and cannot be checked client-side.

## USB line protocol: `RU+CFG`

Over the USB-Serial/JTAG console, 115200 baud, one JSON object per line.

```
host   -> RU+CFG {"v":1,"domoticz":{"url":"…","token":"…","insecure_lan":false}}\n
host   -> RU+CFG {"v":1,"wifi":{"ssid":"…","password":"…"},"domoticz":{…}[,"force":true]}\n   (Wi-Fi block is optional)
device -> RU+CFG OK <info>\n
device -> RU+CFG ERR <code> [detail]\n
```

| Code | Meaning |
|---|---|
| `bad_json`, `bad_version`, `bad_url` | Rejected before any network use. `bad_version` carries the detail `update_firmware_or_installer` |
| `too_long` | Line over 2048 bytes (the detail is the limit) |
| `unreachable` | No route, DNS failure or timeout. With the detail `not_domoticz` the address answered, but not like a Domoticz server |
| `tls` | Certificate or handshake failure |
| `token_rejected` | HTTP 401 or 403 |
| `no_wifi` | Wi-Fi not set up yet (send the `wifi` block, or use Improv) |
| `bad_wifi` | The `wifi` block is invalid (missing or empty `ssid`, missing `password`, rules below). Rejected before any network use |
| `wifi_failed` | The device could not join the network within about 20 s; nothing was changed and the previous Wi-Fi (if any) is back. Detail `no_ap` (network not found), `auth` (rejected, usually a wrong password) or `timeout` (no address in time) or `busy` (another join, e.g. from the setup page, is still running; nothing was started) when the driver reports it; no detail otherwise. The Domoticz part was not attempted |
| `store` | The settings were valid but could not be written to storage |

### Optional `wifi` block (version 1, backward compatible)

`v` stays `1`: a line without `wifi` behaves exactly as before. At least one of `wifi` and `domoticz` must be present (neither is `bad_json`); `wifi` alone and `domoticz` alone both work. Both ends validate with the same rules (`ru_wifi_ssid_check` / `ru_wifi_pass_check` in `components/ru_cfg`, `validateWifi` in `site/lib.js`, the same vectors in `tests/test_cfg.c`, `tests/test_net.c`, `tests/site/test_lib.mjs`, and a 20 000 line JS-versus-C differential run):

| Field | Rule |
|---|---|
| `wifi.ssid` | 1 to 32 **bytes** of well-formed UTF-8 (so fewer characters if they are not ASCII), no control characters (U+0000 to U+001F, U+007F to U+009F). Never trimmed |
| `wifi.password` | Required (explicit). Empty = open network; otherwise 8 to 63 printable ASCII characters (WPA2 passphrase) or exactly 64 hex digits (raw PSK). No control characters, no non-ASCII. The page only sends an empty password after an "open network" tick |

Order on the device: **Wi-Fi first**, with the same logic Improv uses (try the new credentials, save them only once an IP address is obtained, otherwise go back to the previous ones; blocks up to 20 s), **then**, if present, the existing `getversion` test and save for Domoticz (up to 15 s). The page waits 45 s.

Replies with a Wi-Fi block:

| Reply | When |
|---|---|
| `RU+CFG OK wifi_saved` | `wifi` alone, joined and saved |
| `RU+CFG OK wifi_saved saved [info]` / `RU+CFG OK wifi_saved saved untested` | both, everything worked (or `force` saved an unreachable server) |
| `RU+CFG ERR wifi_failed [no_ap\|auth\|timeout\|busy]` | the join failed; Domoticz not attempted, nothing saved |
| `RU+CFG ERR <domoticz code> [detail] wifi_saved` | Wi-Fi worked and **stays saved** (it worked), the Domoticz step failed with that existing code; the trailing `wifi_saved` says so |
| `RU+CFG ERR store` | the join worked but the credentials could not be stored (Wi-Fi not kept) |

`force` still applies only to Domoticz `unreachable`; it never skips a failed join. A line is validated completely before anything is changed: a good `wifi` with a bad Domoticz URL is refused without touching the Wi-Fi.

**Line size.** The 2048 byte cap (including `RU+CFG `) is kept. The worst legal line (a 32 byte SSID of quote characters and a 63 character password of backslashes, both JSON-escaped, a 191 character address, a 767 character token and `force`) is about 1250 bytes, so a 63 character password plus a long JWT fits with room to spare (checked in all three test suites).

**Privacy.** The Wi-Fi password is handled like the token: it exists only in the USB line buffer (zeroed after use), in a RAM copy while joining (wiped), and in the `secrets` NVS namespace once the join succeeded. It is never echoed, never in a reply, never logged (the log says only that Wi-Fi was saved or failed), and shown masked (`ru_wifi_pass_masked`: `********` or `(none)`, no length) wherever a UI needs a placeholder. The page keeps no copy, clears both password fields after a successful send, and `redact()` hides it.

A success reply is `RU+CFG OK saved`, or `RU+CFG OK saved untested` when the line carried `"force":true` and the connection test failed with `unreachable`. **`force` applies only to `unreachable`**: a rejected token, a TLS failure or invalid settings are never saved. The page shows the two success replies differently, so a forced save is not reported as "connected".

Device-side requirements (to implement in firmware):

- Accept the line only on the **console**, never on a network port.
- **Wi-Fi before Domoticz** when both are in the line (see above); a Wi-Fi failure ends the line with `wifi_failed`.
- **Test before saving**: call `getversion` with the token. Save to the secrets NVS namespace only on success, unless the failure is `unreachable` and the line carries `"force":true`.
- **Never echo the line or the token** to the console, logs or crash dumps. Reply codes carry no secret. Ordinary log lines must not start with `RU+CFG `, so the host can ignore them.
- Cap the line length (the token is a few hundred bytes; cap at 2 KiB) and the JSON depth.
- A 15 s timeout for the connection test (plus up to 20 s for a Wi-Fi join); a reply always follows, exactly once.

The page ignores any console line that does not parse as a reply, and shows a fixed human message per code. It never shows the raw reply text or the config. `redact()` in `site/lib.js` exists for any future debug view.

## Domoticz-side guidance shown to users

Create the token under *Setup, More Options, Access Tokens*. Prefer **Viewer** (a display that cannot switch anything) or **User** if controls are wanted. Never Admin. Domoticz shows the token only once and stores a hash, and deleting it in the UI cuts the device off immediately.

## Open questions

1. Whether to also support Domoticz installs older than the access-token feature (user and password, or trusted networks). See `docs/research/domoticz-api.md` section 5.
2. ~~QR payload format for the captive portal~~ Resolved: the standard `WIFI:T:WPA;S:...;P:...;;` string (escaped, hex-looking values quoted) plus the portal URL as text or a second QR.
3. Whether Improv's post-provisioning "redirect URL" should point at the device's LAN page, or be left empty so only the USB path exists. Leaning empty: fewer ways to leak a token.
