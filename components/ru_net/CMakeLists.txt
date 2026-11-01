idf_component_register(SRCS ru_cfgline.c ru_improv.c ru_console.c ru_wifi_sm.c ru_wifi_esp.c ru_console_esp.c
                       INCLUDE_DIRS include
                       REQUIRES ru_cfg ru_core
                       PRIV_REQUIRES esp_wifi esp_event esp_netif esp_driver_usb_serial_jtag)
