# ESP-IDF only. Board profile and port for the Waveshare ESP32-S3-Touch-AMOLED-1.75 (selected by RU_BOARD=amoled-175).
idf_component_register(SRCS ru_board_amoled_175.c
                       PRIV_REQUIRES ru_hal lvgl__lvgl waveshare__esp32_s3_touch_amoled_1_75)
