# Needs LVGL. geom.c / ru_geom.h are LVGL-free but live here (ru_shell depends on them).
idf_component_register(SRCS geom.c ru_theme.c ru_widgets.c ru_splash.c
                       INCLUDE_DIRS include
                       REQUIRES lvgl__lvgl ru_data)
