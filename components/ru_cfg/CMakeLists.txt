idf_component_register(SRCS ru_cfg.c ru_kv_mem.c ru_kv_nvs.c ru_prefs.c
                       INCLUDE_DIRS include
                       PRIV_REQUIRES nvs_flash)
