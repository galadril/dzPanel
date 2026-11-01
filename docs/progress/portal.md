# portal agent: SoftAP setup portal

Status: host code and tests done and green; IDF glue compile-checked in a scratch project. Nothing run on a device.

## Files (all mine)
- `components/ru_portal/include/ru_http.h`, `ru_http.c`: strict request parser (GET/POST, limits, total deadline) + form decoding.
- `components/ru_portal/include/ru_captive.h`, `ru_captive.c`: DNS answer, probe-path classify, portal URL, Wi-Fi QR payload, password/SSID/CSRF generators, constant-time compare.
- `components/ru_portal/include/ru_portal.h`, `ru_portal.c`: the flow as a pure state machine, pages, policy.
- `components/ru_portal/include/ru_portal_esp.h`, `ru_portal_esp.c`, `CMakeLists.txt`: IDF glue (esp_http_server, UDP DNS task, AP+STA, supervisor task).
- `tests/test_portal.c` and the delimited block `BEGIN portal agent` at the end of the top-level `CMakeLists.txt`.
- Docs touched: `docs/provisioning.md` (SoftAP section), `docs/privacy.md`.

## Commands
- Host: `cmake -S . -B /tmp/vbp -DRU_BUILD_SIM=OFF && cmake --build /tmp/vbp -j8 && cd /tmp/vbp && ctest` (target `portal`).
- IDF compile check (scratch, do NOT use firmware/): `/tmp/portal_chk` has symlinks to `components/{ru_core,ru_cfg,ru_net,ru_portal,ru_data,int_domoticz}`.
  `export IDF_TOOLS_PATH=/home/dev/.idf-shadow; . /opt/esp/idf/export.sh; cd /tmp/portal_chk && idf.py build` (run in the background, about 5 minutes cold).

## Decisions
- esp_http_server does its own request parsing (limits come from `CONFIG_HTTPD_MAX_REQ_HDR_LEN` 512 and `CONFIG_HTTPD_MAX_URI_LEN` 512 in sdkconfig, stricter than `ru_http`). The glue enforces the 2 KiB body cap, form content type and a 5 s total body deadline itself and passes the parsed request to the pure layer. `ru_http` is the host-tested reference parser for a raw-socket server and for fuzzing; the portal logic does not depend on which one fronts it.
- Domoticz validation, test and save go through `ru_cfgline_handle` (the USB path) by building the same JSON in a stack buffer that is wiped afterwards. The token never reaches any page builder: pages have no access to it.
- Wi-Fi scan runs BEFORE the AP is up (a scan in AP+STA hops channels under the phone). The result is cached for the session.
- The Wi-Fi join uses `ru_wifi_esp_try_new` (existing: saves credentials only after an IP, restores on failure) and polls `ru_wifi_esp_state`. The glue releases the portal lock while waiting.
- AP `max_connection` is 2 so a phone reconnecting under a new MAC is not locked out; the pure layer still allows one client session (others see a busy page).

## Not done / open
- No screen. See the request below.
- Nothing wires the Wi-Fi manager's `on_ap_requested` or the long-press gesture to `ru_portal_esp_start` yet (fw-core / apps).
- After a failed join `ru_wifi_esp` keeps retrying the pending credentials in the background; a 'cancel pending' call in `ru_wifi_esp` would be cleaner (fw-net).
- Unverified on hardware: AP channel behaviour when STA joins a router on another channel, DHCP DNS option, iOS and Android captive sheets, esp_fill_random quality (true RNG needs the RF subsystem on, which it is once Wi-Fi started), heap and stack headroom.
- Not offered in the portal: the RU+CFG `force` (save when unreachable). Add a checkbox if wanted.

## Request to the apps owner: 'Set up this device' screen with a QR
Use `ru_portal_esp.h` (glue, device) or `ru_portal.h` (pure types):

```c
ru_portal_status_t st;                       /* ru_portal.h */
if (ru_portal_esp_status(&st)) {             /* false when not running; never blocks on a slow join */
  st.ssid;            /* "RoundOS-XXXX" */
  st.password;        /* 12 chars, SECRET: draw it, never log it */
  st.url;             /* "http://192.168.4.1/" */
  st.remaining_min;   /* minutes until the idle timeout, rounded up (st.remaining_ms for a ring) */
  st.step;            /* RU_PORTAL_STEP_WIFI / _DOMOTICZ / _DONE: for a hint line ("Step 1 of 2") */
  st.locked;          /* too many failed attempts: show "Restart setup" */
}
char qr[128]; ru_portal_esp_qr_wifi(qr, sizeof qr);   /* "WIFI:T:WPA;S:..;P:..;;" for the first QR */
char url[40]; ru_portal_esp_qr_url(url, sizeof url);  /* second QR or text, show it as text too */
```
Start/stop: `ru_portal_esp_start(RU_PORTAL_SRC_USER)`, `ru_portal_esp_stop()`; `on_stopped` in `ru_portal_esp_cfg_t` says why it ended (idle, success, user). Suggested screen: QR 1 "Join this Wi-Fi", then QR 2 or the URL text once joined, the password in text beside the QR (phones without a QR scanner), and the minutes left. Redraw about once a second.

LVGL 9.4 has `lv_qrcode` (`src/libs/qrcode/lv_qrcode.h`: `lv_qrcode_create`, `lv_qrcode_set_size`, `lv_qrcode_set_dark_color`, `lv_qrcode_set_light_color`, `lv_qrcode_update(obj, data, len)`), but it is disabled:
- `firmware/sdkconfig` has `# CONFIG_LV_USE_QRCODE is not set`. **Add `CONFIG_LV_USE_QRCODE=y` to `firmware/sdkconfig.defaults`** (fw-core), delete the stale line or regenerate sdkconfig.
- The simulator's `sim/lv_conf.h` has no `LV_USE_QRCODE`: set it to 1 there (apps/UI owner) so the same code compiles on the host.
- Draw the QR with a white background and black modules (dark mode inverts are not scannable on many phones); keep a 4 module quiet zone; about 160 px for a `WIFI:` payload of roughly 55 characters (QR version 4 or so).
