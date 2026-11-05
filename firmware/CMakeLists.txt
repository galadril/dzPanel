cmake_minimum_required(VERSION 3.16)
# dzPanel firmware (ESP32-S3). Host code lives in ../components and is shared with the host build.
set(EXTRA_COMPONENT_DIRS
  ${CMAKE_CURRENT_LIST_DIR}/../components/ru_core
  ${CMAKE_CURRENT_LIST_DIR}/../components/ru_data
  ${CMAKE_CURRENT_LIST_DIR}/../components/ru_ui
  ${CMAKE_CURRENT_LIST_DIR}/../components/ru_runtime
  ${CMAKE_CURRENT_LIST_DIR}/../components/ru_shell
  ${CMAKE_CURRENT_LIST_DIR}/../components/ru_hal
  ${CMAKE_CURRENT_LIST_DIR}/../components/ru_cfg
  ${CMAKE_CURRENT_LIST_DIR}/../components/ru_net
  ${CMAKE_CURRENT_LIST_DIR}/../components/ru_apps
  ${CMAKE_CURRENT_LIST_DIR}/../components/int_domoticz)
# Version: the VERSION file at the repo root, or the DZPANEL_VERSION environment variable (CI sets nightlies like
# 0.1.0-nightly.20261101). It must match what tools/make_manifest.py accepts, and fit esp_app_desc (31 chars).
if(DEFINED ENV{DZPANEL_VERSION})
  set(PROJECT_VER "$ENV{DZPANEL_VERSION}")
else()
  file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/../VERSION PROJECT_VER LIMIT_COUNT 1)
endif()
if(NOT PROJECT_VER MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?$")
  message(FATAL_ERROR "bad version '${PROJECT_VER}': want 1.2.3 or 1.2.3-rc.1")
endif()
string(LENGTH "${PROJECT_VER}" _ver_len)
if(_ver_len GREATER 31)
  message(FATAL_ERROR "version '${PROJECT_VER}' is ${_ver_len} chars; esp_app_desc holds 31")
endif()

# Board selection (build time). Pass -DRU_BOARD=<board>, or use tools/build_fw.sh <board>. Each board has
# components/boards/<board>/ (component "ru_board": profile + ru_hal_port.h implementation) and
# firmware/boards/<board>/sdkconfig.defaults. Use one build directory per board: sdkconfig lives inside it.
set(RU_BOARD amoled-175 CACHE STRING "Target board: amoled-175 | guition-4848s040")
set(_ru_boards amoled-175 guition-4848s040)
if(NOT RU_BOARD IN_LIST _ru_boards)
  message(FATAL_ERROR "unknown RU_BOARD '${RU_BOARD}': want one of ${_ru_boards}")
endif()
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/../components/boards/${RU_BOARD}
   OR NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/boards/${RU_BOARD}/sdkconfig.defaults)
  message(FATAL_ERROR "board '${RU_BOARD}' has no driver yet: components/boards/${RU_BOARD} or "
                      "firmware/boards/${RU_BOARD}/sdkconfig.defaults is missing")
endif()
# SDKCONFIG_DEFAULTS are only read when sdkconfig is generated, so reusing a build dir for another board would keep
# the old board's sdkconfig. Stamp the board in the build dir and refuse a mismatch.
set(_ru_stamp ${CMAKE_BINARY_DIR}/ru_board.stamp)
if(EXISTS ${_ru_stamp})
  file(READ ${_ru_stamp} _ru_prev)
  string(STRIP "${_ru_prev}" _ru_prev)
  if(NOT _ru_prev STREQUAL RU_BOARD)
    message(FATAL_ERROR "build dir ${CMAKE_BINARY_DIR} was configured for board '${_ru_prev}', not '${RU_BOARD}'. "
                        "Use a separate build dir per board (tools/build_fw.sh does) or delete this one.")
  endif()
elseif(EXISTS ${CMAKE_BINARY_DIR}/sdkconfig)
  message(FATAL_ERROR "build dir ${CMAKE_BINARY_DIR} has an sdkconfig but no ru_board.stamp (configured before board stamping), "
                      "so its board is unknown. Delete the build dir or reconfigure it from scratch.")
else()
  file(WRITE ${_ru_stamp} "${RU_BOARD}\n")
endif()
list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../components/boards/${RU_BOARD})
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;boards/${RU_BOARD}/sdkconfig.defaults")
set(SDKCONFIG ${CMAKE_BINARY_DIR}/sdkconfig)
# All boards are ESP32-S3. A fresh build dir has no sdkconfig to take the target from, so state it (no set-target needed).
if(NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
  set(IDF_TARGET esp32s3)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Per-board component-manager lock file (IDF >= 5.x build property, resolved against the project dir).
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${RU_BOARD})
project(dzpanel)
