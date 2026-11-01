# fw-core progress
Env: `mkdir -p /home/dev/.idf-shadow && for e in /opt/esp/tools/*; do ln -sfn "$e" /home/dev/.idf-shadow/$(basename $e); done; export IDF_TOOLS_PATH=/home/dev/.idf-shadow; . /opt/esp/idf/export.sh`
- [x] 1 skeleton firmware/ (CMakeLists, main/idf_component.yml, sdkconfig.defaults, partitions.csv)
- [x] 2 components/*/CMakeLists.txt written (not yet built)
- [x] 3 ru_osal_freertos.c written (not yet built)
- [ ] 4 ru_hal, 5 app_main, 6 build, 7 merge-bin+manifest
- NOTE: first `idf.py set-target esp32s3` downloads ~20 managed components onto a slow FS (10+ min). Run in background, never kill; then `ls firmware/managed_components/waveshare__*`.
- ru_hal/include/ru_hal.h written (API + board profile). Next: ru_hal.c over BSP (bsp_display_start_with_config, wrap indev read_cb for touch sink), app_main.c.
- ru_hal.c/.h, ru_board_amoled_175.c, app_main.c written (uncompiled). .gitignore extended. /workspace is a slow Windows mount: managed_components download took >20 min. Next: wait for set-target to finish, then idf.py build in background to /tmp log.
- set-target done (configure took 1155 s on /workspace mount). Building with -B /tmp/fw/build (fast overlay FS) log /tmp/fw/build.log
- idf.py -B /tmp/fw/build build PASSED first try (10m17s full, 0xbf570 B app = 19% of 4MB slot). Remaining: warnings review, disable LV examples/demos, merge-bin, manifest, docs.
- first build+merge-bin+manifest OK. Re-running clean build with LV examples/demos off (log /tmp/fw/build2.log). set-target must be given on a fresh sdkconfig: idf.py -B /tmp/fw/build set-target esp32s3 (or IDF_TARGET=esp32s3 env)
- DONE 1-7. Final clean build: 7m39s (examples/demos off), see report. Commands:
  `cd firmware; export IDF_TOOLS_PATH=/home/dev/.idf-shadow; . /opt/esp/idf/export.sh; IDF_TARGET=esp32s3 idf.py -B /tmp/fw/build build` (build dir in /tmp: /workspace is a slow Windows mount, config there took 19 min)
  `idf.py -B /tmp/fw/build merge-bin` -> /tmp/fw/build/merged-binary.bin ; `python3 -I tools/make_manifest.py --board amoled-175 --channel nightly --version 0.0.1-fwcore.1 --merged /tmp/fw/build/merged-binary.bin --out /tmp/fw/site-out --date 2026-11-01`; `node /tmp/fw/check_versions.mjs /tmp/fw/site-out/versions.json` (uses site/lib.js validateVersions, pickBuild)

# Integration task (wave 2b)
## DECISION: one network owner = dz_service
- Device: dz_service owns dz_client + dz_model + dz_cmd. ru_dzapp gets an optional BACKEND (ru_dzapp_set_backend): submit() -> dz_service_submit, conn() -> dz_service_conn_state; outcomes come back via ru_dzapp_post_cmd_event (thread-safe queue into the app's existing res_q, drained by ru_dzapp_ui_tick so the fb state machine is untouched). ru_dzapp_net_* NOT used on the device. Host sim/tests keep ru_dzapp's own net step (backend == NULL = old behaviour).
- DZ_EV_CMD_RESULT has no idx/after: UNCONFIRMED reads the device from the model by fb.idx so "STILL MIXED" is kept.
- Edits: components/ru_apps/ru_dzapp.c + include/ru_dzapp.h (backend), sim/tests/test_apps.c (new backend_path test, nothing weakened).
- Step 0 DONE: backend added, host ctest 19/19 (RU_BUILD_SIM=OFF) and sim 22/22 (incl. new backend_path test in sim/tests/test_apps.c). NEXT: firmware/main app_main + svc_glue + ui_shell + boot_health.
- wrote main/{boot_health,svc_glue,ui_shell,app_main}.c + ru_apps/CMakeLists; building (log /tmp/fw/build3.log)
- Integration build DONE: idf.py -B /tmp/fw/build build passed (incremental 2m35s). roundos.bin 0x18c4a0 B (1,623,200) = 39% of 4 MB slot; DIRAM 247,387 B (72%) static, .bss 29 KB; flash code 975 KB, rodata 430 KB. 0 warnings. Merged via esptool merge_bin (same args as flasher_args) -> /tmp/fw/merged-int.bin (1,754,272 B), make_manifest + validateVersions OK.
- NOTE: after the build, `idf.py merge-bin` re-triggered a build that FAILED in components/ru_apps/ru_dzview.c (DZ_CAP_SENSOR/DZ_CAP_MODE not handled in switch, -Werror=switch): the climate agent's new enums. Not ours; they must update ru_dzview.c switch.
- first full-stack build OK (1.62 MB, 39% slot) but DIRAM 72% static: dropped LV IRAM attr + wifi IRAM opts, clean rebuild log /tmp/fw/build4.log

## Integration design decisions (review list)
1. ONE owner = dz_service (see DECISION above). ru_dzapp backend hooks; sim keeps its own net step (backend NULL).
2. Content gestures: NOT routed through ru_gesture to apps. ru_dzapp already handles taps/swipes via LVGL's own indev (tested in the sim); routing the same touches again would double-fire. ru_gesture runs on every touch sample (LVGL task) and only RIM events act: edge-swipe BACK / rim long-press and bottom edge = home. QUICK/CONTEXT/launcher: ignored (not built). ru_nav state machine is NOT used (only ru_nav_input_from_gesture for the mapping); ru_runtime is a registry (apps registered, go_home), section swipes happen inside ru_dzapp, so the runtime's "foreground app" is not kept in step with the section shown.
3. Refresh: ru_dzapp's own 250 ms LVGL timer + a bridge task that waits on the bus (topic 8, coalesced), forwards DZ_EV_CMD_RESULT to ru_dzapp_post_cmd_event, then takes the LVGL lock and runs ru_dzapp_ui_tick (min 100 ms apart).
4. First boot: not provisioned (no Domoticz config OR no Wi-Fi creds) -> "NOT SET UP" screen, text only (no URL: the installer URL is not fixed yet). Switches to the apps live when RU+CFG/Improv completes (control timer).
5. Boot health: NVS "boot/fails" incremented at every boot start, cleared after 30 s healthy (heartbeat from LVGL timer must move every 5 s, display init ok) together with esp_ota_mark_app_valid_cancel_rollback. UI stall -> esp_restart. >= 3 previous unhealthy boots -> safe mode: HAL + safe screen + Wi-Fi + console only, no service, no apps, image NOT marked valid, counter kept. Safe screen has a 3 s hold-to-erase (config + Wi-Fi + counter, restarts). BOOT-button gestures from DESIGN (5 s / 15 s) NOT implemented (GPIO0 low at reset means ROM download mode anyway).
6. Improv: improv_authorized=true (no physical authorisation); Improv accepted only while not provisioned. Wi-Fi AP/captive portal: not implemented (callback just logs).
7. IRAM: dropped CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM and Wi-Fi IRAM opts after seeing DIRAM 72% static. Costs speed; unmeasured.
8. Service memory: 128 KB response buffer, 512-entry model and stage in PSRAM; no PSRAM -> favourites-only 16 KB internal. allow_control is always true (no viewer detection; dz_cmd/403 handle viewers).
9. app_main order: NVS -> boot counter -> HAL -> bus -> service alloc -> UI -> Wi-Fi/console/service start -> health watcher.
- DONE integration: idf build OK 0x191250 B (39% of ota_0), DIRAM static 43% (146 KB, 195 KB left), clean build 8m52s. merge-bin 1,774,160 B + make_manifest + validateVersions OK. Host ctest 21/21 (RU_BUILD_SIM=OFF, tree incl. another agent's ru_climate); sim 22/22 earlier. Other agent's ru_climate.c has 3 format-truncation warnings (ru_apps CMake downgrades to warning for IDF).
