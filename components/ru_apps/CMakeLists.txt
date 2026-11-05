# Needs LVGL (via ru_ui). The Domoticz model/commands come through int_domoticz headers only.
idf_component_register(SRCS ru_apps_table.c ru_dzapp.c ru_dzview.c ru_fb.c ru_climate.c ru_climate_ui.c
                       INCLUDE_DIRS include
                       REQUIRES ru_ui ru_runtime ru_core ru_data int_domoticz lvgl__lvgl ru_shell)
# IDF builds with -Werror=format-truncation; the warning stays visible but does not stop the device build.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)
