# apps agent progress (RoundOS wave 2)

## Status log
- [start] build dir /tmp/appsbuild (cmake -S . -B /tmp/appsbuild -DRU_BUILD_SIM=ON; log /tmp/appsbuild.log, run in background).
- Existing before me: components/ru_apps/ru_dzapp.c (monolithic: home list + detail per cap; scenes WRONG: scene had ON/OFF buttons), sim/tests/test_dzapp.c (mock e2e).

## Plan
1. LVGL 9.4.0 compile fixes (sim + ru_ui widgets)
2. ru_fb (command feedback, pure C) + ru_dzview (formatting, pure C) -> ctest host tests
3. ru_ui: ru_state_tile (Off/On/Mixed/Pushed indicator)
4. ru_dzapp sections: Home, Devices, Scenes(+Groups), Energy, Weather, Settings; ru_app_t descriptors
5. sim driver on fixtures + screenshots 466/480; tests

## Log
- LVGL 9.4.0: existing sim + widgets compiled and sim_ui passed unchanged (no API fixes needed). /tmp/appsbuild is the build dir (lvgl_core compile ~10 min first time; incremental is fast).
- Written: components/ru_apps/{include/ru_fb.h,ru_fb.c (command feedback), include/ru_dzview.h, ru_dzview.c (pure formatting; Scene vs Group tile state; energy W/kW; '--')}.
- ru_ui: added ru_state_dot / ru_state_tile (OFF hollow, ON filled, MIXED half filled, PUSH core, NONE=value) in ru_widgets.[ch].
- ru_dzapp.c REWRITTEN with sections (Home, Devices, Scenes, Energy, Weather, Settings + sub screens), swipe pager, fb line. Not yet compiled at time of this note.
- NEXT: ru_apps_table.c (ru_app_t descriptors), CMake block (ru_apps_core + tests), sim driver (fixtures), tests, screenshots.
- Coordinator note (workspace-32 stopped editing ru_dzapp.*; it only touches sim/tests/test_dzapp.c + docs): (1) avoid format-truncation in snprintf buffers (fixed char c[24]); (2) cb_hold: RU_HOLD_CANCELLED must NOT leave the confirm screen (only the CANCEL button does); test_dzapp has 'short press keeps the confirm view' - keep green; test_dzapp (ctest 'dzapp', real client over sockets vs sim/mock via tests/run_with_mock.sh) must stay green.
- [checkpoint] Compiles; test_apps (sim/tests/test_apps.c, 734 checks, 466+480) green; apps_fb/apps_view host tests green. Files: components/ru_apps/{ru_dzapp.c,ru_apps_table.c,ru_fb.c,ru_dzview.c}, sim/src/apps_demo.[ch] (fixture driver), sim/src/main.c (--apps DIR / --app NAME --shot F / --list-apps), sim/CMakeLists.txt (ru_apps, ru_sim_apps, test_apps, app_screenshots target), top-level CMake block "Apps agent".
- Build: cmake -S . -B /tmp/appsbuild -DRU_BUILD_SIM=ON; cmake --build /tmp/appsbuild -j6 (background!). Screens: /tmp/appsbuild/sim/roundos_sim --size 466 --apps sim/screenshots
- NEXT: view screenshots + fix visible defects; update test_dzapp.c for new semantics (done edits, verify ctest dzapp); full ctest both modes; copy final screenshots to sim/screenshots; final report.

## State at end of session (apps agent)
Done: all deliverables below compile and are tested.
- components/ru_apps: ru_fb (feedback pending/confirmed/unconfirmed/failed), ru_dzview (pure view model), ru_dzapp (sections Home/Devices/Scenes/Energy/Weather/Settings + sub screens, swipe pager, confirm/hold, detail per capability), ru_apps_table (6 ru_app_t descriptors, ru_apps_register).
- ru_ui: ru_state_tile / ru_state_dot widgets.
- sim: apps_demo.[ch] fixture driver, roundos_sim --apps/--app/--list-apps, tests/test_apps.c, screens.c 'scenes' demo fixed (Scene = push, Group = On/Off/MIXED).
- Semantics: Scene tile = PUSH indicator + "SCENE" (+ "ACTIVATED" while the command is CONFIRMED), tap -> hold -> SCENE_ON only. Group tile = ON filled / OFF hollow / MIXED half-filled; tap -> hold -> SCENE_OFF only when fully On, else SCENE_ON. Unconfirmed (e.g. Mixed after On) shows "STILL MIXED"/"NOT CONFIRMED" in warn colour for 7 s.
- Lock detail: state in words + button for the OTHER state (On = lock, Off = unlock), always via hold. (Old code sent the wrong direction.)
- Dial screens (dimmer, selector, thermostat, blinds): the device name doubles as BACK (no room for a BACK label).
Commands: see final report. Screenshots: sim/screenshots/app_*_{466,480}.png (+ regenerated demo set).
Open: see report "Requests".

