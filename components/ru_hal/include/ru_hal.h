/* ru_hal: thin hardware layer over the Waveshare BSP. App code never calls the BSP; it calls this.
 * Compiles on ESP-IDF only (the host build uses the SDL simulator instead).
 *
 *   ru_hal_init()           display + touch + LVGL port (LVGL task, tick, draw buffers) via the BSP
 *   ru_hal_lock/unlock()    guard every LVGL call made outside the LVGL task
 *   ru_hal_set_brightness() 0..100 percent
 *   ru_hal_set_touch_sink() receive every touch sample as an ru_touch_t for ru_gesture
 *
 * Board profile: the rest of the OS asks the profile, never "is this the 1.75". */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ru_gesture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *id;       /* "amoled_175" */
  int diameter;         /* px, 466 for the 1.75" */
  int safe_inset_px;    /* content kept this far inside the circle edge */
  bool round;           /* physical shape (2.16" is unverified) */
  bool has_touch;
  bool has_brightness;
  /* Informational today: BSP 3.0.1 fixes the LVGL buffers at 2 x 50 lines in PSRAM (esp_lvgl_adapter).
   * These fields become real knobs if the HAL stops using bsp_display_start(). */
  bool draw_buf_psram;
  int draw_buf_lines;
  uint32_t flash_mb;
} ru_board_t;

const ru_board_t *ru_board(void);

typedef enum { RU_HAL_OK = 0, RU_HAL_ERR_DISPLAY, RU_HAL_ERR_TOUCH, RU_HAL_ERR_STATE } ru_hal_err_t;

/* Idempotent. A touch failure clears has_touch and still returns RU_HAL_OK. */
ru_hal_err_t ru_hal_init(void);
bool ru_hal_has_touch(void);

/* Recursive-safe lock around LVGL. timeout_ms < 0 waits forever. Returns false on timeout. */
bool ru_hal_lock(int timeout_ms);
void ru_hal_unlock(void);

void ru_hal_set_brightness(int percent);
int ru_hal_get_brightness(void);

/* Called from the LVGL task (with the LVGL lock held) for every touch read. Keep it short; post to a queue. */
typedef void (*ru_hal_touch_cb_t)(const ru_touch_t *sample, void *ud);
void ru_hal_set_touch_sink(ru_hal_touch_cb_t cb, void *ud);

/* Free heap in bytes: internal RAM and PSRAM. */
void ru_hal_heap_log(const char *tag);

#ifdef __cplusplus
}
#endif
