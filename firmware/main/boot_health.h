/* Boot health: a failed-boot counter in NVS (3 failed boots -> safe mode) and the point where a new OTA image is
 * declared good (esp_ota_mark_app_valid_cancel_rollback) after the UI has been running ~30 s.
 * Compiled only, never run on a board. */
#pragma once
#include <stdbool.h>

#include "ru_kv.h"

#define BH_SAFE_AFTER_FAILS 3
#define BH_STABLE_MS 30000u

/* Call first thing in app_main. Counts this boot as "not yet healthy" and returns true when safe mode is required. */
bool bh_boot_begin(ru_kv_t *kv);
unsigned bh_fail_count(void); /* failed boots seen before this one */
/* Marks display state; call after ru_hal_init(). */
void bh_set_display_ok(bool ok);
/* UI loop heartbeat: call from an LVGL timer (every 250 ms or so). */
void bh_heartbeat(void);
/* Starts the watcher task. safe_mode: never marks the image valid and never clears the counter. */
void bh_start(bool safe_mode);
/* Forget the failure count (factory reset path, successful provisioning while in safe mode). */
void bh_clear(void);
