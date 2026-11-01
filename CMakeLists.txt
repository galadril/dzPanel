cmake_minimum_required(VERSION 3.16)
project(roundos C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic -Werror)
option(RU_SANITIZE "Build with ASAN+UBSAN" ON)
if(RU_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
find_package(Threads REQUIRED)

add_library(ru_core components/ru_core/ru_osal_pthread.c)
target_include_directories(ru_core PUBLIC components/ru_core/include)
target_link_libraries(ru_core PUBLIC Threads::Threads)

add_library(ru_ui components/ru_ui/geom.c)
target_include_directories(ru_ui PUBLIC components/ru_ui/include)
target_link_libraries(ru_ui PUBLIC m)

add_library(ru_data components/ru_data/ru_data.c)
target_include_directories(ru_data PUBLIC components/ru_data/include)

add_library(int_domoticz components/int_domoticz/dz_parse.c components/int_domoticz/dz_transport.c
  components/int_domoticz/dz_model.c components/int_domoticz/dz_client.c components/int_domoticz/dz_cmd.c)
target_include_directories(int_domoticz PUBLIC components/int_domoticz/include)
target_link_libraries(int_domoticz PUBLIC m ru_data ru_core)

enable_testing()
foreach(t osal geom data domoticz)
  add_executable(test_${t} tests/test_${t}.c)
  target_include_directories(test_${t} PRIVATE tests)
  target_link_libraries(test_${t} ru_core ru_ui ru_data int_domoticz)
  target_compile_definitions(test_${t} PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
  add_test(NAME ${t} COMMAND test_${t})
endforeach()

# ===== BEGIN Shell agent (ru_bus, ru_runtime, ru_shell) =====
target_sources(ru_core PRIVATE components/ru_core/ru_bus.c)

add_library(ru_runtime components/ru_runtime/ru_runtime.c components/ru_runtime/ru_heap.c)
target_include_directories(ru_runtime PUBLIC components/ru_runtime/include)
target_link_libraries(ru_runtime PUBLIC ru_core)

add_library(ru_shell components/ru_shell/ru_gesture.c components/ru_shell/ru_nav.c)
target_include_directories(ru_shell PUBLIC components/ru_shell/include)
target_link_libraries(ru_shell PUBLIC ru_ui m)

foreach(t bus runtime gesture nav)
  add_executable(test_${t} tests/test_${t}.c)
  target_include_directories(test_${t} PRIVATE tests)
  target_link_libraries(test_${t} ru_core ru_runtime ru_shell)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
target_compile_definitions(test_gesture PRIVATE TRACE_DIR="${CMAKE_SOURCE_DIR}/tests/traces/gesture")

# ThreadSanitizer build of the bus test: cmake -B build-tsan -DRU_SANITIZE=OFF -DRU_TSAN=ON
option(RU_TSAN "Build test_bus_tsan with ThreadSanitizer (use with -DRU_SANITIZE=OFF)" OFF)
if(RU_TSAN)
  add_executable(test_bus_tsan tests/test_bus.c components/ru_core/ru_bus.c components/ru_core/ru_osal_pthread.c)
  target_include_directories(test_bus_tsan PRIVATE tests components/ru_core/include)
  target_compile_options(test_bus_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(test_bus_tsan PRIVATE -fsanitize=thread)
  target_link_libraries(test_bus_tsan PRIVATE Threads::Threads)
  add_test(NAME bus_tsan COMMAND test_bus_tsan)
endif()
# ===== END Shell agent =====

# ===== UI agent: LVGL simulator, Halo theme and widgets (sim/, components/ru_ui/ru_theme.c ru_widgets.c) =====
option(RU_BUILD_SIM "Build the LVGL simulator, Halo widgets and UI tests" ON)
if(RU_BUILD_SIM AND EXISTS ${CMAKE_SOURCE_DIR}/vendor/lvgl/src/lvgl.h)
  add_subdirectory(sim)
endif()
# ===== END UI agent =====

# ===== Client agent: Domoticz transport, client, model, command layer =====
foreach(t dz_model dz_client dz_cmd dz_mock)
  add_executable(test_${t} tests/test_${t}.c)
  target_include_directories(test_${t} PRIVATE tests)
  target_link_libraries(test_${t} int_domoticz ru_data ru_core)
  target_compile_definitions(test_${t} PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
endforeach()
add_test(NAME dz_model COMMAND test_dz_model)
add_test(NAME dz_client COMMAND test_dz_client)
add_test(NAME dz_cmd COMMAND test_dz_cmd)
# integration: starts the Node mock on a private port, runs the C test against it, stops the mock
add_test(NAME dz_mock COMMAND ${CMAKE_SOURCE_DIR}/tests/run_with_mock.sh $<TARGET_FILE:test_dz_mock>)
add_test(NAME mock_js COMMAND node ${CMAKE_SOURCE_DIR}/sim/mock/test_mock.js)
add_executable(dz_live_check tests/dz_live_check.c)
target_link_libraries(dz_live_check int_domoticz ru_data ru_core)
# ===== END Client agent =====

# ===== BEGIN fw-net agent (ru_cfg, ru_net, dz_service) =====
add_library(ru_cfg components/ru_cfg/ru_cfg.c components/ru_cfg/ru_kv_mem.c)
target_include_directories(ru_cfg PUBLIC components/ru_cfg/include)
add_executable(test_cfg tests/test_cfg.c)
target_include_directories(test_cfg PRIVATE tests)
target_link_libraries(test_cfg ru_cfg)
add_test(NAME cfg COMMAND test_cfg)
add_library(ru_net components/ru_net/ru_cfgline.c components/ru_net/ru_improv.c components/ru_net/ru_console.c
  components/ru_net/ru_wifi_sm.c)
target_include_directories(ru_net PUBLIC components/ru_net/include)
target_link_libraries(ru_net PUBLIC ru_cfg)
add_executable(test_net tests/test_net.c)
target_include_directories(test_net PRIVATE tests)
target_link_libraries(test_net ru_net)
add_test(NAME net COMMAND test_net)
target_sources(int_domoticz PRIVATE components/int_domoticz/dz_service.c)
target_link_libraries(int_domoticz PUBLIC ru_cfg)
add_executable(test_dz_service tests/test_dz_service.c)
target_include_directories(test_dz_service PRIVATE tests)
target_link_libraries(test_dz_service int_domoticz ru_data ru_core ru_cfg)
target_compile_definitions(test_dz_service PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
add_test(NAME dz_service COMMAND test_dz_service)
add_executable(test_dz_service_mock tests/test_dz_service_mock.c)
target_include_directories(test_dz_service_mock PRIVATE tests)
target_link_libraries(test_dz_service_mock int_domoticz ru_data ru_core ru_cfg)
add_test(NAME dz_service_mock COMMAND ${CMAKE_SOURCE_DIR}/tests/run_with_mock.sh $<TARGET_FILE:test_dz_service_mock>)
# ===== END fw-net agent =====

# ===== BEGIN Apps agent (ru_apps_core: command feedback + view model, host-tested without LVGL) =====
add_library(ru_apps_core components/ru_apps/ru_fb.c components/ru_apps/ru_dzview.c components/ru_apps/ru_climate.c)
target_include_directories(ru_apps_core PUBLIC components/ru_apps/include)
target_link_libraries(ru_apps_core PUBLIC int_domoticz ru_data ru_core m)
foreach(t apps_fb apps_view apps_climate)
  add_executable(test_${t} tests/test_${t}.c)
  target_include_directories(test_${t} PRIVATE tests)
  target_link_libraries(test_${t} ru_apps_core)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
target_compile_definitions(test_apps_climate PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
# ===== END Apps agent =====

# ===== BEGIN portal agent (ru_portal: HTTP parser, forms, DNS, QR, portal state machine) =====
add_library(ru_portal components/ru_portal/ru_http.c components/ru_portal/ru_captive.c components/ru_portal/ru_portal.c)
target_include_directories(ru_portal PUBLIC components/ru_portal/include)
target_link_libraries(ru_portal PUBLIC ru_net ru_cfg)
add_executable(test_portal tests/test_portal.c)
target_include_directories(test_portal PRIVATE tests)
target_link_libraries(test_portal ru_portal)
add_test(NAME portal COMMAND test_portal)
# ===== END portal agent =====
