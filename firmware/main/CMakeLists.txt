idf_component_register(SRCS app_main.c boot_health.c svc_glue.c ui_shell.c heap_log.c
                       INCLUDE_DIRS .
                       REQUIRES nvs_flash esp_timer
                       PRIV_REQUIRES ru_hal ru_ui ru_core ru_data ru_runtime ru_shell ru_apps ru_cfg ru_net
                                     int_domoticz esp_system app_update esp_wifi)
