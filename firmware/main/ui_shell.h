/* UI wiring: setup / safe-mode screens, the six Domoticz apps, gestures, bridge from dz_service events.
 * Compiled only, never run on a board. All functions except ui_shell_bridge_task need the LVGL lock held. */
#pragma once
#include <stdbool.h>

#include "dz_service.h"

/* Builds the shell. app mode: apps registered with ru_runtime, shown once the device is provisioned; before that the
 * "not set up" screen. safe mode: only the safe screen. svc may be NULL in safe mode. */
void ui_shell_start(dz_service_t *svc, bool safe_mode, unsigned failed_boots);
