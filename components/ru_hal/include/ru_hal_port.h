/* ru_hal_port: what each board provides to ru_hal. One implementation per board, in components/boards/<board>/ru_board
 * (selected at build time by RU_BOARD). ru_hal.c holds everything shared and calls only these. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up panel, touch, LVGL port (task, tick, draw buffers). Returns the display, or NULL on failure.
 * *indev receives the touch input device, or NULL if the board has none or touch init failed. */
lv_display_t *ru_port_start(lv_indev_t **indev);

/* Recursive-safe LVGL lock. Returns false on timeout. ms == UINT32_MAX waits forever. */
bool ru_port_lock(uint32_t ms);
void ru_port_unlock(void);

/* Backlight / panel brightness, 0..100 (already clamped by ru_hal). */
void ru_port_brightness_set(int percent);
int ru_port_brightness_get(void);

#ifdef __cplusplus
}
#endif
