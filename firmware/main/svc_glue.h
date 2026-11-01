/* Network side of the device: Wi-Fi manager, USB console (Improv + RU+CFG) and the Domoticz service (ONE owner of
 * client, model and command layer). Compiled only, never run on a board. No function here logs the token, the URL
 * or the Wi-Fi password. */
#pragma once
#include <stdbool.h>

#include "dz_service.h"
#include "ru_bus.h"
#include "ru_kv.h"

/* Allocates the service (response buffer >= 128 KB in PSRAM, favourites-only fallback in internal RAM when PSRAM
 * cannot supply it). with_service=false (safe mode) skips the service entirely. */
bool svc_init(ru_kv_t *kv, ru_bus_t *bus, bool with_service);
/* Starts Wi-Fi, the console task and the service task; applies a stored configuration. */
void svc_start(void);

dz_service_t *svc_service(void);   /* NULL in safe mode or when init failed */
bool svc_provisioned(void);        /* Domoticz config AND Wi-Fi credentials are stored */
bool svc_cfg_set(void);
/* Erases Domoticz config, Wi-Fi credentials and the boot counter. Caller restarts. */
void svc_factory_reset(void);
