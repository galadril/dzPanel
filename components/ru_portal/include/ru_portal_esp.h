/* ru_portal_esp: IDF glue for the setup portal: SoftAP (AP+STA), esp_http_server, a UDP DNS responder, one supervisor
 * task. COMPILE-CHECKED ONLY, not run on a device. All logic is in the pure layer (ru_portal.h).
 *
 * Nothing runs until ru_portal_esp_start(). Start it from: the Wi-Fi manager's on_ap_requested callback (call
 * ru_portal_esp_start(RU_PORTAL_SRC_WIFI_SM) only when ru_wifi_esp_state() == AP_REQUESTED), a long-press gesture, or a
 * Settings entry (RU_PORTAL_SRC_USER). ru_wifi_esp_start() must have run (it owns esp_netif, the event loop and STA).
 * Everything (AP, http server, DNS socket, tasks, buffers) is stopped and freed on stop, timeout or success. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "ru_portal.h"

typedef struct {
  ru_kv_t *kv;
  ru_portal_cfg_t cfg;        /* zeros = defaults (10 min idle, 5 failures, 12 char password) */
  ru_cfgline_ops_t test;      /* .test = dz_service_cfgline_test wrapped with its dz_probe_ctx_t as .user; .saved optional */
  /* Called from the supervisor task after teardown. Optional. */
  void (*on_stopped)(void *user, ru_portal_stop_t why);
  void *user;
} ru_portal_esp_cfg_t;

bool ru_portal_esp_init(const ru_portal_esp_cfg_t *cfg);      /* once, no radio use yet */
/* Scans (blocking, a few seconds, BEFORE the AP comes up so the AP channel does not hop), brings up AP+STA, DHCP with DNS
 * = AP address, the http server and the DNS responder. false: already running, bad source, or something failed (all
 * partial state is rolled back). */
bool ru_portal_esp_start(ru_portal_src_t src);
void ru_portal_esp_stop(void);                                 /* blocks until everything is torn down (<= ~6 s) */
bool ru_portal_esp_running(void);
/* Snapshot for the 'Set up this device' screen. Never blocks on a slow join (uses a cached copy). SECRET inside. */
bool ru_portal_esp_status(ru_portal_status_t *out);
bool ru_portal_esp_qr_wifi(char *out, size_t cap);
bool ru_portal_esp_qr_url(char *out, size_t cap);
