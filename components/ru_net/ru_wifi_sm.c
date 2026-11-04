#include "ru_wifi_sm.h"

#include <string.h>

const char *ru_wifi_state_name(ru_wifi_state_t s) {
  switch (s) {
    case RU_WIFI_IDLE: return "idle";
    case RU_WIFI_CONNECTING: return "connecting";
    case RU_WIFI_CONNECTED: return "connected";
    case RU_WIFI_FAILED: return "failed";
    case RU_WIFI_AP_REQUESTED: return "ap_requested";
  }
  return "?";
}

void ru_wifi_sm_init(ru_wifi_sm_t *sm, const ru_wifi_sm_cfg_t *cfg, const ru_wifi_sm_ops_t *ops) {
  memset(sm, 0, sizeof *sm);
  if (cfg) sm->cfg = *cfg;
  if (ops) sm->ops = *ops;
  if (!sm->cfg.connect_timeout_ms) sm->cfg.connect_timeout_ms = 15000;
  if (!sm->cfg.backoff_base_ms) sm->cfg.backoff_base_ms = 2000;
  if (!sm->cfg.backoff_max_ms) sm->cfg.backoff_max_ms = 60000;
  if (!sm->cfg.max_failures) sm->cfg.max_failures = 3;
  sm->state = RU_WIFI_IDLE;
}

static void set(ru_wifi_sm_t *sm, ru_wifi_state_t s) {
  if (sm->state == s) return;
  ru_wifi_state_t old = sm->state;
  sm->state = s;
  if (sm->ops.on_state) sm->ops.on_state(sm->ops.user, old, s);
}

static void begin_connect(ru_wifi_sm_t *sm, uint32_t now) {
  sm->attempts++;
  sm->due_ms = now + sm->cfg.connect_timeout_ms;
  set(sm, RU_WIFI_CONNECTING);
  if (sm->ops.connect) sm->ops.connect(sm->ops.user);
}

static void fail(ru_wifi_sm_t *sm, uint32_t now) {
  sm->failures++;
  bool may_ap = !sm->ever_connected || sm->cfg.ap_after_drop;
  if (may_ap && sm->failures >= sm->cfg.max_failures) {
    set(sm, RU_WIFI_AP_REQUESTED);
    sm->ap_requests++;
    if (sm->ops.request_ap) sm->ops.request_ap(sm->ops.user);
    return;
  }
  uint32_t wait = sm->cfg.backoff_base_ms;
  for (unsigned i = 1; i < sm->failures && wait < sm->cfg.backoff_max_ms; i++) wait = wait > UINT32_MAX / 2 ? UINT32_MAX : wait * 2;
  if (wait > sm->cfg.backoff_max_ms) wait = sm->cfg.backoff_max_ms;
  sm->due_ms = now + wait;
  set(sm, RU_WIFI_FAILED);
}

void ru_wifi_sm_start(ru_wifi_sm_t *sm, bool has_creds, uint32_t now) {
  sm->has_creds = has_creds;
  sm->failures = 0;
  sm->ever_connected = false;
  if (has_creds) begin_connect(sm, now);
  else set(sm, RU_WIFI_IDLE);
}

void ru_wifi_sm_event(ru_wifi_sm_t *sm, ru_wifi_event_t ev, int arg, uint32_t now) {
  switch (ev) {
    case RU_WIFI_EV_CREDS:
      sm->has_creds = true;
      sm->failures = 0;
      sm->ever_connected = false;
      if (sm->state == RU_WIFI_CONNECTED || sm->state == RU_WIFI_CONNECTING) { if (sm->ops.disconnect) sm->ops.disconnect(sm->ops.user); }
      begin_connect(sm, now);
      sm->due_ms = now + sm->cfg.connect_timeout_ms;
      break;
    case RU_WIFI_EV_CREDS_CLEARED:
      sm->has_creds = false;
      sm->failures = 0;
      sm->ever_connected = false;
      if (sm->state != RU_WIFI_IDLE && sm->ops.disconnect) sm->ops.disconnect(sm->ops.user);
      set(sm, RU_WIFI_IDLE);
      break;
    case RU_WIFI_EV_CONNECTED:
      if (sm->state != RU_WIFI_CONNECTING && sm->state != RU_WIFI_FAILED && sm->state != RU_WIFI_AP_REQUESTED) break;
      sm->failures = 0;
      sm->ever_connected = true;
      set(sm, RU_WIFI_CONNECTED);
      break;
    case RU_WIFI_EV_DISCONNECTED:
      sm->last_reason = arg;
      if (sm->state == RU_WIFI_CONNECTED) {
        sm->failures = 0; /* the link worked: retry at once, backoff only if that fails */
        begin_connect(sm, now);
      } else if (sm->state == RU_WIFI_CONNECTING) fail(sm, now);
      break;
    case RU_WIFI_EV_RETRY:
      if (!sm->has_creds || sm->state == RU_WIFI_CONNECTED || sm->state == RU_WIFI_CONNECTING) break;
      sm->failures = 0;
      begin_connect(sm, now);
      break;
  }
}

void ru_wifi_sm_tick(ru_wifi_sm_t *sm, uint32_t now) {
  if (sm->state != RU_WIFI_CONNECTING && sm->state != RU_WIFI_FAILED) return;
  if ((int32_t)(now - sm->due_ms) < 0) return;
  if (sm->state == RU_WIFI_FAILED) { begin_connect(sm, now); return; }
  sm->last_reason = -1; /* timeout */
  if (sm->ops.disconnect) sm->ops.disconnect(sm->ops.user);
  fail(sm, now);
}

uint32_t ru_wifi_sm_ms_until_due(const ru_wifi_sm_t *sm, uint32_t now) {
  if (sm->state != RU_WIFI_CONNECTING && sm->state != RU_WIFI_FAILED) return UINT32_MAX;
  int32_t d = (int32_t)(sm->due_ms - now);
  return d > 0 ? (uint32_t)d : 0;
}

bool ru_wifi_attempt_begin(ru_wifi_attempt_t *a, uint32_t *ticket) {
  if (a->st == RU_WIFI_ATT_PENDING) return false;
  a->ticket++;
  a->st = RU_WIFI_ATT_PENDING;
  if (ticket) *ticket = a->ticket;
  return true;
}

void ru_wifi_attempt_on_state(ru_wifi_attempt_t *a, ru_wifi_state_t to) {
  if (a->st != RU_WIFI_ATT_PENDING) return; /* final already: the revert's CONNECTED is not ours */
  if (to == RU_WIFI_CONNECTED) a->st = RU_WIFI_ATT_OK;
  else if (to == RU_WIFI_FAILED || to == RU_WIFI_AP_REQUESTED) a->st = RU_WIFI_ATT_FAILED;
}

ru_wifi_att_state_t ru_wifi_attempt_poll(const ru_wifi_attempt_t *a, uint32_t ticket) {
  return a->ticket == ticket ? a->st : RU_WIFI_ATT_IDLE;
}

void ru_wifi_attempt_abort(ru_wifi_attempt_t *a, uint32_t ticket) {
  if (a->ticket == ticket && a->st == RU_WIFI_ATT_PENDING) a->st = RU_WIFI_ATT_FAILED;
}