## Clock page and start page (wave 3)
- New first section RU_SEC_CLOCK (app id org.dzpanel.clock, always available like Settings): time (hero font, minutes accent), date, up to three ring gauges from the first numeric favourites (ru_dzv_gauge in ru_dzview.c). RU_SEC_HOME was renamed RU_SEC_FAVS.
- Time comes from dz_model_server_now (server_epoch_s + monotonic delta, PARTIAL commits do not move it). Unknown: "--:--", no date. Day arithmetic is local to ru_dzview.c (ru_dzv_clock_text, ru_dzv_date_text), no libc time, no zone or DST rules: the digits are the server's.
- In place: the time label is a fixed-width box (a size change would redraw twice); a minute change redraws only that label (test_dzflicker test_clock_minute). Ticks inside a minute copy nothing from the model (time-only path in ru_dzapp_ui_tick); gauges update through ru_ring_gauge_set_display; the structure signature (clock_sig) covers which devices are gauges, their capability and ring kind.
- Start page: ru_dzapp_set_home_section(CLOCK | FAVS), default CLOCK. Home, back (false on the start page), the first screen and the runtime's home app follow it. Tests that are about the Favourites grid set FAVS explicitly in their start helpers.
- Sim screens: clock, clock_nosync, clock_offline, clock_stale, clock_two, clock_none (clock demos set Celsius units).
- Note: the first commit of the Clock code (8de7213, "..") was a sweep of work in progress made outside this series; commit 13f4a0a adds its tests.

## Settings, rotation and preferences (wave 3)
- ru_prefs (components/ru_cfg): namespace "ui", keys "rot" ("0" "90" "180" "270") and "start" ("clock" "favs"); a missing or invalid value reads as rotation 0 / Clock. Factory reset (svc_factory_reset) erases the namespace. tests/test_prefs.c.
- Settings has seven rows (STATUS, TOKEN, ACCESS, START PAGE, ROTATION, BRIGHTNESS, ABOUT) at 9u, no scrolling, inside the circle (test_apps settings_prefs checks the corners). START PAGE: a tap toggles Clock / Favourites at once. ROTATION opens RU_SUB_ROTATION: four chips in a compass layout (the chip's position is the panel edge the top of the picture moves to: 0 top, 90 left, 180 bottom, 270 right); a rotation the board cannot do is dimmed and refused with "NOT SUPPORTED ON THIS PANEL".
- Platform seam: ru_dzapp_set_pref_hook (RU_PREF_START / ROTATION / RESTART, answers APPLIED / NEEDS_RESTART / UNSUPPORTED) and ru_dzapp_set_rotation_info(current, mask). NEEDS_RESTART goes straight to a hold "RESTART TO APPLY" (the confirm screen with pend_restart); cancelling leaves the line "RESTART TO APPLY" under the chips, which offers the hold again. The hold calls the hook with RU_PREF_RESTART (the firmware calls esp_restart).
- Geometry: ru_rot_to_panel / ru_rot_to_logical (ru_geom.h) use LVGL's own mapping (lv_display_rotate_area, the pointer transform in lv_indev.c): 90 sends logical (x, y) to panel (y, D-1-x).
- Simulator: LVGL stays upright. sim_t.fb is always the logical framebuffer; sim_masked_copy (the window) and sim_save_png turn the OUTPUT, the mouse goes through sim_pointer_panel. `dzpanel_sim --rotate N` (PNG names get _rN); the apps driver (appfx_prefs_attach) keeps the prefs in a ru_kv_mem and applies a rotation live. Tests: test_sim_ui rotation (PNG == rotated framebuffer, a tap at the panel position of a tile hits it), test_apps rotated_gestures (rim back-swipe and content swipe in panel coordinates, rotation through the settings screen).
- Device: ru_hal_set_rotation(deg) before ru_hal_init (checked against ru_board_t.rotations), port API ru_port_start(indev, rot) and ru_port_rotation() (what was really applied). touch_read_wrap maps the sample to logical coordinates for ru_gesture, and for LVGL too when the port rotates in hardware (LVGL's display rotation stays 0: AMOLED MADCTL, Guition adapter); a port that uses lv_display_set_rotation leaves LVGL to do it. app_main loads the prefs before ru_hal_init; ui_shell passes the start page, the rotation in force and the hook (hold -> esp_restart).
- Boards: AMOLED 1.75 via bsp_display_start_with_config + bsp_display_rotation_set (rotations 0xF); Guition via esp_lv_adapter rotation with TRIPLE_PARTIAL tear avoidance and 3 PSRAM frame buffers when rotated, one buffer upright (rotations 0xF). All of it is unverified on hardware: docs/bringup.md step 3b, docs/hardware/verified.md.
