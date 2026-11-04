#include "ru_wifi_esp.h"

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "ru_osal.h"

static const char *TAG = "ru_wifi";

#define TRY_NEW_BUDGET_MS 45000 /* an Improv attempt never stays pending longer than this */

static struct {
  bool started;
  ru_wifi_esp_cfg_t cfg;
  ru_wifi_sm_t sm;
  ru_mutex_t *mu;
  ru_wifi_creds_t pending; /* SECRET while an Improv attempt is running */
  bool pending_on;
  uint32_t ticket; /* ticket of the pending attempt */
  ru_wifi_attempt_t att; /* busy rule and ticket (pure, tested in tests/test_net.c) */
  bool revert;
  bool quiet;       /* a blocking ru_wifi_esp_apply_wait is waiting: report through W.result, not on_provision_result */
  int result;       /* 0 pending, 1 joined and saved, 2 joined but not saved, 3 failed */
  int fail_reason;  /* sm.last_reason at failure: wifi_err_reason_t, or -1 for the SM's own timeout */
  bool connected;
  volatile bool stop;
} W;

/* Give up the pending attempt (W.mu held): wipe the password, fail the ticket, report once. revert = restore old creds. */
static void pending_fail(bool revert, int reason) {
  ru_wifi_creds_wipe(&W.pending);
  W.pending_on = false;
  ru_wifi_attempt_abort(&W.att, W.ticket);
  W.revert = revert; /* re-enter the SM from the task, not from its own callback */
  W.fail_reason = reason;
  if (W.quiet) W.result = 3;
  else if (W.cfg.on_provision_result) W.cfg.on_provision_result(W.cfg.user, false);
}

/* ---- SM ops (always called with W.mu held) ---- */
static void op_connect(void *u) {
  (void)u;
  ru_wifi_creds_t c;
  bool ok;
  if (W.pending_on) { c = W.pending; ok = true; }
  else ok = ru_wifi_creds_load(W.cfg.kv, &c) == RU_CFG_OK;
  if (!ok) { ESP_LOGW(TAG, "no stored credentials"); return; }
  wifi_config_t wc;
  memset(&wc, 0, sizeof wc);
  memcpy(wc.sta.ssid, c.ssid, strnlen(c.ssid, sizeof wc.sta.ssid));
  memcpy(wc.sta.password, c.pass, strnlen(c.pass, sizeof wc.sta.password));
  wc.sta.threshold.authmode = c.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  wc.sta.pmf_cfg.capable = true;
  wc.sta.pmf_cfg.required = false;
  ru_wifi_creds_wipe(&c);
  esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
  ru_secure_zero(&wc, sizeof wc);
  if (e == ESP_OK) e = esp_wifi_connect();
  if (e != ESP_OK && e != ESP_ERR_WIFI_CONN) ESP_LOGW(TAG, "connect start failed: %s", esp_err_to_name(e));
}
static void op_disconnect(void *u) { (void)u; (void)esp_wifi_disconnect(); }
static void op_ap(void *u) {
  (void)u;
  ESP_LOGW(TAG, "join failed repeatedly: asking for the setup portal");
  if (W.cfg.on_ap_requested) W.cfg.on_ap_requested(W.cfg.user);
}
static void op_state(void *u, ru_wifi_state_t from, ru_wifi_state_t to) {
  (void)u;
  W.connected = to == RU_WIFI_CONNECTED;
  ESP_LOGI(TAG, "wifi %s -> %s", ru_wifi_state_name(from), ru_wifi_state_name(to));
  ru_wifi_attempt_on_state(&W.att, to);
  if (W.pending_on) {
    if (to == RU_WIFI_CONNECTED) {
      bool saved = ru_wifi_creds_save(W.cfg.kv, W.pending.ssid, W.pending.pass) == RU_CFG_OK;
      ru_wifi_creds_wipe(&W.pending);
      W.pending_on = false;
      if (W.quiet) W.result = saved ? 1 : 2;
      else if (W.cfg.on_provision_result) W.cfg.on_provision_result(W.cfg.user, saved);
    } else if (to == RU_WIFI_FAILED || to == RU_WIFI_AP_REQUESTED) {
      pending_fail(true, W.sm.last_reason);
    } else if (to == RU_WIFI_IDLE) { /* credentials cleared under the attempt: nothing to revert to */
      pending_fail(false, -1);
    }
  }
  if (W.cfg.on_state) W.cfg.on_state(W.cfg.user, from, to);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *d = data;
    if (d->reason == WIFI_REASON_ASSOC_LEAVE) return; /* our own esp_wifi_disconnect() */
    ru_mutex_lock(W.mu);
    ru_wifi_sm_event(&W.sm, RU_WIFI_EV_DISCONNECTED, d->reason, ru_now_ms());
    ru_mutex_unlock(W.mu);
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ru_mutex_lock(W.mu);
    ru_wifi_sm_event(&W.sm, RU_WIFI_EV_CONNECTED, 0, ru_now_ms());
    ru_mutex_unlock(W.mu);
  }
}

