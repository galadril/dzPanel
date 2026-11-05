cmake_minimum_required(VERSION 3.16)
# Guition ESP32-S3-4848S040 bring-up probe: panel, backlight and touch only. No networking, no dzPanel UI.
# It builds the same ru_board component as the firmware, with the same sdkconfig defaults. See README.md.
set(_fw ${CMAKE_CURRENT_LIST_DIR}/../..)
set(EXTRA_COMPONENT_DIRS
  ${_fw}/../components/ru_core
  ${_fw}/../components/ru_data
  ${_fw}/../components/ru_ui
  ${_fw}/../components/ru_shell
  ${_fw}/../components/ru_hal
  ${_fw}/../components/boards/guition-4848s040)
set(SDKCONFIG_DEFAULTS "${_fw}/sdkconfig.defaults;${_fw}/boards/guition-4848s040/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
set(SDKCONFIG ${CMAKE_BINARY_DIR}/sdkconfig)
if(NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
  set(IDF_TARGET esp32s3)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(guition_probe)
