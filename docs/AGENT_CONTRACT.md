# Parallel build contract

Every agent working on dzPanel reads this first. It exists so several agents can work at once without stepping on each other.

## Ground rules

1. **Own your directories. Touch nothing else.** If you need a change elsewhere, write it under "Requests" at the bottom of your report and carry on with a stub.
2. **Generic for all Domoticz users.** No device names, idx numbers, hostnames, languages or house layouts in code, tests or defaults. Screens are chosen from `dz_cap_t` capabilities, never from names. The only per-user inputs are the base URL and the access token.
3. **Honest data.** Unknown never renders as 0. Stale values are visibly stale (`ru_val_t`). Protected devices are never controlled. Commands are confirmed by re-reading the device (read-after-write), not by trusting the `OK`.
4. **Secrets.** The token comes from the environment (`DOMOTICZ_ACCESSTOKEN`) on host, from NVS on the device. Never print it, log it, write it to a file, put it in a URL, or commit it. Redact `Authorization` in any log. Never upload raw Domoticz responses: they contain home coordinates and internal URLs (see `docs/research/domoticz-api.md` section 7).
5. **Live server rules** (`https://domoticz.hnogames.nl`, belongs to the user):
   - Reading is fine. Be gentle: no polling faster than every 5 s, no loops of calls.
   - The token has *user* rights. **The only thing you may control is group idx `6` ("Kantoor lampen") via `switchscene`, and only when you must prove a control path end to end.** Leave it Off afterwards and verify. Everything else uses the mock (`sim/mock/domoticz_mock.js`).
   - HTTPS only reaches the server through the sandbox proxy. Do not work around the proxy. `libcurl` dev headers are not installed; for live calls on host use the `curl` CLI via a small dev transport.
6. **Build hygiene.** C11, `-Wall -Wextra -Wpedantic -Werror`, must pass under ASAN+UBSAN (`cmake -B build && cmake --build build && ctest`). Existing tests must keep passing. New code ships with tests. Fixtures are in `tests/fixtures/` (`real_*.json` are sanitised captures; `mock_*.json` are hand-written).
7. **Don't commit, don't `git init`.** The workspace is not a repo and the user has not asked for commits.
8. **Honest reports.** End with: what works (with the command you ran), what you did not finish, what is unverified, and Requests for other owners. If a test fails, say so.

## Existing code you can rely on

- `components/ru_data` (`ru_val_t`, strict parse, format), `components/ru_core` (pthread OSAL: queue, mutex, task, timer), `components/ru_ui/geom.c` (round geometry), `components/int_domoticz/dz_parse.[ch]` (parser and capability classifier; `dz_device_t`).
- `docs/mockups/*.png` are the visual targets at 466 and 480 px. `DESIGN.md` section 8 is the Halo visual language. `docs/research/ux.md` has the interaction research.
- LVGL 9.2.2 source is in `vendor/lvgl/src` (shallow clone). SDL2 2.30 dev headers are installed.

## Ownership

| Agent | Owns | Builds |
|---|---|---|
| **UI** | `sim/` (except `sim/mock/`), `components/ru_ui/` (except `geom.c`, `ru_geom.h`), `tools/fonts`, `tools/icons`, top-level `CMakeLists.txt` *sim target only* | LVGL+SDL2 simulator with round mask, Halo theme tokens, widgets (`value_tile`, ring gauge, status rim, dimmer arc, selector wheel, hold-to-confirm), screenshot export at 466 and 480 |
| **Client** | `components/int_domoticz/` (except `dz_parse.*`), `components/ru_data/` additions, `sim/mock/` | HTTP transport interface, Domoticz client (poll with `ActTime` cursor, backoff, token-rejected state), device model store with staleness, command layer with read-after-write and risk levels, mock upgrades (selector, color switch, scene, latency, errors, 401/403) |
| **Shell** | `components/ru_core/` additions (bus), `components/ru_runtime/`, `components/ru_shell/` | Event bus with coalescing and drop counters, app registry and lifecycle, low-memory levels, gesture recogniser (annulus and sector rules) with synthetic traces, navigation model |

A second wave builds the **apps** (Home, Devices, Scenes, Energy, Weather, Settings) once UI and Shell land, against the interfaces they expose.

## Interfaces between owners (agree by header, not by chat)

- Client exposes `components/int_domoticz/include/dz_client.h` and `dz_model.h`. The model is a store of `dz_device_t` plus per-device `ru_val_t` freshness, a change counter, and a `dz_cmd_*` API returning a request id; results arrive as events.
- Shell exposes `ru_bus.h` (post/subscribe, coalescing key), `ru_app.h` (app descriptor: id, name, icon, create/destroy/on_event, memory class), `ru_gesture.h`.
- UI exposes `ru_theme.h`, `ru_widgets.h`. Widgets take plain values (`ru_val_t`, level, label), never `dz_device_t`, so the UI has no dependency on Domoticz.
- Until the other side's header exists, code against your own minimal stub and note it under Requests.

---

# Wave 2: firmware (read this too if you work on firmware, apps or networking)

Wave 1 (UI toolkit, Domoticz client, shell) is done and verified: 13 ctest targets pass (`cmake -S . -B /tmp/vb -DRU_BUILD_SIM=OFF && cmake --build /tmp/vb -j8 && cd /tmp/vb && ctest`). The user has now explicitly asked for firmware and allowed parallel agents.