static void mgr_task(void *arg) {
  (void)arg;
  while (!W.stop) {
    ru_mutex_lock(W.mu);
    uint32_t now = ru_now_ms();
    if (W.pending_on && !W.quiet && ru_wifi_attempt_expired(&W.att, now)) { /* Improv attempt out of time */
      ESP_LOGW(TAG, "new credentials: attempt timed out");
      pending_fail(true, -1);
    }
    if (W.revert) {
      W.revert = false;
      ru_wifi_creds_t c;
      if (ru_wifi_creds_load(W.cfg.kv, &c) == RU_CFG_OK) { ru_wifi_creds_wipe(&c); ru_wifi_sm_event(&W.sm, RU_WIFI_EV_CREDS, 0, now); }
      else ru_wifi_sm_event(&W.sm, RU_WIFI_EV_CREDS_CLEARED, 0, now);
    }
    ru_wifi_sm_tick(&W.sm, now);
    uint32_t ms = ru_wifi_sm_ms_until_due(&W.sm, now);
    ru_mutex_unlock(W.mu);
    ru_sleep_ms(ms > 250 ? 250 : (ms ? ms : 10));
  }
}

bool ru_wifi_esp_start(const ru_wifi_esp_cfg_t *cfg) {
  if (W.started || !cfg || !cfg->kv) return false;
  memset(&W, 0, sizeof W);
  W.cfg = *cfg;
  W.mu = ru_mutex_create();
  if (!W.mu) return false;
  ru_wifi_sm_ops_t ops = {op_connect, op_disconnect, op_ap, op_state, NULL};
  ru_wifi_sm_init(&W.sm, &cfg->sm, &ops);
  esp_err_t e = esp_netif_init();
  if (e != ESP_OK) return false;
  e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&ic) != ESP_OK) return false;
  if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return false; /* credentials only in our own namespace */
  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
  if (esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event, NULL) != ESP_OK) return false;
  if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL) != ESP_OK) return false;
  if (esp_wifi_start() != ESP_OK) return false;
  W.started = true;
  ru_wifi_creds_t c;
  bool has = ru_wifi_creds_load(cfg->kv, &c) == RU_CFG_OK;
  ru_wifi_creds_wipe(&c);
  ru_mutex_lock(W.mu);
  ru_wifi_sm_start(&W.sm, has, ru_now_ms());
  ru_mutex_unlock(W.mu);
  return ru_task_create("ru_wifi", mgr_task, NULL) != NULL;
}

bool ru_wifi_esp_connected(void) { return W.started && W.connected; }
ru_wifi_state_t ru_wifi_esp_state(void) { return W.sm.state; }

static int by_rssi(const void *a, const void *b) {
  return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

size_t ru_wifi_esp_scan(ru_improv_net_t *out, size_t max) {
  if (!W.started || !max) return 0;
  wifi_scan_config_t sc;
  memset(&sc, 0, sizeof sc);
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0; /* blocking */
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n > 24) n = 24;
  wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof *recs);
  if (!recs) { esp_wifi_clear_ap_list(); return 0; }
  esp_wifi_scan_get_ap_records(&n, recs);
  qsort(recs, n, sizeof *recs, by_rssi);
  size_t k = 0;
  for (uint16_t i = 0; i < n && k < max; i++) {
    if (!recs[i].ssid[0]) continue; /* hidden */
    bool dup = false;
    for (size_t j = 0; j < k; j++) if (!strcmp(out[j].ssid, (const char *)recs[i].ssid)) dup = true;
    if (dup) continue;
    snprintf(out[k].ssid, sizeof out[k].ssid, "%s", (const char *)recs[i].ssid);
    out[k].rssi = recs[i].rssi;
    out[k].secured = recs[i].authmode != WIFI_AUTH_OPEN;
    k++;
  }
  free(recs);
  return k;
}

