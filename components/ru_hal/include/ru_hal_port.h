/* ru_hal_port: what each board provides to ru_hal. One implementation per board, in components/boards/<board>/ru_board
 * (selected at build time by RU_BOARD). ru_hal.c holds everything shared and calls only these. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up panel, touch, LVGL port (task, tick, draw buffers) with the picture turned by rot degrees (0, 90, 180, 270; already
 * checked against ru_board_t.rotations by ru_hal). Returns the display, or NULL on failure. *indev receives the touch input device,
 * or NULL if the board has none or touch init failed.
 * LVGL's resolution stays D x D and the UI draws upright. HOW the picture is turned is the port's business:
 *   - in hardware (panel scan direction): LVGL knows nothing of it, lv_display_get_rotation() stays 0, and the touch read callback
 *     reports PANEL coordinates: ru_hal maps them to logical ones for LVGL and for ru_gesture;
 *   - in software (lv_display_set_rotation or the display adapter): LVGL turns the touch points itself, ru_hal only maps the copy it
 *     hands to ru_gesture. ru_hal tells the two apart by lv_display_get_rotation() after this returns. */
lv_display_t *ru_port_start(lv_indev_t **indev, int rot);

/* The rotation ru_port_start really applied (0, 90, 180 or 270): what was asked for, or 0 when the port could not turn the picture
 * (it then leaves the panel upright and says so here, so ru_hal maps touch the way the picture is). Valid after ru_port_start. */
int ru_port_rotation(void);

/* Recursive-safe LVGL lock. Returns false on timeout. ms == UINT32_MAX waits forever. */
bool ru_port_lock(uint32_t ms);
void ru_port_unlock(void);

/* Backlight / panel brightness, 0..100 (already clamped by ru_hal). */
void ru_port_brightness_set(int percent);
int ru_port_brightness_get(void);

#ifdef __cplusplus
}
#endif
