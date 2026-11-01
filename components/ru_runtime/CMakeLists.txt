# LVGL-free.
idf_component_register(SRCS ru_runtime.c ru_heap.c
                       INCLUDE_DIRS include
                       REQUIRES ru_core)
