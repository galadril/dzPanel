# ESP-IDF only. The Waveshare BSP is the only place that knows pins and drivers.
idf_component_register(SRCS ru_hal.c ru_board_amoled_175.c
                       INCLUDE_DIRS include
                       REQUIRES ru_shell
                       PRIV_REQUIRES waveshare__esp32_s3_touch_amoled_1_75 lvgl__lvgl ru_core esp_timer esp_psram heap log)