## Facts established (verified by running, 2026-11-01)

- **ESP-IDF v5.5.6** and the Xtensa ESP32-S3 toolchain are installed read-only under `/opt/esp`. The component manager wants to write inside `IDF_TOOLS_PATH`, so use a writable shadow that symlinks the real tools:
  ```bash
  mkdir -p /home/dev/.idf-shadow && for e in /opt/esp/tools/*; do ln -sfn "$e" /home/dev/.idf-shadow/$(basename $e); done   # once
  export IDF_TOOLS_PATH=/home/dev/.idf-shadow; . /opt/esp/idf/export.sh
  ```
  (Do not try to change `/opt/esp`; it is read-only by design.)
- **Managed components download fine** (Waveshare BSP, LVGL, drivers). The unmodified vendor demo `vendor/ESP32-S3-Touch-AMOLED-1.75/examples/esp-idf/02_lvgl_demo_v9` builds (copy it to `/tmp` first; never build inside `vendor/`): 1.0 MB, 12% of an 8 MB slot.
- Resolved: `waveshare/esp32_s3_touch_amoled_1_75` 3.0.1, **`lvgl/lvgl` 9.4.0**, `espressif/esp_lcd_co5300` 2.2.0 (display), `waveshare/esp_lcd_touch_cst9217` 2.0.0 (touch), `espressif/esp_io_expander_tca9554` (IO expander), `espressif/esp_lvgl_adapter` 0.6.4. Flash 16 MB QIO, octal PSRAM 80 MHz (from the vendor `sdkconfig.defaults`).
- The host simulator is pinned to **the same LVGL 9.4.0** (`vendor.lock`). Code must compile on both.
- `esptool` syntax for 5.5.6, from the build output: `python -m esptool --chip esp32s3 -b 460800 write_flash --flash_mode dio --flash_size ... --flash_freq 80m 0x0 ...`.

## Rules for building

- **No board is attached.** "It builds" is the strongest claim available for anything that touches hardware. Never write "works on the device". Say "compiles, not run".
- Builds are slow. Run `idf.py build` and LVGL builds **in the background** with output to a log (`run_in_background`), then wait for completion; never a foreground command that may exceed 2 minutes.
- **Only the fw-core agent runs `idf.py` inside `firmware/`** (one project directory means one `managed_components` and one `sdkconfig`). Everyone else develops host-testable code with ctest, and compile-checks IDF-only glue by asking fw-core to include it, or in a scratch copy under `/tmp` that you own.
- Keep progress notes in `docs/progress/<your-agent>.md` (what is done, what is next, exact commands). The session can end at any time and kills background agents; the next one resumes from your notes and the files on disk. Save early and often.
- Keep `RU_BUILD_SIM=OFF` ctest green. Add new host tests to the top-level `CMakeLists.txt` inside your own clearly delimited block, re-reading the file right before editing.

## Domoticz semantics you must respect (confirmed in `vendor/domoticz`)

- **Scene** (`Type: "Scene"`, `DZ_CAP_SCENE`): **push only**. Only an On command; it has no Off. Its status stays On after a push.
- **Group** (`Type: "Group"`, `DZ_CAP_GROUP`): **toggles On and Off**. State is **On, Off or Mixed** (members differ): use `dz_device_state()`. A tap sends `dz_cmd_scene_tap_kind()`: On->Off, Off or Mixed->On. A group that answers Mixed after On is UNCONFIRMED, not confirmed.
- Both are in their own idx namespace (`getscenes`), separate from devices. Both need a hold-to-confirm (`dz_cmd_risk`).

## Ownership, wave 2

| Agent | Owns | Delivers |
|---|---|---|
| **fw-core** | `firmware/` (the IDF project), `components/*/CMakeLists.txt` (IDF registration only), `components/ru_core/ru_osal_freertos.c`, `components/ru_hal/` | Project skeleton, partition table (two OTA slots, NVS, otadata, coredump, storage), `sdkconfig.defaults`, board bring-up on the 1.75" (display, touch, LVGL via the BSP), FreeRTOS OSAL port, `app_main` showing a Halo screen from `ru_ui`, `idf.py build` passing, size report, `idf.py merge-bin` output fed through `tools/make_manifest.py` |
| **fw-net** | `components/ru_net/`, `components/ru_cfg/`, `components/int_domoticz/dz_transport_esp.c`, `components/int_domoticz/dz_service.[ch]` | Key-value config store (NVS impl behind an interface, memory impl for tests), `RU+CFG` and Improv Serial protocol handlers as pure host-tested C, Wi-Fi manager state machine, `esp_http_client` transport with CA bundle and the `insecure_lan` rule, the Domoticz service (one network task driving `dz_client_step` and `dz_cmd_pump`, results posted on the bus) |
| **apps** | `components/ru_apps/`, `sim/src/` and `components/ru_ui/` (it now owns them) | Apps on the simulator against the mock: Home (favourites), Devices (detail screens per capability), Scenes and Groups (push vs toggle, **Mixed** state), Energy, Weather and Temperature, Settings; bump of the simulator code to LVGL 9.4.0; golden screenshots at 466 and 480 |

Interfaces: apps talk to Domoticz only through `dz_model.h` and `dz_cmd.h` (plus `dz_parse.h` types), to the shell through `ru_app.h`, `ru_bus.h`, `ru_nav.h`, `ru_gesture.h`. Widgets take plain values.
