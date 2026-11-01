#include "ru_hal.h"

/* Waveshare ESP32-S3-Touch-AMOLED-1.75: 466 px round, CO5300 + CST9217, 16 MB flash, 8 MB PSRAM.
 * The 2.16" (480 px, shape unverified) gets its own profile file later; ru_hal.c stays unchanged. */
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
