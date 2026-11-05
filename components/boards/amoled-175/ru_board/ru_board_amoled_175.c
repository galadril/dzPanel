#include "ru_hal.h"
#include "ru_hal_port.h"
#include "bsp/esp-bsp.h"

/* Waveshare ESP32-S3-Touch-AMOLED-1.75: 466 px round, CO5300 + CST9217, 16 MB flash, 8 MB PSRAM.
 * Other boards get their own directory under components/boards; ru_hal.c stays unchanged. */
static const ru_board_t k_board = {
    .id = "amoled_175",
    .diameter = 466,
    .safe_inset_px = 14,   /* unverified: calibrate on the real glass (BUILD_PLAN C3) */
    .round = true,
    .has_touch = true,
    .has_brightness = true,
    .draw_buf_psram = true,
    .draw_buf_lines = 50,
    .flash_mb = 16,
};

const ru_board_t *ru_board(void) { return &k_board; }

/* Port functions: the Waveshare BSP is the only place that knows pins and drivers for this board. */
lv_display_t *ru_port_start(lv_indev_t **indev) {
  lv_display_t *disp = bsp_display_start();
  /* bsp_display_start() fails hard (returns NULL) if touch init fails; if it returned a display we have touch. */
  *indev = disp ? bsp_display_get_input_dev() : NULL;
  return disp;
}

bool ru_port_lock(uint32_t ms) { return bsp_display_lock(ms) == ESP_OK; }
void ru_port_unlock(void) { bsp_display_unlock(); }
void ru_port_brightness_set(int percent) { bsp_display_brightness_set(percent); }
int ru_port_brightness_get(void) { return bsp_display_brightness_get(); }
