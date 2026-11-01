idf_component_register(SRCS ru_http.c ru_captive.c ru_portal.c ru_portal_esp.c
                       INCLUDE_DIRS include
                       REQUIRES ru_cfg ru_net ru_core
                       PRIV_REQUIRES esp_http_server esp_wifi esp_event esp_netif esp_hw_support lwip freertos)
