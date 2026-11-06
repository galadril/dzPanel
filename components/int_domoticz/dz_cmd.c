#include "dz_cmd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *dz_cmd_err_name(dz_cmd_err_t e) {
  static const char *const n[] = {"accepted", "control-disabled", "unknown-device", "protected", "not-controllable",
                                  "bad-arg", "needs-confirm", "stale", "queue-full", "unsupported"};
  return (unsigned)e < 10 ? n[e] : "?";
}
const char *dz_outcome_name(dz_outcome_t o) {
  static const char *const n[] = {"confirmed", "unconfirmed", "failed"};
  return (unsigned)o < 3 ? n[o] : "?";
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

/* The MCP setpoint tool takes Celsius only. A server whose temperature unit is not KNOWN to be Celsius (getconfig failed or has not
 * run yet) is treated like a Fahrenheit one: a wrong guess would store 70 F as 70 C in a heating device. */
static bool setpoint_celsius(const dz_device_t *d, const dz_units_t *u) {
  return !strstr(d->vunit, "\xC2\xB0" "F") && u && u->have_temp && u->temp[0] == 'C';
}

/* Which capability may take which command. */
static bool cap_allows(const dz_device_t *d, dz_cmd_kind_t k, const dz_units_t *u) {
  switch (k) {
    case DZ_CMD_ON: case DZ_CMD_OFF: case DZ_CMD_TOGGLE:
      return d->cap == DZ_CAP_ONOFF || d->cap == DZ_CAP_DIMMER || d->cap == DZ_CAP_LOCK;
    case DZ_CMD_SET_LEVEL: return d->cap == DZ_CAP_DIMMER;
    case DZ_CMD_BLINDS_OPEN: case DZ_CMD_BLINDS_CLOSE: case DZ_CMD_BLINDS_STOP: return d->cap == DZ_CAP_BLINDS;
    case DZ_CMD_SELECTOR: return d->cap == DZ_CAP_SELECTOR;
    case DZ_CMD_SCENE_ON: return dz_cap_is_scene_ns(d->cap);   /* a scene is pushed; a group is switched on */
    case DZ_CMD_SCENE_OFF: return d->cap == DZ_CAP_GROUP;       /* a Scene has no Off: never sent */
    case DZ_CMD_SETPOINT: return d->cap == DZ_CAP_THERMOSTAT && setpoint_celsius(d, u);
    case DZ_CMD_PUSH: return d->cap == DZ_CAP_PUSH;
    case DZ_CMD_MODE: return false; /* the MCP endpoint has no thermostat-mode tool: shown read-only */
  }
  return false;
}

bool dz_cmd_supported(const dz_device_t *d, dz_cmd_kind_t kind, const dz_units_t *u) { return d && cap_allows(d, kind, u); }

/* Commands the capability would take but MCP cannot carry (decided before the generic NOT_CONTROLLABLE). */
static bool unsupported(const dz_device_t *d, dz_cmd_kind_t k, const dz_units_t *u) {
  if (k == DZ_CMD_MODE) return d->cap == DZ_CAP_MODE;
  if (k == DZ_CMD_SETPOINT && d->cap == DZ_CAP_THERMOSTAT) return !setpoint_celsius(d, u); /* Fahrenheit or unknown units */
  return false;
}

dz_risk_t dz_cmd_risk(const dz_device_t *d, dz_cmd_kind_t kind, const dz_units_t *u) {
  if (d->is_protected || !d->controllable || !cap_allows(d, kind, u)) return DZ_RISK_LOCKED;
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
  dz_units_t units;
  dz_model_units(c->model, &units);
  if (unsupported(&d, req->kind, &units)) return DZ_CMDE_UNSUPPORTED;
  if (!d.controllable || !cap_allows(&d, req->kind, &units)) return DZ_CMDE_NOT_CONTROLLABLE;

  int level = -1;
  float setpoint = req->setpoint;
  if (req->kind == DZ_CMD_SET_LEVEL) {
    if (d.max_level > 100) return DZ_CMDE_BAD_ARG; /* the MCP level tool takes 0..100 only */
    level = req->level < 0 ? 0 : req->level > d.max_level ? d.max_level : req->level;
  } else if (req->kind == DZ_CMD_SELECTOR) {
    int cnt = selector_count(&d);
    if (req->level < 0 || req->level % 10 != 0 || (cnt ? req->level / 10 >= cnt : req->level > d.max_level))
      return DZ_CMDE_BAD_ARG;
    if (req->level > 100) return DZ_CMDE_BAD_ARG;
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
  if (dz_cmd_risk(&d, req->kind, &units) == DZ_RISK_CONFIRM && !req->confirmed) return DZ_CMDE_NEEDS_CONFIRM;
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

/* The MCP tools/call body for a command (JSON-RPC 2.0). Only integers, fixed words and fmt_setpoint are printed, so no JSON
 * escaping is needed (no user text is ever sent). Lock type 20 gets the inverted word, as the legacy path did. Returns the
 * length, or 0 when the command has no MCP tool (MODE), a number does not fit, or the buffer is too small. */
size_t dz_mcp_encode(char *out, size_t cap, uint32_t rpc_id, dz_cmd_kind_t kind, const char *idx, const dz_device_t *pre,
                     int level, float setpoint) {
  const char *tool = NULL, *word = NULL;
  char args[96], num[24];
  if (!idx || !*idx || strlen(idx) > 9 || !pre) return 0;
  for (const char *p = idx; *p; p++) if (*p < '0' || *p > '9') return 0;
  unsigned long id = strtoul(idx, NULL, 10);
  bool lock20 = pre->cap == DZ_CAP_LOCK && pre->switch_type == 20;
  int n;
  switch (kind) {
    case DZ_CMD_ON: tool = "set_switch_state"; word = lock20 ? "Off" : "On"; break;
    case DZ_CMD_OFF: tool = "set_switch_state"; word = lock20 ? "On" : "Off"; break;
    case DZ_CMD_PUSH: tool = "set_switch_state"; word = pre->switch_type == 10 ? "Off" : "On"; break;
    case DZ_CMD_TOGGLE: tool = "toggle_switch_state"; break;
    case DZ_CMD_SET_LEVEL: case DZ_CMD_SELECTOR: tool = "set_dimmer_level"; if (level < 0 || level > 100) return 0; break;
    case DZ_CMD_BLINDS_OPEN: tool = "control_blinds"; word = "Open"; break;
    case DZ_CMD_BLINDS_CLOSE: tool = "control_blinds"; word = "Close"; break;
    case DZ_CMD_BLINDS_STOP: tool = "control_blinds"; word = "Stop"; break;
    case DZ_CMD_SCENE_ON: tool = "switch_scene"; word = "On"; break;
    case DZ_CMD_SCENE_OFF: tool = "switch_scene"; word = "Off"; break;
    case DZ_CMD_SETPOINT: tool = "set_setpoint_value"; break;
    case DZ_CMD_MODE: return 0;
  }
  switch (kind) {
    case DZ_CMD_ON: case DZ_CMD_OFF: case DZ_CMD_PUSH: n = snprintf(args, sizeof args, "{\"idx\":%lu,\"state\":\"%s\"}", id, word); break;
    case DZ_CMD_TOGGLE: n = snprintf(args, sizeof args, "{\"idx\":%lu}", id); break;
    case DZ_CMD_SET_LEVEL: case DZ_CMD_SELECTOR: n = snprintf(args, sizeof args, "{\"idx\":%lu,\"level\":%d}", id, level); break;
    case DZ_CMD_BLINDS_OPEN: case DZ_CMD_BLINDS_CLOSE: case DZ_CMD_BLINDS_STOP:
      n = snprintf(args, sizeof args, "{\"idx\":%lu,\"command\":\"%s\"}", id, word); break;
    case DZ_CMD_SCENE_ON: case DZ_CMD_SCENE_OFF: /* scene_id is a JSON NUMBER, never a string */
      n = snprintf(args, sizeof args, "{\"scene_id\":%lu,\"command\":\"%s\"}", id, word); break;
    default: /* SETPOINT */
      fmt_setpoint(num, sizeof num, setpoint);
      n = snprintf(args, sizeof args, "{\"idx\":%lu,\"setpoint\":%s}", id, num); break;
  }
  if (n <= 0 || (size_t)n >= sizeof args) return 0;
  int m = snprintf(out, cap, "{\"jsonrpc\":\"2.0\",\"id\":%lu,\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
                   (unsigned long)rpc_id, tool, args);
  return m > 0 && (size_t)m < cap ? (size_t)m : 0;
}

/* Did the re-read show the intended state? */
static bool matches(const dz_cmd_slot_t *s, const dz_device_t *a) {
  const dz_device_t *p = &s->pre;
  switch (s->req.kind) {
    case DZ_CMD_ON: case DZ_CMD_OFF: case DZ_CMD_TOGGLE: {
      bool tog = s->req.kind == DZ_CMD_TOGGLE;
      if (p->cap == DZ_CAP_LOCK) {
        /* ON always means "lock", OFF "unlock" (type 20 locks get the inverted word in dz_mcp_encode). */
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
    /* Blinds: Domoticz applies inversion (ReverseState / ReversePosition, the old types 6 and 16) on the server and reports the
     * user-facing state, so Open is "Open" or, on percentage blinds that report "Set Level: N %", level 100; Close is "Closed"
     * or level 0. Not verified against a live inverted blind. */
    case DZ_CMD_BLINDS_OPEN: return !strncmp(a->status, "Open", 4) || (a->has_level && a->level >= 100 && !strncmp(a->status, "Set Level", 9));
    case DZ_CMD_BLINDS_CLOSE: return !strncmp(a->status, "Closed", 6) || (a->has_level && a->level <= 0 && !strncmp(a->status, "Set Level", 9));
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

/* One re-read of the device (or scene list) the command is about. Returns the failure to report, DZ_FAIL_NONE when the
 * request worked; *got says whether the device was in the answer. Updates the model like a poll would. */
static dz_fail_t read_one(dz_cmd_t *c, dz_cmd_slot_t *s, uint32_t now, dz_device_t *out, bool *got) {
  char q[64];
  bool scene = is_scene_cmd(s->req.kind);
  if (scene) snprintf(q, sizeof q, "param=getscenes");
  else snprintf(q, sizeof q, "param=getdevices&rid=%s", s->idx);
  dz_http_result_t hr;
  dz_net_err_t e = dz_client_request(c->client, q, &hr);
  grab_t g;
  memset(&g, 0, sizeof g);
  g.idx = s->idx;
  g.scene = scene;
  *got = false;
  if (e == DZ_NET_CONNECT || e == DZ_NET_TLS || e == DZ_NET_TIMEOUT) return DZ_FAIL_TRANSPORT;
  if (e != DZ_NET_OK) return DZ_FAIL_BAD_RESPONSE;
  if (hr.status == 401) { dz_client_note_auth_failure(c->client); return DZ_FAIL_UNAUTHORIZED; }
  if (hr.status == 403) return DZ_FAIL_FORBIDDEN;
  if (hr.status >= 500) return DZ_FAIL_SERVER;
  if (hr.status != 200) return DZ_FAIL_BAD_RESPONSE;
  dz_result_t res;
  dz_err_t pe = dz_parse_response(c->client->cfg.buf, hr.len, grab_cb, &g, &res);
  if (pe == DZ_ERR_STATUS) return DZ_FAIL_SERVER;
  if (pe != DZ_OK) return DZ_FAIL_BAD_RESPONSE;
  if (g.found) {
    *got = true;
    *out = g.got;
    dz_model_begin(c->model);
    dz_model_stage_cb(&g.got, c->model);
    dz_model_commit(c->model, DZ_COMMIT_PARTIAL, NULL, now);
  }
  return DZ_FAIL_NONE;
}

/* The MCP endpoint skips Domoticz's Protected check and its per-user device check, so this is the guard: one small GET right
 * before the call (it also refreshes the pre-state: lock type, push type, status, LastUpdate). */
static void do_preflight(dz_cmd_t *c, dz_cmd_slot_t *s, uint32_t now) {
  dz_device_t fresh;
  bool got;
  dz_fail_t f = read_one(c, s, now, &fresh, &got);
  if (f != DZ_FAIL_NONE) { s->fail = f; finish(c, s, DZ_OUT_FAILED, NULL); return; }
  if (!got) { s->fail = DZ_FAIL_REJECTED; finish(c, s, DZ_OUT_FAILED, NULL); return; } /* gone since the last poll */
  if (fresh.is_protected) { s->fail = DZ_FAIL_PROTECTED; finish(c, s, DZ_OUT_FAILED, &fresh); return; }
  dz_units_t units;
  dz_model_units(c->model, &units);
  if (!fresh.controllable || !cap_allows(&fresh, s->req.kind, &units)) { s->fail = DZ_FAIL_REJECTED; finish(c, s, DZ_OUT_FAILED, &fresh); return; }
  s->pre = fresh;
  s->state = 3;
  s->due_ms = now;
}

/* JSON-RPC error code -> failure. -32001 / -32600 only reach here after the one retry (BAD_RESPONSE). */
static dz_fail_t rpc_error_fail(int code) {
  switch (code) {
    case -32601: return DZ_FAIL_NO_MCP;
    case -32002: return DZ_FAIL_FORBIDDEN;
    case -32000: case -32003: case -32004: case -32603: return DZ_FAIL_SERVER;
    case -32602: return DZ_FAIL_REJECTED;
    default: return DZ_FAIL_BAD_RESPONSE; /* -32700, -32600, -32001 and anything unknown */
  }
}

static dz_fail_t http_fail(int status) {
  if (status == 401) return DZ_FAIL_UNAUTHORIZED;
  if (status == 403) return DZ_FAIL_FORBIDDEN;
  if (status == 404 || status == 405 || status == 503) return DZ_FAIL_NO_MCP;
  if (status >= 500) return DZ_FAIL_SERVER;
  return DZ_FAIL_BAD_RESPONSE;
}

static void do_send(dz_cmd_t *c, dz_cmd_slot_t *s, uint32_t now) {
  dz_client_t *cl = c->client;
  if (!dz_mcp_encode(cl->mcp_call, sizeof cl->mcp_call, dz_client_mcp_next_id(cl), s->req.kind, s->idx, &s->pre, s->level,
                     s->req.setpoint)) {
    s->fail = DZ_FAIL_BAD_RESPONSE;
    finish(c, s, DZ_OUT_FAILED, NULL);
    return;
  }
  dz_mcp_result_t r;
  bool ok = dz_client_mcp_call(cl, now, cl->mcp_call, &r);
  bool reply = r.net == DZ_NET_OK && r.http == 200 && r.parse == DZ_OK; /* a JSON-RPC answer, even if the call failed */
  if (!ok && !reply) {
    bool at_call = r.stage == DZ_MCP_STAGE_CALL;
    if (at_call && (r.net == DZ_NET_TIMEOUT || r.net == DZ_NET_CONNECT || r.net == DZ_NET_TLS ||
                    (r.net == DZ_NET_TOO_LARGE && r.http == 200))) {
      /* The request may have reached the server (a reset or TLS error after the send, an answer too large for the buffer): the
       * call may have run and is NEVER retried; the re-read decides. */
      s->uncertain = true;
    } else if (r.net == DZ_NET_TIMEOUT || r.net == DZ_NET_CONNECT || r.net == DZ_NET_TLS) {
      s->fail = DZ_FAIL_TRANSPORT; /* includes a timeout in initialize: nothing was sent yet */
      finish(c, s, DZ_OUT_FAILED, NULL);
      return;
    } else {
      s->fail = r.net == DZ_NET_OK ? http_fail(r.http) : DZ_FAIL_BAD_RESPONSE;
      if (s->fail == DZ_FAIL_UNAUTHORIZED) dz_client_note_auth_failure(cl);
      finish(c, s, DZ_OUT_FAILED, NULL);
      return;
    }
  } else if (r.session_lost) {
    s->fail = DZ_FAIL_BAD_RESPONSE; /* still no session after one re-initialize */
    finish(c, s, DZ_OUT_FAILED, NULL);
    return;
  } else if (r.reply.has_error) {
    s->fail = rpc_error_fail(r.reply.code);
    finish(c, s, DZ_OUT_FAILED, NULL);
    return;
  } else if (!r.reply.has_result) {
    s->fail = DZ_FAIL_BAD_RESPONSE;
    finish(c, s, DZ_OUT_FAILED, NULL);
    return;
  } else if (r.reply.is_error) {
    s->fail = DZ_FAIL_REJECTED; /* device / scene not found, bad arguments */
    finish(c, s, DZ_OUT_FAILED, NULL);
    return;
  } else if (!strncmp(r.reply.text, "Error ", 6)) {
    s->fail = DZ_FAIL_SERVER; /* the switch itself failed: isError false, text starts with Error */
    finish(c, s, DZ_OUT_FAILED, NULL);
    return;
  } else {
    /* anything else, including "... was already X. No action taken.": the re-read is the truth */
    s->no_action = strstr(r.reply.text, "No action taken") != NULL;
    s->sent_ok = true;
  }
  s->state = 2;
  s->due_ms = now + c->verify_delay_ms;
}

static void do_verify(dz_cmd_t *c, dz_cmd_slot_t *s, uint32_t now) {
  dz_device_t after;
  bool got = false;
  dz_fail_t f = read_one(c, s, now, &after, &got);
  s->reads++;
  if (got && f == DZ_FAIL_NONE && matches(s, &after)) { finish(c, s, DZ_OUT_CONFIRMED, &after); return; }
  if (s->reads >= 2) { finish(c, s, DZ_OUT_UNCONFIRMED, got ? &after : NULL); return; }
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
  if (best->state == 1) do_preflight(c, best, now);
  else if (best->state == 3) do_send(c, best, now);
  else do_verify(c, best, now);
  return true;
}

bool dz_cmd_scene_tap_kind(const dz_device_t *d, dz_cmd_kind_t *kind) {
  if (!d || !kind || !dz_cap_is_scene_ns(d->cap)) return false;
  if (d->cap == DZ_CAP_SCENE) { *kind = DZ_CMD_SCENE_ON; return true; }
  *kind = dz_device_state(d) == DZ_STATE_ON ? DZ_CMD_SCENE_OFF : DZ_CMD_SCENE_ON;
  return true;
}
