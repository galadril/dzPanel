# dzPanel simulator: LVGL 9.4.0 (vendor/lvgl/src) + optional SDL2 window + headless PNG screenshots.
# Included from the top-level CMakeLists.txt when RU_BUILD_SIM is ON. Owned by the UI agent.
set(LVGL_ROOT ${CMAKE_SOURCE_DIR}/vendor/lvgl/src)

file(GLOB_RECURSE LVGL_SRCS ${LVGL_ROOT}/src/*.c)
list(FILTER LVGL_SRCS EXCLUDE REGEX "/src/drivers/")   # the simulator owns its SDL glue
add_library(lvgl_core STATIC ${LVGL_SRCS})
# Third party code: no -Werror / -Wpedantic here. Sanitizers stay on so leaks in our widgets are caught.
set(_lv_opts -w)
if(RU_SANITIZE)
  list(APPEND _lv_opts -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
set_target_properties(lvgl_core PROPERTIES COMPILE_OPTIONS "${_lv_opts}")
target_compile_definitions(lvgl_core PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_include_directories(lvgl_core SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${LVGL_ROOT} ${LVGL_ROOT}/src)
target_link_libraries(lvgl_core PUBLIC m)

# Halo theme + widgets (plain values only, no Domoticz dependency)
add_library(ru_ui_lvgl
  ${CMAKE_SOURCE_DIR}/components/ru_ui/ru_theme.c
  ${CMAKE_SOURCE_DIR}/components/ru_ui/fonts/ru_icons_24.c
  ${CMAKE_SOURCE_DIR}/components/ru_ui/ru_widgets.c
  ${CMAKE_SOURCE_DIR}/components/ru_ui/ru_splash.c)
target_include_directories(ru_ui_lvgl PUBLIC ${CMAKE_SOURCE_DIR}/components/ru_ui/include)
target_link_libraries(ru_ui_lvgl PUBLIC lvgl_core ru_ui ru_data)

# Simulator core (display, circular mask, PNG writer) and demo screens
add_library(ru_sim_core STATIC src/sim_core.c src/sim_png.c src/screens.c)
target_include_directories(ru_sim_core PUBLIC src)
target_link_libraries(ru_sim_core PUBLIC ru_ui_lvgl)

add_executable(dzpanel_sim src/main.c)
target_link_libraries(dzpanel_sim ru_sim_core)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
endif()
if(SDL2_FOUND)
  target_sources(dzpanel_sim PRIVATE src/sim_sdl.c)
  target_compile_definitions(dzpanel_sim PRIVATE RU_SIM_SDL=1)
  target_link_libraries(dzpanel_sim PkgConfig::SDL2)
  message(STATUS "dzpanel_sim: SDL2 window enabled")
else()
  message(STATUS "dzpanel_sim: SDL2 not found, headless screenshots only")
endif()

# Tests: layout stays inside the circle, every screen has lit pixels, widget behaviour with synthetic touch
add_executable(test_sim_ui tests/test_sim_ui.c)
target_include_directories(test_sim_ui PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_sim_ui ru_sim_core)
target_compile_definitions(test_sim_ui PRIVATE SHOT_DIR="${CMAKE_BINARY_DIR}/screenshots")
add_test(NAME sim_ui COMMAND test_sim_ui)

add_executable(test_splash tests/test_splash.c)
target_include_directories(test_splash PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_splash ru_sim_core)
add_test(NAME splash COMMAND test_splash)

# Regenerate the PNGs for both sizes: cmake --build build --target screenshots
add_custom_target(screenshots
  COMMAND dzpanel_sim --size 466 --all ${CMAKE_SOURCE_DIR}/sim/screenshots
  COMMAND dzpanel_sim --size 480 --all ${CMAKE_SOURCE_DIR}/sim/screenshots
  COMMAND dzpanel_sim --size 480 --shape square --all ${CMAKE_SOURCE_DIR}/sim/screenshots
  DEPENDS dzpanel_sim
  COMMENT "Rendering simulator screenshots to sim/screenshots")

# Domoticz app: joins model + client + commands + widgets. Generic: screens come from device capabilities.
add_library(ru_apps STATIC ${CMAKE_SOURCE_DIR}/components/ru_apps/ru_dzapp.c ${CMAKE_SOURCE_DIR}/components/ru_apps/ru_apps_table.c
  ${CMAKE_SOURCE_DIR}/components/ru_apps/ru_climate_ui.c)
target_include_directories(ru_apps PUBLIC ${CMAKE_SOURCE_DIR}/components/ru_apps/include)
target_link_libraries(ru_apps PUBLIC ru_ui_lvgl ru_apps_core ru_runtime ru_shell int_domoticz ru_core ru_data)

# Fixture-backed driver: every app on real-shaped data, no network (dzpanel_sim --apps, tests)
add_library(ru_sim_apps STATIC src/apps_demo.c)
target_include_directories(ru_sim_apps PUBLIC src)
target_link_libraries(ru_sim_apps PUBLIC ru_apps ru_sim_core)
target_link_libraries(dzpanel_sim ru_sim_apps)
target_compile_definitions(dzpanel_sim PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")

# Every app screen at 466 and 480: inside the circle, lit-pixel budget, behaviour (Scene push vs Group toggle, '--')
add_executable(test_apps tests/test_apps.c)
target_include_directories(test_apps PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_apps ru_sim_apps)
target_compile_definitions(test_apps PRIVATE SHOT_DIR="${CMAKE_BINARY_DIR}/screenshots" FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
add_test(NAME apps COMMAND test_apps)

# Regenerate the app screenshots: cmake --build <dir> --target app_screenshots
add_custom_target(app_screenshots
  COMMAND dzpanel_sim --size 466 --apps ${CMAKE_SOURCE_DIR}/sim/screenshots
  COMMAND dzpanel_sim --size 480 --apps ${CMAKE_SOURCE_DIR}/sim/screenshots
  COMMAND dzpanel_sim --size 480 --shape square --apps ${CMAKE_SOURCE_DIR}/sim/screenshots
  DEPENDS dzpanel_sim
  COMMENT "Rendering app screenshots to sim/screenshots")

# End to end against the Node mock: real client, real model, real screens, simulated touches
add_executable(test_dzapp tests/test_dzapp.c)
target_include_directories(test_dzapp PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_dzapp ru_apps ru_sim_core)
target_compile_definitions(test_dzapp PRIVATE SHOT_DIR="${CMAKE_BINARY_DIR}/screenshots")
add_test(NAME dzapp COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run_with_mock.sh $<TARGET_FILE:test_dzapp>)

# Flicker budget: invalidated pixels per refresh, in-place updates, model copies (fixture-backed, no network)
add_executable(test_dzflicker tests/test_dzflicker.c)
target_include_directories(test_dzflicker PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_dzflicker ru_sim_apps)
target_compile_definitions(test_dzflicker PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
add_test(NAME dzflicker COMMAND test_dzflicker)
