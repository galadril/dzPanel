/* Demo screens built only from ru_widgets. Fake data, no Domoticz. */
#pragma once
#include "lvgl.h"

extern const char *const sim_screen_names[];
extern const int sim_screen_count;

/* Builds a fresh screen (not loaded). Returns NULL for an unknown name. now_ms is the demo clock. */
lv_obj_t *sim_screen_build(const char *name, uint32_t now_ms);
