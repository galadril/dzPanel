#include "dz_cmd.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char *dz_cmd_err_name(dz_cmd_err_t e) {
  static const char *const n[] = {"accepted", "control-disabled", "unknown-device", "protected", "not-controllable",
                                  "bad-arg", "needs-confirm", "stale", "queue-full"};
  return (unsigned)e < 9 ? n[e] : "?";
}
const char *dz_outcome_name(dz_outcome_t o) {
  static const char *const n[] = {"confirmed", "unconfirmed", "failed"};
  return (unsigned)o < 3 ? n[o] : "?";
}

int dz_url_encode(char *dst, size_t cap, const char *src) {
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;
  for (; *src; src++) {
    unsigned char c = (unsigned char)*src;
    bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                 c == '_' || c == '~';
    if (n + (plain ? 1 : 3) + 1 > cap) return -1;
    if (plain) dst[n++] = (char)c;
    else { dst[n++] = '%'; dst[n++] = hex[c >> 4]; dst[n++] = hex[c & 15]; }
  }
  if (n >= cap) return -1;
  dst[n] = 0;
  return (int)n;
}

void dz_cmd_init(dz_cmd_t *c, dz_client_t *client, dz_model_t *model, bool allow_control, dz_cmd_cb cb, void *user) {
  memset(c, 0, sizeof *c);
  c->client = client;
  c->model = model;
  c->allow_control = allow_control;
  c->verify_delay_ms = 1500;
  c->cb = cb;
  c->user = user;
  c->next_id = 1;
}

static bool is_scene_cmd(dz_cmd_kind_t k) { return k == DZ_CMD_SCENE_ON || k == DZ_CMD_SCENE_OFF; }

/* Which capability may take which command. */
static bool cap_allows(const dz_device_t *d, dz_cmd_kind_t k) {
  switch (k) {
    case DZ_CMD_ON: case DZ_CMD_OFF: case DZ_CMD_TOGGLE:
      return d->cap == DZ_CAP_ONOFF || d->cap == DZ_CAP_DIMMER || d->cap == DZ_CAP_LOCK;
    case DZ_CMD_SET_LEVEL: return d->cap == DZ_CAP_DIMMER;
    case DZ_CMD_BLINDS_OPEN: case DZ_CMD_BLINDS_CLOSE: case DZ_CMD_BLINDS_STOP: return d->cap == DZ_CAP_BLINDS;
    case DZ_CMD_SELECTOR: return d->cap == DZ_CAP_SELECTOR;
    case DZ_CMD_SCENE_ON: return dz_cap_is_scene_ns(d->cap);   /* a scene is pushed; a group is switched on */
    case DZ_CMD_SCENE_OFF: return d->cap == DZ_CAP_GROUP;       /* a Scene has no Off: never sent */
    case DZ_CMD_SETPOINT: return d->cap == DZ_CAP_THERMOSTAT;
    case DZ_CMD_PUSH: return d->cap == DZ_CAP_PUSH;
    case DZ_CMD_MODE: return d->cap == DZ_CAP_MODE;
  }
  return false;
}

dz_risk_t dz_cmd_risk(const dz_device_t *d, dz_cmd_kind_t kind) {
  if (d->is_protected || !d->controllable || !cap_allows(d, kind)) return DZ_RISK_LOCKED;
  /* DESIGN.md section 7: locks, heating and scenes/groups need a hold. A group may contain
   * everything in the house and getscenes does not list members, so every scene counts. */
  if (d->cap == DZ_CAP_LOCK || dz_cap_is_scene_ns(d->cap) || d->cap == DZ_CAP_THERMOSTAT || d->cap == DZ_CAP_MODE)
    return DZ_RISK_CONFIRM;
  return DZ_RISK_SAFE;
}

static int selector_count(const dz_device_t *d) {
  if (!d->level_names[0]) return 0;
  int n = 1;
  for (const char *p = d->level_names; *p; p++) if (*p == '|') n++;
  return n;
}

