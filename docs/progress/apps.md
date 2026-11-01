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
