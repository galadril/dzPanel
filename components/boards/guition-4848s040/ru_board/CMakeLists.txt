# ESP-IDF only. Board profile and port for the Guition ESP32-S3-4848S040 (selected by RU_BOARD=guition-4848s040).
# The public requirements are what ru_board_guition_4848s040.h exposes (probe-only accessors).
idf_component_register(SRCS ru_board_guition_4848s040.c
                       INCLUDE_DIRS .
                       REQUIRES esp_driver_i2c espressif__esp_lcd_touch
                       PRIV_REQUIRES ru_hal lvgl__lvgl esp_driver_ledc esp_driver_gpio esp_lcd
                                     espressif__esp_lcd_st7701 espressif__esp_lcd_panel_io_additions
                                     espressif__esp_lcd_touch_gt911 espressif__esp_lvgl_adapter)