bool ru_wifi_esp_try_new(const char *ssid, const char *pass) {
  if (!W.started || !ru_wifi_creds_valid(ssid, pass)) return false;
  ru_mutex_lock(W.mu);
  uint32_t ticket;
  if (W.pending_on || !ru_wifi_attempt_begin_until(&W.att, &ticket, ru_now_ms(), TRY_NEW_BUDGET_MS)) { ru_mutex_unlock(W.mu); return false; } /* busy */
  W.ticket = ticket;
  snprintf(W.pending.ssid, sizeof W.pending.ssid, "%s", ssid);
  snprintf(W.pending.pass, sizeof W.pending.pass, "%s", pass);
  W.pending_on = true;
  ru_wifi_sm_event(&W.sm, RU_WIFI_EV_CREDS, 0, ru_now_ms());
  ru_mutex_unlock(W.mu);
  return true;
}

static ru_wifi_apply_t map_reason(int r) {
  switch (r) {
    case -1: return RU_WIFI_APPLY_TIMEOUT;
    case WIFI_REASON_NO_AP_FOUND: return RU_WIFI_APPLY_NO_AP;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return RU_WIFI_APPLY_AUTH;
    default: return RU_WIFI_APPLY_FAILED; /* assoc fail, beacon timeout, ...: not reported reliably, so no detail */
  }
}

ru_wifi_apply_t ru_wifi_esp_apply_wait(const char *ssid, const char *pass, uint32_t timeout_ms) {
  if (!W.started || !ru_wifi_creds_valid(ssid, pass)) return RU_WIFI_APPLY_FAILED;
  ru_mutex_lock(W.mu);
  uint32_t ticket;
  if (W.pending_on || !ru_wifi_attempt_begin(&W.att, &ticket)) { ru_mutex_unlock(W.mu); return RU_WIFI_APPLY_BUSY; }
  W.ticket = ticket;
  snprintf(W.pending.ssid, sizeof W.pending.ssid, "%s", ssid);
  snprintf(W.pending.pass, sizeof W.pending.pass, "%s", pass);
  W.pending_on = true;
  W.quiet = true;
  W.result = 0;
  W.fail_reason = 0;
  ru_wifi_sm_event(&W.sm, RU_WIFI_EV_CREDS, 0, ru_now_ms());
  ru_mutex_unlock(W.mu);
  uint32_t t0 = ru_now_ms();
  int r = 0;
  for (;;) {
    ru_mutex_lock(W.mu);
    r = W.result;
    ru_mutex_unlock(W.mu);
    if (r || (uint32_t)(ru_now_ms() - t0) >= timeout_ms) break;
    ru_sleep_ms(50);
  }
  ru_mutex_lock(W.mu);
  if (!W.result) { /* budget used up (the SM's own 15 s timeout normally fires first): give up and restore the old credentials */
    pending_fail(true, -1); /* quiet: sets W.result = 3 */
  }
  r = W.result;
  int reason = W.fail_reason;
  W.quiet = false;
  W.result = 0;
  ru_mutex_unlock(W.mu);
  if (r == 1) return RU_WIFI_APPLY_OK;
  if (r == 2) return RU_WIFI_APPLY_STORE;
  return map_reason(reason);
}

void ru_wifi_esp_retry(void) {
  if (!W.started) return;
  ru_mutex_lock(W.mu);
  ru_wifi_sm_event(&W.sm, RU_WIFI_EV_RETRY, 0, ru_now_ms());
  ru_mutex_unlock(W.mu);
}

void ru_wifi_esp_forget(void) {
  if (!W.started) return;
  ru_mutex_lock(W.mu);
  (void)ru_wifi_creds_clear(W.cfg.kv);
  if (W.pending_on) pending_fail(false, -1); /* wipes W.pending, aborts the ticket; the SM's IDLE would also */
  else ru_secure_zero(&W.pending, sizeof W.pending);
  ru_wifi_sm_event(&W.sm, RU_WIFI_EV_CREDS_CLEARED, 0, ru_now_ms());
  ru_mutex_unlock(W.mu);
}