dz_cmd_err_t dz_cmd_submit(dz_cmd_t *c, const dz_cmd_req_t *req, uint32_t now_ms, uint32_t *id) {
  dz_device_t d;
  ru_val_t v;
  ru_val_state_t st;
  if (!req || !req->idx || !*req->idx) return DZ_CMDE_BAD_ARG;
  if (!c->allow_control) return DZ_CMDE_CONTROL_DISABLED;
  for (const char *p = req->idx; *p; p++) if (*p < '0' || *p > '9') return DZ_CMDE_BAD_ARG;
  if (strlen(req->idx) >= sizeof((dz_cmd_slot_t *)0)->idx) return DZ_CMDE_BAD_ARG;
  if (!dz_model_get(c->model, req->idx, is_scene_cmd(req->kind), &d, &v, &st, now_ms)) return DZ_CMDE_UNKNOWN_DEVICE;
  if (d.is_protected) return DZ_CMDE_PROTECTED;
  if (!d.controllable || !cap_allows(&d, req->kind)) return DZ_CMDE_NOT_CONTROLLABLE;

  int level = -1;
  float setpoint = req->setpoint;
  if (req->kind == DZ_CMD_SET_LEVEL) {
    level = req->level < 0 ? 0 : req->level > d.max_level ? d.max_level : req->level;
  } else if (req->kind == DZ_CMD_SELECTOR) {
    int cnt = selector_count(&d);
    if (req->level < 0 || req->level % 10 != 0 || (cnt ? req->level / 10 >= cnt : req->level > d.max_level))
      return DZ_CMDE_BAD_ARG;
    level = req->level;
  } else if (req->kind == DZ_CMD_SETPOINT) {
    if (!isfinite(req->setpoint) || req->setpoint < -1000.0f || req->setpoint > 1000.0f) return DZ_CMDE_BAD_ARG;
    if (!d.can_set) return DZ_CMDE_NOT_CONTROLLABLE; /* no sane range: read-only */
    setpoint = req->setpoint < d.setpoint_min ? d.setpoint_min : req->setpoint > d.setpoint_max ? d.setpoint_max : req->setpoint;
  } else if (req->kind == DZ_CMD_MODE) {
    bool ok = false;
    for (int i = 0; i < d.n_modes && i < DZ_MAX_MODES; i++) ok = ok || d.mode_val[i] == req->level;
    if (!ok) return DZ_CMDE_BAD_ARG;
    level = req->level;
  }
  if (dz_cmd_risk(&d, req->kind) == DZ_RISK_CONFIRM && !req->confirmed) return DZ_CMDE_NEEDS_CONFIRM;
  if (st != RU_VAL_OK) return DZ_CMDE_STALE;

  for (size_t i = 0; i < DZ_CMD_QUEUE; i++) {
    dz_cmd_slot_t *s = &c->q[i];
    if (s->state) continue;
    memset(s, 0, sizeof *s);
    s->id = c->next_id++;
    s->req = *req;
    s->req.setpoint = setpoint; /* clamped to the device's range */
    s->req.idx = NULL;
    snprintf(s->idx, sizeof s->idx, "%s", req->idx);
    s->level = level;
    s->pre = d;
    s->state = 1;
    s->due_ms = now_ms;
    if (id) *id = s->id;
    return DZ_CMD_ACCEPTED;
  }
  return DZ_CMDE_QUEUE_FULL;
}

size_t dz_cmd_pending(const dz_cmd_t *c) {
  size_t n = 0;
  for (size_t i = 0; i < DZ_CMD_QUEUE; i++) n += c->q[i].state != 0;
  return n;
}

uint32_t dz_cmd_ms_until_due(const dz_cmd_t *c, uint32_t now_ms) {
  uint32_t best = UINT32_MAX;
  for (size_t i = 0; i < DZ_CMD_QUEUE; i++) {
    if (!c->q[i].state) continue;
    int32_t d = (int32_t)(c->q[i].due_ms - now_ms);
    uint32_t u = d > 0 ? (uint32_t)d : 0;
    if (u < best) best = u;
  }
  return best;
}

/* "21.5" without printf's locale-dependent %f */
static void fmt_setpoint(char *out, size_t cap, float sp) {
  long t = lroundf(sp * 10.0f);
  bool neg = t < 0;
  if (neg) t = -t;
  snprintf(out, cap, "%s%ld.%ld", neg ? "-" : "", t / 10, t % 10);
}

