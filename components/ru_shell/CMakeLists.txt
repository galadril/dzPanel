# LVGL-free sources. Depends on ru_ui only for ru_geom.h (which pulls LVGL include paths transitively).
idf_component_register(SRCS ru_gesture.c ru_nav.c
                       INCLUDE_DIRS include
                       REQUIRES ru_ui)
