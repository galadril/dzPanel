/* Wi-Fi manager as a pure, event-driven state machine. No IDF, no clock of its own: the caller passes now_ms
 * and calls ru_wifi_sm_tick when ru_wifi_sm_ms_until_due says so. The IDF glue is ru_wifi_esp.c.
 *
 *   IDLE ------creds------> CONNECTING --connected--> CONNECTED
 *                             |  ^ timeout/disconnect       |
 *                             v  | backoff elapsed          | disconnected (immediate retry, no AP fallback
 *                           FAILED ---(N consecutive)--> AP_REQUESTED     unless cfg.ap_after_drop)
 *
 * Failure k waits min(backoff_base << (k-1), backoff_max) before the next attempt. After max_failures consecutive
 * failures (default 3) with credentials that never worked, ops.request_ap fires ONCE and the machine waits in
 * AP_REQUESTED until RU_WIFI_EV_CREDS (new credentials) or RU_WIFI_EV_RETRY. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { RU_WIFI_IDLE, RU_WIFI_CONNECTING, RU_WIFI_CONNECTED, RU_WIFI_FAILED, RU_WIFI_AP_REQUESTED } ru_wifi_state_t;
typedef enum {
  RU_WIFI_EV_CREDS,        /* new credentials stored: reset failures and connect */
  RU_WIFI_EV_CREDS_CLEARED,/* credentials removed: disconnect, go IDLE */
  RU_WIFI_EV_CONNECTED,    /* got an IP */
  RU_WIFI_EV_DISCONNECTED, /* link lost or join failed (reason in arg) */
  RU_WIFI_EV_RETRY         /* user asked to try again (from FAILED, AP_REQUESTED or IDLE with creds) */
} ru_wifi_event_t;

typedef struct {
  uint32_t connect_timeout_ms; /* default 15000 */
  uint32_t backoff_base_ms;    /* default 2000 */
  uint32_t backoff_max_ms;     /* default 60000 */
  unsigned max_failures;       /* default 3 */
  bool ap_after_drop;          /* allow AP fallback after a link that worked once drops (default false) */
} ru_wifi_sm_cfg_t;

typedef struct {
  void (*connect)(void *user);     /* (re)start a join attempt with the stored credentials */
  void (*disconnect)(void *user);
  void (*request_ap)(void *user);  /* start the setup portal */
  void (*on_state)(void *user, ru_wifi_state_t from, ru_wifi_state_t to);
  void *user;
} ru_wifi_sm_ops_t;

typedef struct {
  ru_wifi_sm_cfg_t cfg;
  ru_wifi_sm_ops_t ops;
  ru_wifi_state_t state;
  unsigned failures; /* consecutive */
  bool ever_connected; /* with the current credentials */
  bool has_creds;
  uint32_t due_ms;
  int last_reason;
  uint32_t attempts, ap_requests;
} ru_wifi_sm_t;

void ru_wifi_sm_init(ru_wifi_sm_t *sm, const ru_wifi_sm_cfg_t *cfg, const ru_wifi_sm_ops_t *ops);
void ru_wifi_sm_start(ru_wifi_sm_t *sm, bool has_creds, uint32_t now_ms); /* boot: CONNECTING or IDLE */
void ru_wifi_sm_event(ru_wifi_sm_t *sm, ru_wifi_event_t ev, int arg, uint32_t now_ms);
void ru_wifi_sm_tick(ru_wifi_sm_t *sm, uint32_t now_ms);
uint32_t ru_wifi_sm_ms_until_due(const ru_wifi_sm_t *sm, uint32_t now_ms); /* UINT32_MAX = nothing scheduled */
const char *ru_wifi_state_name(ru_wifi_state_t s);