static bool build_query(const dz_cmd_slot_t *s, char *q, size_t cap) {
  const char *sw = NULL;
  char enc[48], num[24];
  switch (s->req.kind) {
    case DZ_CMD_ON: sw = (s->pre.cap == DZ_CAP_LOCK && s->pre.switch_type == 20) ? "Off" : "On"; break;
    case DZ_CMD_OFF: sw = (s->pre.cap == DZ_CAP_LOCK && s->pre.switch_type == 20) ? "On" : "Off"; break;
    case DZ_CMD_TOGGLE: sw = "Toggle"; break;
    case DZ_CMD_SET_LEVEL: case DZ_CMD_SELECTOR: sw = "Set Level"; break;
    case DZ_CMD_BLINDS_OPEN: sw = "Open"; break;
    case DZ_CMD_BLINDS_CLOSE: sw = "Close"; break;
    case DZ_CMD_BLINDS_STOP: sw = "Stop"; break;
    case DZ_CMD_PUSH: sw = s->pre.switch_type == 10 ? "Off" : "On"; break;
    case DZ_CMD_SCENE_ON: case DZ_CMD_SCENE_OFF: case DZ_CMD_SETPOINT: case DZ_CMD_MODE: break;
  }
  int n;
  if (s->req.kind == DZ_CMD_SCENE_ON || s->req.kind == DZ_CMD_SCENE_OFF) {
    n = snprintf(q, cap, "param=switchscene&idx=%s&switchcmd=%s", s->idx, s->req.kind == DZ_CMD_SCENE_ON ? "On" : "Off");
  } else if (s->req.kind == DZ_CMD_MODE) {
    n = snprintf(q, cap, "param=thermostatstate&idx=%s&state=%d", s->idx, s->level);
  } else if (s->req.kind == DZ_CMD_SETPOINT) {
    fmt_setpoint(num, sizeof num, s->req.setpoint);
    n = snprintf(q, cap, "param=setsetpoint&idx=%s&setpoint=%s", s->idx, num);
  } else {
    if (dz_url_encode(enc, sizeof enc, sw) < 0) return false;
    n = snprintf(q, cap, "param=switchlight&idx=%s&switchcmd=%s", s->idx, enc);
    if (n > 0 && (size_t)n < cap && s->level >= 0) n += snprintf(q + n, cap - (size_t)n, "&level=%d", s->level);
  }
  return n > 0 && (size_t)n < cap;
}

/* Did the re-read show the intended state? */
static bool matches(const dz_cmd_slot_t *s, const dz_device_t *a) {
  const dz_device_t *p = &s->pre;
  switch (s->req.kind) {
    case DZ_CMD_ON: case DZ_CMD_OFF: case DZ_CMD_TOGGLE: {
      bool tog = s->req.kind == DZ_CMD_TOGGLE;
      if (p->cap == DZ_CAP_LOCK) {
        /* ON always means "lock", OFF "unlock"; build_query sends the inverted word for type 20 locks. */
        bool lock = tog ? strcmp(p->status, "Locked") != 0 : s->req.kind == DZ_CMD_ON;
        return !strcmp(a->status, lock ? "Locked" : "Unlocked");
      }
      bool on = tog ? !dz_status_is_on(p) : s->req.kind == DZ_CMD_ON;
      return on ? dz_status_is_on(a) : !strcmp(a->status, "Off");
    }
    case DZ_CMD_SET_LEVEL:
      if (s->level == 0) return !strcmp(a->status, "Off");
      return dz_status_is_on(a) && a->has_level && a->level == s->level;
    case DZ_CMD_SELECTOR: return a->has_level && (a->level == s->level || a->level_int == s->level);
    case DZ_CMD_BLINDS_OPEN: return !strncmp(a->status, "Open", 4);
    case DZ_CMD_BLINDS_CLOSE: return !strncmp(a->status, "Closed", 6);
    case DZ_CMD_SCENE_ON:
      if (p->cap == DZ_CAP_SCENE) {
        /* A scene is pushed and then reads "On" for good, so "On" alone proves nothing if it was On before:
         * then the only evidence is a fresh LastUpdate. */
        if (!strcmp(p->status, "On")) return a->last_update[0] && strcmp(a->last_update, p->last_update) != 0;
        return !strcmp(a->status, "On");
      }
      /* Group: "Mixed" after On means some members did not switch. That is NOT confirmed. */
      return !strcmp(a->status, "On");
    case DZ_CMD_SCENE_OFF: return !strcmp(a->status, "Off"); /* groups only; "Mixed" is not Off either */
    case DZ_CMD_SETPOINT: return a->has_setpoint && fabsf(a->setpoint - s->req.setpoint) < 0.06f;
    case DZ_CMD_MODE: return a->has_mode && a->mode == s->level; /* read-after-write, as for every command */
    case DZ_CMD_BLINDS_STOP: case DZ_CMD_PUSH:
      /* no state to compare: the only evidence is a fresh LastUpdate */
      return a->last_update[0] && strcmp(a->last_update, p->last_update) != 0;
  }
  return false;
}

typedef struct {
  const char *idx;
  bool scene;
  dz_device_t got;
  bool found;
} grab_t;

static bool grab_cb(const dz_device_t *d, void *u) {
  grab_t *g = u;
  if (!g->found && !strcmp(d->idx, g->idx) && dz_cap_is_scene_ns(d->cap) == g->scene) { g->got = *d; g->found = true; }
  return true;
}

static void finish(dz_cmd_t *c, dz_cmd_slot_t *s, dz_outcome_t o, const dz_device_t *after) {
  dz_cmd_result_t r;
  memset(&r, 0, sizeof r);
  r.id = s->id;
  r.kind = s->req.kind;
  snprintf(r.idx, sizeof r.idx, "%s", s->idx);
  r.outcome = o;
  r.fail = o == DZ_OUT_FAILED ? s->fail : DZ_FAIL_NONE;
  r.level_sent = s->level;
  if (after) { r.have_after = true; r.after = *after; }
  s->state = 0;
  if (c->cb) c->cb(&r, c->user);
}

