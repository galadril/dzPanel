# climate agent progress

Build: `cmake -S . -B /tmp/vbc -DRU_BUILD_SIM=OFF && cmake --build /tmp/vbc -j8 && (cd /tmp/vbc && ctest)`; sim build dir /tmp/climbuild (background).

## Done (in order)
- [x] dz_parse.h/.c: readings (dz_reading_t rd[8], dz_rk_t kinds), trend, battery, hum_status/forecast/dir_str/quality/vunit/sensor_unit/value_units, plan_ids (PlanIDs array), can_set, has_mode/mode/modes, caps DZ_CAP_SENSOR + DZ_CAP_MODE (appended). Wind/UV Temp -> DZ_RK_AUXTEMP, has_temp=false for them.
## Next
- units (getconfig) in dz_client/dz_model; plan lookup; dz_cmd MODE + setpoint clamp; ru_dzview switch cases; ru_climate view model + UI; fixtures/mock; tests.
- [x] units: dz_units_t in dz_model (dz_model_units/set_units, dz_units_temp_text, dz_model_plan_temp), dz_units_parse + PH_CONFIG phase in dz_client (after getversion, again on each full refresh; soft-fails). tests updated (test_dz_client: +config step, test_units).
- [x] dz_cmd: DZ_CMD_MODE (thermostatstate, UNVERIFIED), setpoint clamped to min..max, not sent without can_set. test_dz_cmd's Setpoint fake got min/max/step (needed since no-range = read-only).
- [x] mock (sim/mock/domoticz_mock.js): getconfig, units=imperial, --climate set (idx 100..125), thermostatstate. test_mock.js extended. Fixtures: tests/fixtures/mock_climate_{metric,imperial}.json + mock_getconfig_*.json + README.md; regen: node tests/tools/regen_climate_fixtures.js
- [x] tests/test_domoticz.c: climate families, edges, truncation+mutation over climate fixtures (ctest domoticz passes)
- NEXT: ru_climate (pure view model + LVGL screens) wired from ru_dzapp.c build_weather/detail; fw build check (see below); sim tests in test_apps.c; screenshots.
- coordinator ask (fw-core final): (1) every switch over dz_cap_t must list new caps explicitly (no default); (2) run `cd /workspace/firmware; export IDF_TOOLS_PATH=/home/dev/.idf-shadow; . /opt/esp/idf/export.sh; idf.py -B /tmp/fw/build build` in BACKGROUND to verify (do not edit firmware/); (3) optional: public ru_dzapp_back() and ui_tick skipping rebuild while its screen not active.
- [x] components/ru_apps: ru_climate.[ch] (pure view model: units, pages, thermo, modes) + ru_climate_ui.[ch] (LVGL page/compass/overview; NOT yet compiled), ru_dzview value_text_u(units). ru_ui: ru_ring_gauge_set_display, ru_thermostat_set_unit/readonly. tests/test_apps_climate.c (ctest apps_climate) green; test_dz_mock extended (climate e2e, F setpoint, mode).
- NEXT: wire ru_dzapp.c (build_weather -> climate pager + overview; detail for TEMP/WEATHER/SENSOR/MODE/THERMOSTAT; home hero units; confirm verb with value; no `default` in cap switches), sim apps_demo sets + test_apps climate checks (466/480, bright-pixel <=12%), screenshots, firmware build check, notes to coordinator: components/ru_apps/CMakeLists.txt got ru_climate.c ru_climate_ui.c (fw-core file).
