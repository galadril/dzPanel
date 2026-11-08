# ESP-IDF only. ru_board is the board component chosen by RU_BOARD (firmware/CMakeLists.txt); it owns pins and drivers.
idf_component_register(SRCS ru_hal.c
                       INCLUDE_DIRS include
                       REQUIRES ru_shell
                       PRIV_REQUIRES ru_board ru_ui lvgl__lvgl ru_core esp_timer esp_psram heap log)