static void do_send(dz_cmd_t *c, dz_cmd_slot_t *s, uint32_t now) {
  char q[160];
  dz_http_result_t hr;
  if (!build_query(s, q, sizeof q)) { s->fail = DZ_FAIL_BAD_RESPONSE; finish(c, s, DZ_OUT_FAILED, NULL); return; }
  dz_net_err_t e = dz_client_request(c->client, q, &hr);
  if (e == DZ_NET_TIMEOUT) {
    /* the switch may or may not have happened: the re-read decides */
    s->uncertain = true;
  } else if (e == DZ_NET_CONNECT || e == DZ_NET_TLS) {
    s->fail = DZ_FAIL_TRANSPORT;
    finish(c, s, DZ_OUT_FAILED, NULL);
    return;
  } else if (hr.status == 401) {
    s->fail = DZ_FAIL_UNAUTHORIZED;
    dz_client_note_auth_failure(c->client);
    finish(c, s, DZ_OUT_FAILED, NULL);
    return;
  } else if (hr.status == 403) {
    s->fail = DZ_FAIL_FORBIDDEN;
    finish(c, s, DZ_OUT_FAILED, NULL);
    return;
  } else if (hr.status >= 500) {
    s->fail = DZ_FAIL_SERVER;
    finish(c, s, DZ_OUT_FAILED, NULL);
    return;
  } else if (hr.status != 200) {
    s->fail = DZ_FAIL_BAD_RESPONSE;
    finish(c, s, DZ_OUT_FAILED, NULL);
    return;
  } else {
    dz_result_t res;
    dz_err_t pe = dz_parse_response(c->client->cfg.buf, hr.len, NULL, NULL, &res);
    if (pe != DZ_OK) {
      s->fail = pe == DZ_ERR_STATUS ? DZ_FAIL_SERVER : DZ_FAIL_BAD_RESPONSE;
      finish(c, s, DZ_OUT_FAILED, NULL);
      return;
    }
    s->sent_ok = true;
  }
  s->state = 2;
  s->due_ms = now + c->verify_delay_ms;
}

static void do_verify(dz_cmd_t *c, dz_cmd_slot_t *s, uint32_t now) {
  char q[64];
  bool scene = is_scene_cmd(s->req.kind);
  if (scene) snprintf(q, sizeof q, "param=getscenes");
  else snprintf(q, sizeof q, "param=getdevices&rid=%s", s->idx);
  dz_http_result_t hr;
  dz_net_err_t e = dz_client_request(c->client, q, &hr);
  s->reads++;
  grab_t g;
  memset(&g, 0, sizeof g);
  g.idx = s->idx;
  g.scene = scene;
  bool got = false;
  if ((e == DZ_NET_OK) && hr.status == 200) {
    dz_result_t res;
    got = dz_parse_response(c->client->cfg.buf, hr.len, grab_cb, &g, &res) == DZ_OK && g.found;
    if (got) {
      dz_model_begin(c->model);
      dz_model_stage_cb(&g.got, c->model);
      dz_model_commit(c->model, DZ_COMMIT_PARTIAL, NULL, now);
    }
  } else if (hr.status == 401) {
    dz_client_note_auth_failure(c->client);
  }
  if (got && matches(s, &g.got)) { finish(c, s, DZ_OUT_CONFIRMED, &g.got); return; }
  if (s->reads >= 2) { finish(c, s, DZ_OUT_UNCONFIRMED, got ? &g.got : NULL); return; }
  s->due_ms = now + c->verify_delay_ms;
}

bool dz_cmd_pump(dz_cmd_t *c, uint32_t now) {
  /* oldest id first, one request per call */
  dz_cmd_slot_t *best = NULL;
  for (size_t i = 0; i < DZ_CMD_QUEUE; i++) {
    dz_cmd_slot_t *s = &c->q[i];
    if (!s->state || (int32_t)(now - s->due_ms) < 0) continue;
    if (!best || (int32_t)(s->id - best->id) < 0) best = s;
  }
  if (!best) return false;
  if (best->state == 1) do_send(c, best, now);
  else do_verify(c, best, now);
  return true;
}

bool dz_cmd_scene_tap_kind(const dz_device_t *d, dz_cmd_kind_t *kind) {
  if (!d || !kind || !dz_cap_is_scene_ns(d->cap)) return false;
  if (d->cap == DZ_CAP_SCENE) { *kind = DZ_CMD_SCENE_ON; return true; }
  *kind = dz_device_state(d) == DZ_STATE_ON ? DZ_CMD_SCENE_OFF : DZ_CMD_SCENE_ON;
  return true;
}
