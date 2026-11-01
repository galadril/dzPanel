cmake_minimum_required(VERSION 3.16)
# RoundOS firmware (ESP32-S3). Host code lives in ../components and is shared with the host build.
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
# Version: the VERSION file at the repo root, or the ROUNDOS_VERSION environment variable (CI sets nightlies like
# 0.1.0-nightly.20261101). It must match what tools/make_manifest.py accepts, and fit esp_app_desc (31 chars).
if(DEFINED ENV{ROUNDOS_VERSION})
  set(PROJECT_VER "$ENV{ROUNDOS_VERSION}")
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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(roundos)
