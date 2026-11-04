/* Wi-Fi manager IDF glue around ru_wifi_sm (esp_wifi STA). Compile-checked, not run on a device.
 * Credentials live in the "secrets" namespace (ru_cfg: ru_wifi_creds_*). The password is never logged. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "ru_cfg.h"
#include "ru_cfgline.h"
#include "ru_improv.h"
#include "ru_wifi_sm.h"

typedef struct {
  ru_kv_t *kv;
  ru_wifi_sm_cfg_t sm; /* zeros = defaults (15 s timeout, 2 s..60 s backoff, AP after 3 failures) */
  void (*on_state)(void *user, ru_wifi_state_t from, ru_wifi_state_t to); /* optional; called with the manager's lock held */
  void (*on_ap_requested)(void *user);  /* start the setup portal (fw-core / portal owner) */
  void (*on_provision_result)(void *user, bool ok); /* result of ru_wifi_esp_try_new (for Improv) */
  void *user;
} ru_wifi_esp_cfg_t;

/* Initialises netif/event loop/esp_wifi (idempotent for the loop), starts STA and the manager task. nvs_flash_init must
 * have run. Returns false on failure. */
bool ru_wifi_esp_start(const ru_wifi_esp_cfg_t *cfg);
bool ru_wifi_esp_connected(void);
ru_wifi_state_t ru_wifi_esp_state(void);
/* Blocking scan (a few seconds), strongest first, de-duplicated by SSID. For Improv ops.wifi_scan. */
size_t ru_wifi_esp_scan(ru_improv_net_t *out, size_t max);
/* Try new credentials (Improv set-wifi). Saved only after an IP is obtained; on failure the previous credentials are
 * restored. The outcome is reported through on_provision_result. false = could not start, or busy (another attempt is pending). */
bool ru_wifi_esp_try_new(const char *ssid, const char *pass);
/* Blocking variant for the RU+CFG line (ru_cfgline_ops_t.wifi_apply): same rules (saved only after an IP, else the previous
 * credentials come back) but the result is returned, not delivered through on_provision_result. Waits at most timeout_ms.
 * Failure reasons come from the driver's disconnect reason; unknown ones map to RU_WIFI_APPLY_FAILED. RU_WIFI_APPLY_BUSY when another attempt is pending. */
ru_wifi_apply_t ru_wifi_esp_apply_wait(const char *ssid, const char *pass, uint32_t timeout_ms);
/* Credentials arrived another way (portal): same as try_new but without the result callback semantics. */
void ru_wifi_esp_retry(void);  /* user asked to try again / leave AP_REQUESTED */
void ru_wifi_esp_forget(void); /* erase credentials, disconnect, IDLE */
