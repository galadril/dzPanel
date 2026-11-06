# LVGL-free. Device build: ESP transport + service; dz_transport.c / dz_transport_curl.c are host only.
idf_component_register(SRCS dz_parse.c dz_tabs.c dz_model.c dz_client.c dz_cmd.c dz_color.c dz_service.c dz_transport_esp.c
                       INCLUDE_DIRS include
                       REQUIRES ru_data ru_core ru_cfg
                       PRIV_REQUIRES esp_http_client esp-tls mbedtls esp_timer)
