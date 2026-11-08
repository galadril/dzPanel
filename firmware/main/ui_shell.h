/* UI wiring: setup / safe-mode screens, the six Domoticz apps, gestures, bridge from dz_service events.
 * Compiled only, never run on a board. All functions except ui_shell_bridge_task need the LVGL lock held. */
#pragma once
#include <stdbool.h>

#include "sdkconfig.h"

#include "dz_service.h"
#include "ru_kv.h"

/* Builds the shell. app mode: apps registered with ru_runtime, shown once the device is provisioned; before that the
 * "not set up" screen. safe mode: only the safe screen. svc may be NULL in safe mode. kv holds the interface preferences
 * (ru_prefs: start page, rotation); NULL = defaults and nothing is stored. */
void ui_shell_start(dz_service_t *svc, bool safe_mode, unsigned failed_boots, ru_kv_t *kv);

#if CONFIG_RU_HEAP_LOG
/* One `disp` log line (numbers only): LVGL invalidation and render counters of the display and the Domoticz app's refresh
 * counters since the previous call, then resets them. Called by the heap log task. */
void ui_shell_log_disp(void);
#endif
