# ESP-IDF registration only. The host build uses the top-level CMakeLists.txt, which does not read this file.
idf_component_register(SRCS ru_bus.c ru_osal_freertos.c
                       INCLUDE_DIRS include
                       REQUIRES esp_timer freertos)
