#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dz_client.h"
#include "dz_cmd.h"
#include "dz_fake.h"
#include "mini_test.h"

/* A tiny Domoticz simulator: just enough state to exercise read-after-write. */
typedef struct {
  const char *idx, *type, *subtype, *name;
  int stv, level, maxlvl, protectedf, scene;
  char status[24], data[24], lu[24], names[48];
  float setpoint;
} sim_t;

static sim_t S[] = {
  {"1", "Light/Switch", "Switch", "lamp", 0, 0, 100, 0, 0, "Off", "Off", "2026-10-31 10:00:00", "", 0},
  {"2", "Light/Switch", "Switch", "dim", 7, 29, 100, 0, 0, "Off", "Off", "2026-10-31 10:00:00", "", 0},
  {"3", "Light/Switch", "Switch", "blind", 3, 0, 100, 0, 0, "Open", "Open", "2026-10-31 10:00:00", "", 0},
  {"4", "Light/Switch", "Selector Switch", "fan", 18, 10, 100, 0, 0, "", "Low", "2026-10-31 10:00:00", "T2ZmfExvd3xNZWR8SGlnaA==", 0},
  {"5", "Light/Switch", "Switch", "push", 9, 0, 100, 0, 0, "", "On", "2026-10-31 10:00:00", "", 0},
  {"6", "Light/Switch", "Switch", "lock", 19, 0, 100, 0, 0, "Locked", "Locked", "2026-10-31 10:00:00", "", 0},
  {"7", "Light/Switch", "Switch", "protected", 0, 0, 100, 1, 0, "Off", "Off", "2026-10-31 10:00:00", "", 0},
  {"8", "Light/Switch", "Switch", "lock2", 20, 0, 100, 0, 0, "Unlocked", "Unlocked", "2026-10-31 10:00:00", "", 0},
  {"9", "Setpoint", "SetPoint", "therm", -1, 0, 100, 0, 0, "", "20.0", "2026-10-31 10:00:00", "", 20.0f},
  {"10", "Temp", "x", "temp", -1, 0, 100, 0, 0, "", "20.0 C", "2026-10-31 10:00:00", "", 0},
  {"11", "Light/Switch", "Switch", "dim15", 7, 0, 15, 0, 0, "Off", "Off", "2026-10-31 10:00:00", "", 0},
  {"12", "Light/Switch", "Switch", "iblind6", 6, 0, 100, 0, 0, "Open", "Open", "2026-10-31 10:00:00", "", 0},
  {"13", "Light/Switch", "Switch", "iblind16", 16, 0, 100, 0, 0, "Open", "Open", "2026-10-31 10:00:00", "", 0},
  {"14", "Light/Switch", "Switch", "pblind", 13, 30, 100, 0, 0, "Set Level: 30 %", "Set Level: 30 %", "2026-10-31 10:00:00", "", 0},
  {"15", "Light/Switch", "Switch", "pushoff", 10, 0, 100, 0, 0, "", "Off", "2026-10-31 10:00:00", "", 0},
  {"16", "Light/Switch", "Switch", "dim255", 7, 0, 255, 0, 0, "Off", "Off", "2026-10-31 10:00:00", "", 0},
  {"17", "Thermostat", "Thermostat Mode", "modedev", -1, 0, 100, 0, 0, "", "Heat", "2026-10-31 10:00:00", "", 0},
  {"18", "Light/Switch", "Switch", "dim32", 7, 0, 32, 0, 0, "Off", "Off", "2026-10-31 10:00:00", "", 0},
  {"19", "Light/Switch", "Selector Switch", "sel12", 18, 0, 100, 0, 0, "", "Off", "2026-10-31 10:00:00", "MHwxfDJ8M3w0fDV8Nnw3fDh8OXwxMHwxMQ==", 0},
  {"1", "Scene", NULL, "scene", -1, 0, 100, 0, 1, "Off", "Off", "2026-10-31 10:00:00", "", 0},
  {"2", "Group", NULL, "group", -1, 0, 100, 0, 1, "Off", "Off", "2026-10-31 10:00:00", "", 0},
};
#define NS (sizeof S / sizeof S[0])

typedef enum { X_OK, X_IGNORE, X_401, X_403, X_500, X_ERR, X_HTML, X_CONNECT, X_TIMEOUT_APPLIED, X_TIMEOUT_IGNORED, X_VERIFY_FAIL,
               X_ISERROR, X_NOACTION, X_NOMCP503, X_NOMCP404, X_RPCERR, X_EXPIRE_ONCE, X_EXPIRED_NEXT, X_EXPIRE_ALWAYS, X_INIT_TIMEOUT,
               X_PREFLIGHT_404, X_TLS, X_TLS_APPLIED, X_TOOLARGE, X_TOOLARGE_APPLIED, X_INIT_CONNECT, X_INIT_TLS, X_INIT_TOOLARGE } xmode_t;
static xmode_t xm;
static int ticks; /* each effective command bumps the fake LastUpdate */
static int cmd_calls, read_calls;

static void render(char *out, size_t cap, bool scenes, const char *rid) {
  size_t n = (size_t)snprintf(out, cap, "{\"ServerTime\":\"2026-10-31 10:00:10\",\"result\":[");
  int first = 1;
  for (size_t i = 0; i < NS; i++) {
    sim_t *s = &S[i];
    if (s->scene != scenes) continue;
    if (rid && strcmp(rid, s->idx)) continue;
    n += (size_t)snprintf(out + n, cap - n,
        "%s{\"idx\":\"%s\",\"Name\":\"%s\",\"Type\":\"%s\",\"Status\":\"%s\",\"Data\":\"%s\",\"LastUpdate\":\"%s\",\"Protected\":%s,\"Favorite\":1,\"HaveTimeout\":false",
        first ? "" : ",", s->idx, s->name, s->type, s->status, s->data, s->lu, s->protectedf ? "true" : "false");
    first = 0;
    if (s->subtype) n += (size_t)snprintf(out + n, cap - n, ",\"SubType\":\"%s\"", s->subtype);
    if (s->stv >= 0) n += (size_t)snprintf(out + n, cap - n, ",\"SwitchTypeVal\":%d,\"Level\":%d,\"LevelInt\":%d,\"MaxDimLevel\":%d", s->stv, s->level, s->level, s->maxlvl);
    if (s->names[0]) n += (size_t)snprintf(out + n, cap - n, ",\"LevelNames\":\"%s\"", s->names);
    if (!strcmp(s->type, "Setpoint")) n += (size_t)snprintf(out + n, cap - n, ",\"SetPoint\":\"%.1f\",\"min\":-20,\"max\":60,\"step\":0.5", (double)s->setpoint);
    if (!strcmp(s->name, "modedev")) n += (size_t)snprintf(out + n, cap - n, ",\"Modes\":\"0;Off;1;Heat;\",\"Mode\":1");
    n += (size_t)snprintf(out + n, cap - n, "}");
  }
  snprintf(out + n, cap - n, "],\"status\":\"OK\",\"title\":\"Devices\"}");
}

static sim_t *find(const char *idx, bool scene) {
  for (size_t i = 0; i < NS; i++) if (!strcmp(S[i].idx, idx) && S[i].scene == scene) return &S[i];
  return NULL;
}
static void touch(sim_t *s) { snprintf(s->lu, sizeof s->lu, "2026-10-31 10:%02d:%02d", ticks / 60 % 60, ticks % 60); ticks++; }

static bool group_mixed; /* simulate a group whose members do not all switch */

/* the tools/call body -> the effect on the simulated server (what Domoticz's SwitchLight does behind the MCP tools) */
static bool jnum(const char *body, const char *key, char *out, size_t cap) {
  char pat[40];
  snprintf(pat, sizeof pat, "\"%s\":", key);
  const char *p = strstr(body, pat);
  if (!p) return false;
  p += strlen(pat);
  size_t n = strspn(p, "-0123456789.");
  if (!n || n >= cap) return false;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}
static bool jstr(const char *body, const char *key, char *out, size_t cap) {
  char pat[40];
  snprintf(pat, sizeof pat, "\"%s\":\"", key);
  const char *p = strstr(body, pat);
  if (!p) return false;
  p += strlen(pat);
  size_t n = strcspn(p, "\"");
  if (n >= cap) return false;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

static int applied; /* effective changes made by tools/call */
static bool srv_round_up; /* the server converts a percentage to its own units rounding up */
static bool pblind_levels; /* percentage blinds report Set Level: N % instead of Open / Closed */
static void apply_call(const char *body) {
  char tool[40], idx[16], cmd[32], lvl[16], sp[16];
  if (!jstr(body, "name", tool, sizeof tool)) return;
  bool scene = !strcmp(tool, "switch_scene");
  if (!jnum(body, scene ? "scene_id" : "idx", idx, sizeof idx)) return;
  sim_t *s = find(idx, scene);
  if (!s) return;
  applied++;
  if (!strcmp(tool, "set_setpoint_value")) { if (jnum(body, "setpoint", sp, sizeof sp)) { s->setpoint = strtof(sp, NULL); touch(s); } return; }
  if (!strcmp(tool, "set_dimmer_level") && jnum(body, "level", lvl, sizeof lvl)) {
    int n = atoi(lvl);
    s->level = (s->stv == 7 || s->stv == 5) ? (n * s->maxlvl + (srv_round_up ? 99 : 50)) / 100 : n; /* the tool takes a percentage */
    if (s->stv == 18) { snprintf(s->data, sizeof s->data, "L%d", n); }
    else if (s->stv == 13) snprintf(s->status, sizeof s->status, n >= 100 ? "Open" : n ? "Set Level: %d %%" : "Closed", n);
    else snprintf(s->status, sizeof s->status, n ? "Set Level: %d %%" : "Off", n);
    touch(s);
    return;
  }
  if (!strcmp(tool, "toggle_switch_state")) strcpy(cmd, (!strcmp(s->status, "Off") || !strcmp(s->status, "Unlocked")) ? "On" : "Off");
  else if (!jstr(body, scene ? "command" : (!strcmp(tool, "control_blinds") ? "command" : "state"), cmd, sizeof cmd)) return;
  if (s->stv == 19) strcpy(s->status, !strcmp(cmd, "On") ? "Locked" : "Unlocked");
  else if (s->stv == 20) strcpy(s->status, !strcmp(cmd, "On") ? "Unlocked" : "Locked");
  else if (s->stv == 13 && pblind_levels && strcmp(cmd, "Stop")) {
    s->level = !strcmp(cmd, "Open") ? 100 : 0;
    snprintf(s->status, sizeof s->status, "Set Level: %d %%", s->level);
  }
  else if (s->stv == 3 || s->stv == 6 || s->stv == 13 || s->stv == 16) { if (strcmp(cmd, "Stop")) strcpy(s->status, !strcmp(cmd, "Open") ? "Open" : "Closed"); }
  else if (s->stv == 9 || s->stv == 10) { strcpy(s->data, cmd); }
  else {
    strcpy(s->status, cmd);
    if (scene && group_mixed && !strcmp(s->idx, "2") && !strcmp(cmd, "On")) strcpy(s->status, "Mixed");
  }
  touch(s);
}

static int rpc_code = -32603; /* for X_RPCERR */
static char bigbody[40000]; /* an answer that does not fit the 32 KB client buffer */
static int mcp_inits, mcp_notifs;
#define MCP_OK(text, err) "{\"id\":9,\"jsonrpc\":\"2.0\",\"result\":{\"content\":[{\"text\":\"" text "\",\"type\":\"text\"}],\"isError\":" err "}}"

static dz_net_err_t srv(fake_t *f, const char *url, char *buf, size_t cap, dz_http_result_t *out) {
  if (strstr(url, "/mcp")) {
    if (strstr(f->body, "\"initialize\"")) {
      mcp_inits++;
      if (xm == X_INIT_TIMEOUT) return DZ_NET_TIMEOUT;
      if (xm == X_INIT_CONNECT) return DZ_NET_CONNECT;
      if (xm == X_INIT_TLS) return DZ_NET_TLS;
      if (xm == X_INIT_TOOLARGE) { memset(bigbody, ' ', sizeof bigbody - 1); return fake_body(bigbody, 200, buf, cap, out); }
      if (xm == X_NOMCP503) return fake_body("MCP disabled", 503, buf, cap, out);
      if (xm == X_NOMCP404) return fake_body("<html>nope</html>", 404, buf, cap, out);
      if (xm == X_401) return fake_body("{\"error\":\"Unauthorized\"}", 401, buf, cap, out);
      if (xm == X_EXPIRE_ONCE) xm = X_EXPIRED_NEXT; /* a new session: the next call works again */
      return fake_body("{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"protocolVersion\":\"2025-06-18\"}}", 200, buf, cap, out);
    }
    if (strstr(f->body, "notifications/initialized")) { mcp_notifs++; return fake_body("<html>Accepted</html>", 202, buf, cap, out); }
    cmd_calls++;
    switch (xm) {
      case X_401: return fake_body("{\"error\":\"Unauthorized\"}", 401, buf, cap, out);
      case X_403: return fake_body("{\"status\":\"ERR\"}", 403, buf, cap, out);
      case X_500: return fake_body("err", 500, buf, cap, out);
      case X_ERR: return fake_body(MCP_OK("Error switching device.", "false"), 200, buf, cap, out);
      case X_ISERROR: return fake_body(MCP_OK("Device not found", "true"), 200, buf, cap, out);
      case X_HTML: return fake_body("<html>proxy</html>", 200, buf, cap, out);
      case X_CONNECT: return DZ_NET_CONNECT;
      case X_TLS: return DZ_NET_TLS;
      case X_TLS_APPLIED: apply_call(f->body); return DZ_NET_TLS;
      case X_TOOLARGE: case X_TOOLARGE_APPLIED:
        if (xm == X_TOOLARGE_APPLIED) apply_call(f->body);
        memset(bigbody, ' ', sizeof bigbody - 1);
        return fake_body(bigbody, 200, buf, cap, out);
      case X_TIMEOUT_APPLIED: apply_call(f->body); return DZ_NET_TIMEOUT;
      case X_TIMEOUT_IGNORED: return DZ_NET_TIMEOUT;
      case X_IGNORE: return fake_body(MCP_OK("Switch set successfully.", "false"), 200, buf, cap, out); /* nothing happens */
      case X_NOACTION: return fake_body(MCP_OK("Switch was already On. No action taken.", "false"), 200, buf, cap, out);
      case X_NOMCP503: return fake_body("MCP disabled", 503, buf, cap, out);
      case X_RPCERR: {
        char e[160];
        snprintf(e, sizeof e, "{\"id\":9,\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"x\"}}", rpc_code);
        return fake_body(e, 200, buf, cap, out);
      }
      case X_EXPIRE_ONCE: case X_EXPIRE_ALWAYS:
        return fake_body("{\"id\":9,\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"Session not found\"}}", 200, buf, cap, out);
      default: apply_call(f->body); return fake_body(MCP_OK("Switch set successfully.", "false"), 200, buf, cap, out);
    }
  }
  read_calls++;
  if (xm == X_VERIFY_FAIL) return DZ_NET_TIMEOUT;
  if (xm == X_PREFLIGHT_404 && read_calls == 1) return fake_body("<html>nope</html>", 404, buf, cap, out);
  char tmp[16384], rid[16];
  const char *p = strstr(url, "&rid=");
  if (p) { size_t n = strcspn(p + 5, "&"); snprintf(rid, sizeof rid, "%.*s", (int)n, p + 5); }
  if (strstr(url, "param=getscenes")) render(tmp, sizeof tmp, true, NULL);
  else render(tmp, sizeof tmp, false, p ? rid : NULL);
  return fake_body(tmp, 200, buf, cap, out);
}

#define CAP 32
static dz_entry_t ent[CAP];
static dz_device_t stg[CAP];
static dz_model_t M;
static char rbuf[32768];
static fake_t F;
static dz_client_t C;
static dz_cmd_t K;
static dz_cmd_result_t R[16];
static int nres;
static void on_res(const dz_cmd_result_t *r, void *u) { (void)u; if (nres < 16) R[nres++] = *r; }

static void reset_sim(void) {
  static sim_t init[sizeof S / sizeof S[0]];
  static bool have;
  if (!have) { memcpy(init, S, sizeof S); have = true; }
  memcpy(S, init, sizeof S); /* every test starts from the table above */
  group_mixed = false;
  srv_round_up = false;
  ticks = 5; /* the first touch must differ from the table's 10:00:00 */
}

static uint32_t T;
/* NULL: units unknown (getconfig never answered) */
static void set_units(const char *temp) {
  dz_units_t u;
  memset(&u, 0, sizeof u);
  if (temp) { u.have_temp = true; snprintf(u.temp, sizeof u.temp, "%s", temp); }
  dz_model_set_units(&M, &u);
}
static void setup(bool allow) {
  if (M.mu) dz_model_deinit(&M);
  dz_model_cfg_t mc = {.ttl_ms = 600000, .old_update_s = UINT32_MAX};
  dz_model_init(&M, ent, CAP, stg, CAP, &mc);
  fake_init(&F, srv, NULL);
  F.secret = "TOK-SECRET";
  F.sid_script = "0123456789abcdef0123456789abcdef";
  mcp_inits = mcp_notifs = applied = 0;
  xm = X_OK;
  cmd_calls = read_calls = nres = 0;
  reset_sim();
  dz_client_cfg_t cfg = {.base_url = "http://h", .token = "TOK-SECRET", .transport = &F.base, .model = &M, .buf = rbuf, .buf_cap = sizeof rbuf};
  CHECK(dz_client_init(&C, &cfg));
  char tmp[16384];
  render(tmp, sizeof tmp, false, NULL);
  CHECK(dz_model_apply(&M, tmp, strlen(tmp), DZ_COMMIT_FULL_DEVICES, 1000, NULL, NULL) == DZ_OK);
  render(tmp, sizeof tmp, true, NULL);
  CHECK(dz_model_apply(&M, tmp, strlen(tmp), DZ_COMMIT_FULL_SCENES, 1000, NULL, NULL) == DZ_OK);
  dz_cmd_init(&K, &C, &M, allow, on_res, NULL);
  set_units("C");
  T = 2000;
}

/* submit, then run the pump on a simulated clock until the command is done. */
static dz_cmd_err_t run(dz_cmd_kind_t kind, const char *idx, int level, float sp, bool confirmed, uint32_t *id) {
  dz_cmd_req_t q = {.kind = kind, .idx = idx, .level = level, .setpoint = sp, .confirmed = confirmed};
  dz_cmd_err_t e = dz_cmd_submit(&K, &q, T, id);
  for (int i = 0; e == DZ_CMD_ACCEPTED && i < 20 && dz_cmd_pending(&K); i++) {
    uint32_t w = dz_cmd_ms_until_due(&K, T);
    if (w == UINT32_MAX) break;
    T += w;
    dz_cmd_pump(&K, T);
  }
  return e;
}
#define LAST (R[nres - 1])
#define LASTB (F.bodies[(F.posts - 1) % 16]) /* the last POST: the tools/call body */

static void test_basic_confirm(void) {
  setup(true);
  uint32_t id1 = 0, id2 = 0;
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, &id1) == DZ_CMD_ACCEPTED);
  CHECK(nres == 1 && LAST.outcome == DZ_OUT_CONFIRMED && LAST.id == id1 && LAST.have_after && !strcmp(LAST.after.status, "On"));
  CHECK(cmd_calls == 1 && read_calls == 2 && strstr(fake_last_url(&F), "getdevices&rid=1")); /* pre-flight read, call, verify read */
  CHECK(mcp_inits == 1 && mcp_notifs == 1 && strstr(LASTB, "\"name\":\"set_switch_state\"") && strstr(LASTB, "\"idx\":1,\"state\":\"On\""));
  CHECK(strstr(F.urls[0], "getdevices&rid=1") && !strstr(F.urls[0], "switch")); /* the first request is the pre-flight GET */
  CHECK(F.had_auth && !F.auth_in_url);
  dz_device_t d; ru_val_t v; ru_val_state_t st;
  CHECK(dz_model_get(&M, "1", false, &d, &v, &st, T) && !strcmp(d.status, "On")); /* model learned from the re-read */
  CHECK(run(DZ_CMD_OFF, "1", 0, 0, false, &id2) == DZ_CMD_ACCEPTED && id2 == id1 + 1 && LAST.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_TOGGLE, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && !strcmp(LAST.after.status, "On"));
  CHECK(run(DZ_CMD_TOGGLE, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && !strcmp(LAST.after.status, "Off"));
}

static void test_timing(void) {
  setup(true);
  dz_cmd_req_t q = {.kind = DZ_CMD_ON, .idx = "1"};
  CHECK(dz_cmd_submit(&K, &q, T, NULL) == DZ_CMD_ACCEPTED);
  CHECK(dz_cmd_pump(&K, T) && cmd_calls == 0 && read_calls == 1);   /* pre-flight read */
  CHECK(dz_cmd_pump(&K, T) && cmd_calls == 1 && read_calls == 1);   /* the MCP call */
  CHECK(!dz_cmd_pump(&K, T + 1499) && nres == 0);       /* the re-read waits */
  CHECK(dz_cmd_pump(&K, T + 1500) && nres == 1 && read_calls == 2);
  /* ignored command: HTTP OK, no effect: two reads, 1.5 s apart, then UNCONFIRMED */
  setup(true);
  xm = X_IGNORE;
  uint32_t t0 = T;
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED);
  CHECK(nres == 1 && LAST.outcome == DZ_OUT_UNCONFIRMED && read_calls == 3 && T - t0 == 3000); /* pre-flight + two verify reads */
  CHECK(LAST.fail == DZ_FAIL_NONE && LAST.have_after && !strcmp(LAST.after.status, "Off"));
}

static void test_levels(void) {
  setup(true);
  CHECK(run(DZ_CMD_SET_LEVEL, "2", 150, 0, false, NULL) == DZ_CMD_ACCEPTED);
  CHECK(strstr(LASTB, "\"name\":\"set_dimmer_level\"") && strstr(LASTB, "\"level\":100") && LAST.level_sent == 100);
  CHECK(LAST.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_SET_LEVEL, "2", -5, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.level_sent == 0 && LAST.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_SET_LEVEL, "2", 40, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && LAST.after.level == 40);
  CHECK(run(DZ_CMD_SET_LEVEL, "11", 99, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.level_sent == 15); /* MaxDimLevel 15 */
  CHECK(run(DZ_CMD_SET_LEVEL, "1", 5, 0, false, NULL) == DZ_CMDE_NOT_CONTROLLABLE);                   /* on/off cannot dim */
  /* selector: valid level, then bad ones */
  CHECK(run(DZ_CMD_SELECTOR, "4", 30, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && LAST.after.level == 30);
  CHECK(run(DZ_CMD_SELECTOR, "4", 35, 0, false, NULL) == DZ_CMDE_BAD_ARG);
  CHECK(run(DZ_CMD_SELECTOR, "4", 40, 0, false, NULL) == DZ_CMDE_BAD_ARG);   /* only 4 names: 0..30 */
  CHECK(run(DZ_CMD_SELECTOR, "4", -10, 0, false, NULL) == DZ_CMDE_BAD_ARG);
  CHECK(run(DZ_CMD_SELECTOR, "2", 10, 0, false, NULL) == DZ_CMDE_NOT_CONTROLLABLE);
}

/* B3: dimmers with MaxDimLevel other than 100: the tool takes a percentage, the verify compares in percent */
static void test_level_percent(void) {
  setup(true);
  CHECK(run(DZ_CMD_SET_LEVEL, "11", 7, 0, false, NULL) == DZ_CMD_ACCEPTED);            /* max 15: 7/15 = 46.7 -> 47 % */
  CHECK(strstr(LASTB, "\"level\":47") && LAST.level_sent == 7 && LAST.outcome == DZ_OUT_CONFIRMED && S[10].level == 7);
  CHECK(run(DZ_CMD_SET_LEVEL, "11", 15, 0, false, NULL) == DZ_CMD_ACCEPTED && strstr(LASTB, "\"level\":100") && LAST.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_SET_LEVEL, "11", 1, 0, false, NULL) == DZ_CMD_ACCEPTED && strstr(LASTB, "\"level\":7}") && LAST.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_SET_LEVEL, "11", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && strstr(LASTB, "\"level\":0") && LAST.outcome == DZ_OUT_CONFIRMED);
  srv_round_up = true; /* one device step of difference is accepted */
  CHECK(run(DZ_CMD_SET_LEVEL, "11", 7, 0, false, NULL) == DZ_CMD_ACCEPTED && S[10].level == 8 && LAST.outcome == DZ_OUT_CONFIRMED);
  srv_round_up = false;
  CHECK(run(DZ_CMD_SET_LEVEL, "18", 16, 0, false, NULL) == DZ_CMD_ACCEPTED && strstr(LASTB, "\"level\":50") && S[17].level == 16 && LAST.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_SET_LEVEL, "18", 99, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.level_sent == 32 && strstr(LASTB, "\"level\":100"));
  CHECK(run(DZ_CMD_SET_LEVEL, "2", 33, 0, false, NULL) == DZ_CMD_ACCEPTED && strstr(LASTB, "\"level\":33") && LAST.outcome == DZ_OUT_CONFIRMED); /* max 100: as is */
  /* a wrong result is not accepted: the server ignored the call */
  xm = X_IGNORE;
  CHECK(run(DZ_CMD_SET_LEVEL, "11", 3, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_UNCONFIRMED);
  xm = X_OK;
  /* max_level above 100 (or 0) is refused; the pre-flight refuses a device whose range changed meanwhile */
  CHECK(run(DZ_CMD_SET_LEVEL, "16", 50, 0, false, NULL) == DZ_CMDE_BAD_ARG);
  dz_cmd_req_t q = {.kind = DZ_CMD_SET_LEVEL, .idx = "11", .level = 5};
  CHECK(dz_cmd_submit(&K, &q, T, NULL) == DZ_CMD_ACCEPTED);
  S[10].maxlvl = 255;
  nres = 0; cmd_calls = 0;
  CHECK(dz_cmd_pump(&K, T) && nres == 1 && LAST.fail == DZ_FAIL_REJECTED && cmd_calls == 0);
  /* selectors: LevelInt as is, multiples of 10, nothing above 100 */
  CHECK(run(DZ_CMD_SELECTOR, "19", 100, 0, false, NULL) == DZ_CMD_ACCEPTED && strstr(LASTB, "\"level\":100"));
  CHECK(run(DZ_CMD_SELECTOR, "19", 110, 0, false, NULL) == DZ_CMDE_BAD_ARG);
}

static void test_blinds_push(void) {
  setup(true);
  CHECK(run(DZ_CMD_BLINDS_CLOSE, "3", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
  CHECK(strstr(LASTB, "\"name\":\"control_blinds\"") && strstr(LASTB, "\"command\":\"Close\""));
  CHECK(run(DZ_CMD_BLINDS_OPEN, "3", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_BLINDS_STOP, "3", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED); /* LastUpdate moved */
  CHECK(run(DZ_CMD_ON, "3", 0, 0, false, NULL) == DZ_CMDE_NOT_CONTROLLABLE);   /* blinds take open/close/stop */
  CHECK(run(DZ_CMD_PUSH, "5", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
  setup(true);
  xm = X_IGNORE;
  CHECK(run(DZ_CMD_PUSH, "5", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_UNCONFIRMED);
  CHECK(run(DZ_CMD_BLINDS_STOP, "3", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_UNCONFIRMED);
}

static void test_risk(void) {
  setup(true);
  dz_device_t d;
  ru_val_t v;
  ru_val_state_t st;
  /* lock: needs confirm, no HTTP without it */
  CHECK(run(DZ_CMD_OFF, "6", 0, 0, false, NULL) == DZ_CMDE_NEEDS_CONFIRM && cmd_calls == 0);
  dz_model_get(&M, "6", false, &d, &v, &st, T);
  CHECK(dz_cmd_risk(&d, DZ_CMD_OFF, NULL) == DZ_RISK_CONFIRM);
  CHECK(run(DZ_CMD_OFF, "6", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && !strcmp(LAST.after.status, "Unlocked"));
  CHECK(run(DZ_CMD_ON, "6", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && !strcmp(LAST.after.status, "Locked"));
  CHECK(run(DZ_CMD_TOGGLE, "6", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && !strcmp(LAST.after.status, "Unlocked"));
  /* inverted lock (type 20): On sends Off */
  CHECK(run(DZ_CMD_ON, "8", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && strstr(fake_last_url(&F) , "rid=8") && LAST.outcome == DZ_OUT_CONFIRMED && strstr(LASTB, "\"state\":\"Off\""));
  /* scenes and groups: confirm; Scene has no Off */
  CHECK(run(DZ_CMD_SCENE_ON, "2", 0, 0, false, NULL) == DZ_CMDE_NEEDS_CONFIRM);
  int c0 = cmd_calls;
  CHECK(run(DZ_CMD_SCENE_ON, "2", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && cmd_calls == c0 + 1);
  CHECK(strstr(LASTB, "\"name\":\"switch_scene\"") && strstr(LASTB, "\"scene_id\":2,\"command\":\"On\"") && strstr(fake_last_url(&F), "getscenes"));
  CHECK(run(DZ_CMD_SCENE_OFF, "2", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && !strcmp(LAST.after.status, "Off"));
  CHECK(run(DZ_CMD_SCENE_OFF, "1", 0, 0, true, NULL) == DZ_CMDE_NOT_CONTROLLABLE);
  CHECK(run(DZ_CMD_SCENE_ON, "1", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
  /* scene idx 1 and device idx 1 are different things */
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && strstr(fake_last_url(&F), "getdevices&rid=1"));
  /* setpoint */
  CHECK(run(DZ_CMD_SETPOINT, "9", 0, 21.5f, false, NULL) == DZ_CMDE_NEEDS_CONFIRM);
  CHECK(run(DZ_CMD_SETPOINT, "9", 0, 21.5f, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
  CHECK(strstr(LASTB, "\"name\":\"set_setpoint_value\"") && strstr(LASTB, "\"idx\":9,\"setpoint\":21.5}"));
  CHECK(run(DZ_CMD_SETPOINT, "9", 0, -3.0f, true, NULL) == DZ_CMD_ACCEPTED && strstr(LASTB, "\"setpoint\":-3.0}"));
  CHECK(run(DZ_CMD_SETPOINT, "9", 0, 1e30f, true, NULL) == DZ_CMDE_BAD_ARG);
  CHECK(run(DZ_CMD_SETPOINT, "9", 0, (float)(0.0 / 0.0 * 0 + __builtin_nanf("")), true, NULL) == DZ_CMDE_BAD_ARG);
  CHECK(run(DZ_CMD_SETPOINT, "1", 0, 20.0f, true, NULL) == DZ_CMDE_NOT_CONTROLLABLE);
}

static void test_refusals(void) {
  setup(true);
  int c0;
  CHECK(run(DZ_CMD_ON, "7", 0, 0, true, NULL) == DZ_CMDE_PROTECTED);               /* protected: never controlled */
  dz_device_t d; ru_val_t v; ru_val_state_t st;
  dz_model_get(&M, "7", false, &d, &v, &st, T);
  CHECK(dz_cmd_risk(&d, DZ_CMD_ON, NULL) == DZ_RISK_LOCKED);
  CHECK(run(DZ_CMD_ON, "10", 0, 0, true, NULL) == DZ_CMDE_NOT_CONTROLLABLE);       /* temperature sensor */
  CHECK(run(DZ_CMD_ON, "99", 0, 0, true, NULL) == DZ_CMDE_UNKNOWN_DEVICE);
  CHECK(run(DZ_CMD_ON, "", 0, 0, true, NULL) == DZ_CMDE_BAD_ARG);
  CHECK(run(DZ_CMD_ON, "1&idx=7", 0, 0, true, NULL) == DZ_CMDE_BAD_ARG);           /* no parameter injection */
  CHECK(run(DZ_CMD_ON, "1;x", 0, 0, true, NULL) == DZ_CMDE_BAD_ARG);
  CHECK(run(DZ_CMD_ON, "1234567890123", 0, 0, true, NULL) == DZ_CMDE_BAD_ARG);
  CHECK(dz_cmd_submit(&K, NULL, T, NULL) == DZ_CMDE_BAD_ARG);
  CHECK(cmd_calls == 0 && read_calls == 0 && F.calls == 0);
  /* stale model: refuse rather than guess */
  c0 = F.calls;
  T += 700000;
  CHECK(run(DZ_CMD_ON, "1", 0, 0, true, NULL) == DZ_CMDE_STALE && F.calls == c0);
  /* read-only configuration (viewer token) */
  setup(false);
  CHECK(run(DZ_CMD_ON, "1", 0, 0, true, NULL) == DZ_CMDE_CONTROL_DISABLED && F.calls == 0);
  /* queue full */
  setup(true);
  dz_cmd_req_t q = {.kind = DZ_CMD_ON, .idx = "1"};
  int acc = 0;
  for (int i = 0; i < 12; i++) acc += dz_cmd_submit(&K, &q, T, NULL) == DZ_CMD_ACCEPTED;
  CHECK(acc == DZ_CMD_QUEUE && dz_cmd_submit(&K, &q, T, NULL) == DZ_CMDE_QUEUE_FULL);
  for (int i = 0; i < 100 && dz_cmd_pending(&K); i++) { T += 1500; dz_cmd_pump(&K, T); }
  CHECK(dz_cmd_pending(&K) == 0 && nres == DZ_CMD_QUEUE);
  for (int i = 1; i < nres; i++) CHECK(R[i].id == R[i - 1].id + 1); /* in order */
}

static void test_failures(void) {
  struct { xmode_t x; dz_fail_t f; } cases[] = {
    {X_403, DZ_FAIL_FORBIDDEN}, {X_500, DZ_FAIL_SERVER}, {X_ERR, DZ_FAIL_SERVER}, {X_HTML, DZ_FAIL_BAD_RESPONSE},
    {X_401, DZ_FAIL_UNAUTHORIZED}, {X_INIT_CONNECT, DZ_FAIL_TRANSPORT}, {X_INIT_TLS, DZ_FAIL_TRANSPORT}, {X_INIT_TOOLARGE, DZ_FAIL_BAD_RESPONSE},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    setup(true);
    xm = cases[i].x;
    CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED);
    CHECK(nres == 1 && LAST.outcome == DZ_OUT_FAILED && LAST.fail == cases[i].f && !LAST.have_after);
    CHECK(read_calls == 1); /* only the pre-flight: no point re-reading after a definite failure */
    CHECK((dz_client_state(&C) == DZ_CS_TOKEN_REJECTED) == (cases[i].x == X_401));
    CHECK(dz_cmd_pending(&K) == 0);
  }
  /* timeout: the effect is decided by the re-read, not by the error */
  setup(true);
  xm = X_TIMEOUT_APPLIED;
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
  setup(true);
  xm = X_TIMEOUT_IGNORED;
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_UNCONFIRMED);
  /* B2: a transport error AFTER the request was sent (call stage) is uncertain, never a plain failure and never retried */
  struct { xmode_t x; bool applied; } up[] = {
    {X_CONNECT, false}, {X_TLS, false}, {X_TLS_APPLIED, true}, {X_TOOLARGE, false}, {X_TOOLARGE_APPLIED, true},
  };
  for (size_t i = 0; i < sizeof up / sizeof up[0]; i++) {
    setup(true);
    xm = up[i].x;
    CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && nres == 1);
    CHECK(LAST.fail == DZ_FAIL_NONE && LAST.outcome == (up[i].applied ? DZ_OUT_CONFIRMED : DZ_OUT_UNCONFIRMED));
    CHECK(cmd_calls == 1 && mcp_inits == 1);                                  /* the call was sent once, no re-initialize, no retry */
    CHECK(read_calls == (up[i].applied ? 2 : 3));                            /* pre-flight, then the verify read(s) decide */
    CHECK(dz_client_state(&C) != DZ_CS_TOKEN_REJECTED);
  }
  /* the command went through but the re-read cannot reach the server */
  setup(true);
  struct { int dummy; } unused = {0};
  (void)unused;
  CHECK(dz_cmd_submit(&K, &(dz_cmd_req_t){.kind = DZ_CMD_ON, .idx = "1"}, T, NULL) == DZ_CMD_ACCEPTED);
  dz_cmd_pump(&K, T);
  xm = X_VERIFY_FAIL;
  for (int i = 0; i < 5 && dz_cmd_pending(&K); i++) { T += 1500; dz_cmd_pump(&K, T); }
  CHECK(nres == 1 && LAST.outcome == DZ_OUT_UNCONFIRMED && !LAST.have_after);
}

static void test_mcp_encode(void) {
  char b[300];
  dz_device_t d;
  memset(&d, 0, sizeof d);
  d.cap = DZ_CAP_ONOFF;
  const char *head = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":";
  char want[320];
#define EXP(args_tool, args) (snprintf(want, sizeof want, "%s\"%s\",\"arguments\":%s}}", head, args_tool, args), want)
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_ON, "12", &d, -1, 0) > 0 && !strcmp(b, EXP("set_switch_state", "{\"idx\":12,\"state\":\"On\"}")));
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_OFF, "12", &d, -1, 0) > 0 && !strcmp(b, EXP("set_switch_state", "{\"idx\":12,\"state\":\"Off\"}")));
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_TOGGLE, "1", &d, -1, 0) > 0 && !strcmp(b, EXP("toggle_switch_state", "{\"idx\":1}")));
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_SET_LEVEL, "2", &d, 30, 0) > 0 && !strcmp(b, EXP("set_dimmer_level", "{\"idx\":2,\"level\":30}")));
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_SELECTOR, "4", &d, 20, 0) > 0 && !strcmp(b, EXP("set_dimmer_level", "{\"idx\":4,\"level\":20}")));
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_SET_LEVEL, "2", &d, 101, 0) == 0); /* above the MCP range */
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_BLINDS_OPEN, "3", &d, -1, 0) > 0 && !strcmp(b, EXP("control_blinds", "{\"idx\":3,\"command\":\"Open\"}")));
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_BLINDS_CLOSE, "3", &d, -1, 0) > 0 && strstr(b, "\"command\":\"Close\""));
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_BLINDS_STOP, "3", &d, -1, 0) > 0 && strstr(b, "\"command\":\"Stop\""));
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_SCENE_ON, "12", &d, -1, 0) > 0 && !strcmp(b, EXP("switch_scene", "{\"scene_id\":12,\"command\":\"On\"}"))); /* a number */
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_SCENE_OFF, "12", &d, -1, 0) > 0 && strstr(b, "\"command\":\"Off\""));
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_SETPOINT, "9", &d, -1, 21.5f) > 0 && !strcmp(b, EXP("set_setpoint_value", "{\"idx\":9,\"setpoint\":21.5}")));
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_SETPOINT, "9", &d, -1, -3.0f) > 0 && strstr(b, "\"setpoint\":-3.0}"));
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_MODE, "9", &d, 1, 0) == 0);
  /* lock types: On always means "lock"; type 20 is sent inverted */
  d.cap = DZ_CAP_LOCK; d.switch_type = 19;
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_ON, "6", &d, -1, 0) > 0 && strstr(b, "\"state\":\"On\""));
  d.switch_type = 20;
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_ON, "6", &d, -1, 0) > 0 && strstr(b, "\"state\":\"Off\""));
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_OFF, "6", &d, -1, 0) > 0 && strstr(b, "\"state\":\"On\""));
  /* push: type 9 sends On, type 10 sends Off */
  d.cap = DZ_CAP_PUSH; d.switch_type = 9;
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_PUSH, "5", &d, -1, 0) > 0 && strstr(b, "\"state\":\"On\""));
  d.switch_type = 10;
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_PUSH, "5", &d, -1, 0) > 0 && strstr(b, "\"state\":\"Off\""));
  /* bad idx and tiny buffers */
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_ON, "9x", &d, -1, 0) == 0 && dz_mcp_encode(b, sizeof b, 5, DZ_CMD_ON, "", &d, -1, 0) == 0);
  CHECK(dz_mcp_encode(b, sizeof b, 5, DZ_CMD_ON, "1234567890", &d, -1, 0) == 0);
  CHECK(dz_mcp_encode(b, 40, 5, DZ_CMD_ON, "1", &d, -1, 0) == 0);
  size_t n = dz_mcp_encode(b, sizeof b, 5, DZ_CMD_ON, "1", &d, -1, 0);
  CHECK(n > 0 && n < 200); /* small enough for one buffer in dz_client_t */
  CHECK(!strpbrk(b, "\r\n"));
#undef EXP
}


/* Domoticz: a Group toggles On/Off and can be Mixed; a Scene is push-only. */
static void test_scene_group_semantics(void) {
  setup(true);
  dz_device_t d, scene, group;
  ru_val_t v;
  ru_val_state_t st;
  dz_cmd_kind_t k;
  CHECK(dz_model_get(&M, "1", true, &scene, &v, &st, T) && scene.cap == DZ_CAP_SCENE);
  CHECK(dz_model_get(&M, "2", true, &group, &v, &st, T) && group.cap == DZ_CAP_GROUP);

  /* what a tap does */
  CHECK(dz_cmd_scene_tap_kind(&scene, &k) && k == DZ_CMD_SCENE_ON);          /* a scene is always pushed */
  CHECK(dz_cmd_scene_tap_kind(&group, &k) && k == DZ_CMD_SCENE_ON);          /* group Off -> On */
  strcpy(group.status, "On");
  CHECK(dz_cmd_scene_tap_kind(&group, &k) && k == DZ_CMD_SCENE_OFF);         /* group On -> Off */
  strcpy(group.status, "Mixed");
  CHECK(dz_cmd_scene_tap_kind(&group, &k) && k == DZ_CMD_SCENE_ON);          /* group Mixed -> On (bring all in line) */
  scene.status[0] = 0;
  strcpy(scene.status, "On");
  CHECK(dz_cmd_scene_tap_kind(&scene, &k) && k == DZ_CMD_SCENE_ON);          /* a scene that reads On is pushed again, never "turned off" */
  CHECK(dz_model_get(&M, "1", false, &d, &v, &st, T) && !dz_cmd_scene_tap_kind(&d, &k)); /* an ordinary device: not ours */
  CHECK(!dz_cmd_scene_tap_kind(NULL, &k));

  /* both need a hold, and a Scene never accepts Off */
  CHECK(dz_cmd_risk(&scene, DZ_CMD_SCENE_ON, NULL) == DZ_RISK_CONFIRM && dz_cmd_risk(&group, DZ_CMD_SCENE_ON, NULL) == DZ_RISK_CONFIRM);
  CHECK(dz_cmd_risk(&scene, DZ_CMD_SCENE_OFF, NULL) == DZ_RISK_LOCKED);
  CHECK(dz_cmd_risk(&group, DZ_CMD_SCENE_OFF, NULL) == DZ_RISK_CONFIRM);
  CHECK(run(DZ_CMD_SCENE_OFF, "1", 0, 0, true, NULL) == DZ_CMDE_NOT_CONTROLLABLE);

  /* group On that comes back Mixed: some members did not switch, so NOT confirmed */
  group_mixed = true;
  CHECK(run(DZ_CMD_SCENE_ON, "2", 0, 0, true, NULL) == DZ_CMD_ACCEPTED);
  CHECK(LAST.outcome == DZ_OUT_UNCONFIRMED && LAST.have_after && !strcmp(LAST.after.status, "Mixed"));
  CHECK(dz_device_state(&LAST.after) == DZ_STATE_MIXED);
  /* and Off from Mixed is confirmed only when it really reads Off */
  CHECK(run(DZ_CMD_SCENE_OFF, "2", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
  group_mixed = false;
  CHECK(run(DZ_CMD_SCENE_ON, "2", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && !strcmp(LAST.after.status, "On"));

  /* scene already On from an earlier push: a repeat is confirmed only by a fresh LastUpdate */
  CHECK(run(DZ_CMD_SCENE_ON, "1", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED); /* Off -> On */
  CHECK(run(DZ_CMD_SCENE_ON, "1", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED); /* On -> pushed: LastUpdate moved */
  xm = X_IGNORE;                                                    /* HTTP OK, nothing happens */
  CHECK(run(DZ_CMD_SCENE_ON, "1", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_UNCONFIRMED); /* still "On", but no proof */
  xm = X_OK;
}

/* ---- MCP control path ---- */

/* every command kind: the exact JSON body that goes out, and that the re-read confirms it */
static void test_kinds_table(void) {
  struct { const char *name; dz_cmd_kind_t kind; const char *idx; int level; float sp; const char *args; dz_outcome_t out; } t[] = {
    {"on/off on", DZ_CMD_ON, "1", 0, 0, "\"arguments\":{\"idx\":1,\"state\":\"On\"}", DZ_OUT_CONFIRMED},
    {"dimmer level", DZ_CMD_SET_LEVEL, "2", 40, 0, "\"arguments\":{\"idx\":2,\"level\":40}", DZ_OUT_CONFIRMED},
    {"selector", DZ_CMD_SELECTOR, "4", 20, 0, "\"arguments\":{\"idx\":4,\"level\":20}", DZ_OUT_CONFIRMED},
    {"lock 19 lock", DZ_CMD_ON, "6", 0, 0, "\"arguments\":{\"idx\":6,\"state\":\"On\"}", DZ_OUT_CONFIRMED},
    {"lock 19 unlock", DZ_CMD_OFF, "6", 0, 0, "\"arguments\":{\"idx\":6,\"state\":\"Off\"}", DZ_OUT_CONFIRMED},
    {"lock 20 lock sends Off", DZ_CMD_ON, "8", 0, 0, "\"arguments\":{\"idx\":8,\"state\":\"Off\"}", DZ_OUT_CONFIRMED},
    {"lock 20 unlock sends On", DZ_CMD_OFF, "8", 0, 0, "\"arguments\":{\"idx\":8,\"state\":\"On\"}", DZ_OUT_CONFIRMED},
    {"push 9 sends On", DZ_CMD_PUSH, "5", 0, 0, "\"arguments\":{\"idx\":5,\"state\":\"On\"}", DZ_OUT_CONFIRMED},
    {"push 10 sends Off", DZ_CMD_PUSH, "15", 0, 0, "\"arguments\":{\"idx\":15,\"state\":\"Off\"}", DZ_OUT_CONFIRMED},
    {"blinds close", DZ_CMD_BLINDS_CLOSE, "3", 0, 0, "\"arguments\":{\"idx\":3,\"command\":\"Close\"}", DZ_OUT_CONFIRMED},
    {"blinds stop", DZ_CMD_BLINDS_STOP, "3", 0, 0, "\"arguments\":{\"idx\":3,\"command\":\"Stop\"}", DZ_OUT_CONFIRMED},
    /* inverted blinds (old types 6 and 16): Domoticz inverts server-side (SwitchLightInt, ReverseState), so the client sends the
     * plain word and never swaps Open/Close itself. The server reports the user-facing state, which is what matches() reads. */
    {"inverted blind 6 open", DZ_CMD_BLINDS_OPEN, "12", 0, 0, "\"arguments\":{\"idx\":12,\"command\":\"Open\"}", DZ_OUT_CONFIRMED},
    {"inverted blind 6 close", DZ_CMD_BLINDS_CLOSE, "12", 0, 0, "\"arguments\":{\"idx\":12,\"command\":\"Close\"}", DZ_OUT_CONFIRMED},
    {"inverted blind 16 open", DZ_CMD_BLINDS_OPEN, "13", 0, 0, "\"arguments\":{\"idx\":13,\"command\":\"Open\"}", DZ_OUT_CONFIRMED},
    {"percentage blind open", DZ_CMD_BLINDS_OPEN, "14", 0, 0, "\"arguments\":{\"idx\":14,\"command\":\"Open\"}", DZ_OUT_CONFIRMED},
    {"scene (number)", DZ_CMD_SCENE_ON, "1", 0, 0, "\"arguments\":{\"scene_id\":1,\"command\":\"On\"}", DZ_OUT_CONFIRMED},
    {"group on", DZ_CMD_SCENE_ON, "2", 0, 0, "\"arguments\":{\"scene_id\":2,\"command\":\"On\"}", DZ_OUT_CONFIRMED},
    {"setpoint", DZ_CMD_SETPOINT, "9", 0, 22.5f, "\"arguments\":{\"idx\":9,\"setpoint\":22.5}", DZ_OUT_CONFIRMED},
  };
  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
    setup(true);
    dz_cmd_err_t e = run(t[i].kind, t[i].idx, t[i].level, t[i].sp, true, NULL);
    if (e != DZ_CMD_ACCEPTED || LAST.outcome != t[i].out || !strstr(LASTB, t[i].args)) printf("kind case: %s (e=%d out=%d)\n", t[i].name, e, nres ? (int)LAST.outcome : -1);
    CHECK(e == DZ_CMD_ACCEPTED && nres == 1 && LAST.outcome == t[i].out && strstr(LASTB, t[i].args));
    CHECK(!strstr(LASTB, "\"scene_id\":\"") && !strstr(LASTB, "Authorization") && !strstr(LASTB, "TOK-SECRET"));
  }
  /* group Off */
  setup(true);
  CHECK(run(DZ_CMD_SCENE_ON, "2", 0, 0, true, NULL) == DZ_CMD_ACCEPTED);
  CHECK(run(DZ_CMD_SCENE_OFF, "2", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && strstr(LASTB, "\"scene_id\":2,\"command\":\"Off\""));
  /* B4: a percentage blind that reports only "Set Level: N %" is NOT confirmed (100 = open is unverified on a real blind) */
  setup(true);
  pblind_levels = true;
  CHECK(run(DZ_CMD_BLINDS_OPEN, "14", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_UNCONFIRMED && !strcmp(LAST.after.status, "Set Level: 100 %") && LAST.fail == DZ_FAIL_NONE);
  CHECK(run(DZ_CMD_BLINDS_CLOSE, "14", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_UNCONFIRMED && !strcmp(LAST.after.status, "Set Level: 0 %"));
  /* and a blind that stayed at 30 % is unconfirmed too */
  xm = X_IGNORE;
  CHECK(run(DZ_CMD_BLINDS_OPEN, "14", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_UNCONFIRMED);
  pblind_levels = false;
}

static void test_mcp_gates(void) {
  setup(true);
  /* level above 100: refused at submit (the MCP tool takes 0..100) */
  CHECK(run(DZ_CMD_SET_LEVEL, "16", 50, 0, false, NULL) == DZ_CMDE_BAD_ARG && F.calls == 0);
  CHECK(run(DZ_CMD_SET_LEVEL, "11", 99, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.level_sent == 15);
  /* MODE: no tool, unsupported, never any request */
  dz_device_t d; ru_val_t v; ru_val_state_t st;
  CHECK(dz_model_get(&M, "17", false, &d, &v, &st, T) && d.cap == DZ_CAP_MODE);
  CHECK(!dz_cmd_supported(&d, DZ_CMD_MODE, NULL) && dz_cmd_risk(&d, DZ_CMD_MODE, NULL) == DZ_RISK_LOCKED);
  int c0 = F.calls;
  CHECK(run(DZ_CMD_MODE, "17", 1, 0, true, NULL) == DZ_CMDE_UNSUPPORTED && F.calls == c0);
  CHECK(!strcmp(dz_cmd_err_name(DZ_CMDE_UNSUPPORTED), "unsupported"));
  /* what is supported */
  CHECK(dz_model_get(&M, "9", false, &d, &v, &st, T) && dz_cmd_supported(&d, DZ_CMD_SETPOINT, &(dz_units_t){.have_temp = true, .temp = "C"}));
  CHECK(dz_model_get(&M, "2", false, &d, &v, &st, T) && dz_cmd_supported(&d, DZ_CMD_SET_LEVEL, NULL) && !dz_cmd_supported(&d, DZ_CMD_BLINDS_OPEN, NULL));
  CHECK(!dz_cmd_supported(NULL, DZ_CMD_ON, NULL));
  /* Fahrenheit server: the setpoint tool takes Celsius only, so SETPOINT is refused and nothing is sent */
  dz_units_t u;
  memset(&u, 0, sizeof u);
  u.have_temp = true;
  strcpy(u.temp, "F");
  dz_model_set_units(&M, &u);
  CHECK(run(DZ_CMD_SETPOINT, "9", 0, 70.0f, true, NULL) == DZ_CMDE_UNSUPPORTED && F.calls == c0);
  strcpy(u.temp, "C");
  dz_model_set_units(&M, &u);
  CHECK(run(DZ_CMD_SETPOINT, "9", 0, 21.0f, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
  /* a device whose own value unit is Fahrenheit is read-only even when the units are unknown */
  CHECK(dz_model_get(&M, "9", false, &d, &v, &st, T));
  strcpy(d.vunit, "\xC2\xB0" "F");
  CHECK(!dz_cmd_supported(&d, DZ_CMD_SETPOINT, NULL));
}

/* B1: a server whose temperature unit is not known (getconfig failed) is treated like Fahrenheit for SETPOINT */
static void test_setpoint_unknown_units(void) {
  setup(true);
  dz_device_t d; ru_val_t v; ru_val_state_t st;
  dz_units_t u;
  CHECK(dz_model_get(&M, "9", false, &d, &v, &st, T));
  memset(&u, 0, sizeof u);
  u.have_temp = true; strcpy(u.temp, "C");
  CHECK(dz_cmd_supported(&d, DZ_CMD_SETPOINT, &u) && dz_cmd_risk(&d, DZ_CMD_SETPOINT, &u) == DZ_RISK_CONFIRM);
  CHECK(!dz_cmd_supported(&d, DZ_CMD_SETPOINT, NULL) && dz_cmd_risk(&d, DZ_CMD_SETPOINT, NULL) == DZ_RISK_LOCKED);
  u.have_temp = false;
  CHECK(!dz_cmd_supported(&d, DZ_CMD_SETPOINT, &u));
  set_units(NULL);
  int c0 = F.calls;
  CHECK(run(DZ_CMD_SETPOINT, "9", 0, 21.0f, true, NULL) == DZ_CMDE_UNSUPPORTED && F.calls == c0 && cmd_calls == 0);
  /* known Celsius works; the units vanishing between submit and the pre-flight stops it before the call */
  set_units("C");
  dz_cmd_req_t q = {.kind = DZ_CMD_SETPOINT, .idx = "9", .setpoint = 21.0f, .confirmed = true};
  CHECK(dz_cmd_submit(&K, &q, T, NULL) == DZ_CMD_ACCEPTED);
  set_units(NULL);
  CHECK(dz_cmd_pump(&K, T) && nres == 1 && LAST.outcome == DZ_OUT_FAILED && LAST.fail == DZ_FAIL_REJECTED && cmd_calls == 0);
  set_units("C");
  CHECK(run(DZ_CMD_SETPOINT, "9", 0, 21.0f, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
}

static void test_mcp_session_and_mapping(void) {
  /* one session for several commands */
  setup(true);
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && run(DZ_CMD_OFF, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED &&
        run(DZ_CMD_TOGGLE, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED);
  CHECK(mcp_inits == 1 && mcp_notifs == 1 && cmd_calls == 3 && nres == 3 && LAST.outcome == DZ_OUT_CONFIRMED);
  CHECK(F.sid_in_req && !strstr(fake_last_url(&F), "0123456789abcdef") && !F.post_secret_leak && !F.auth_in_url && !F.proto_hdr);
  /* the session expired: ONE re-initialize and ONE retry, the command still lands once */
  setup(true);
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED);
  xm = X_EXPIRE_ONCE;
  CHECK(run(DZ_CMD_OFF, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && mcp_inits == 2 && applied == 2);
  /* expired twice in a row: BAD_RESPONSE, no third try */
  setup(true);
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED);
  xm = X_EXPIRE_ALWAYS;
  CHECK(run(DZ_CMD_OFF, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_FAILED && LAST.fail == DZ_FAIL_BAD_RESPONSE && mcp_inits == 2);
  /* timeout on the call is NEVER retried; the re-read decides */
  setup(true);
  xm = X_TIMEOUT_APPLIED;
  CHECK(run(DZ_CMD_TOGGLE, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && cmd_calls == 1 && applied == 1);
  /* a timeout in initialize is a transport failure and nothing was sent */
  setup(true);
  xm = X_INIT_TIMEOUT;
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_FAILED && LAST.fail == DZ_FAIL_TRANSPORT && cmd_calls == 0);

  /* every failure row */
  struct { const char *name; xmode_t x; int code; dz_fail_t f; } rows[] = {
    {"503 no MCP", X_NOMCP503, 0, DZ_FAIL_NO_MCP}, {"404 no MCP", X_NOMCP404, 0, DZ_FAIL_NO_MCP},
    {"401", X_401, 0, DZ_FAIL_UNAUTHORIZED}, {"403", X_403, 0, DZ_FAIL_FORBIDDEN}, {"500", X_500, 0, DZ_FAIL_SERVER},
    {"isError", X_ISERROR, 0, DZ_FAIL_REJECTED}, {"Error text", X_ERR, 0, DZ_FAIL_SERVER}, {"html 200", X_HTML, 0, DZ_FAIL_BAD_RESPONSE},
    {"init connect", X_INIT_CONNECT, 0, DZ_FAIL_TRANSPORT},
    {"-32601", X_RPCERR, -32601, DZ_FAIL_NO_MCP}, {"-32002", X_RPCERR, -32002, DZ_FAIL_FORBIDDEN},
    {"-32000", X_RPCERR, -32000, DZ_FAIL_SERVER}, {"-32003", X_RPCERR, -32003, DZ_FAIL_SERVER},
    {"-32004", X_RPCERR, -32004, DZ_FAIL_SERVER}, {"-32603", X_RPCERR, -32603, DZ_FAIL_SERVER},
    {"-32602", X_RPCERR, -32602, DZ_FAIL_REJECTED}, {"-32700", X_RPCERR, -32700, DZ_FAIL_BAD_RESPONSE},
    {"-32600", X_RPCERR, -32600, DZ_FAIL_BAD_RESPONSE}, {"-32001", X_RPCERR, -32001, DZ_FAIL_BAD_RESPONSE},
  };
  for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
    setup(true);
    xm = rows[i].x;
    rpc_code = rows[i].code;
    CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED);
    if (LAST.outcome != DZ_OUT_FAILED || LAST.fail != rows[i].f) printf("row %s: fail %d want %d\n", rows[i].name, LAST.fail, rows[i].f);
    CHECK(nres == 1 && LAST.outcome == DZ_OUT_FAILED && LAST.fail == rows[i].f && !LAST.have_after);
    CHECK(applied == 0 && dz_cmd_pending(&K) == 0);
    CHECK((dz_client_state(&C) == DZ_CS_TOKEN_REJECTED) == (rows[i].x == X_401));
  }
  /* 'already On. No action taken.': the re-read decides (an out-of-sync device is no longer forced) */
  setup(true);
  xm = X_NOACTION;
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_UNCONFIRMED && applied == 0);
  strcpy(S[0].status, "On");
  nres = 0;
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
}

/* the endpoint skips Domoticz's Protected check: the pre-flight re-read is the guard */
static void test_preflight(void) {
  setup(true);
  S[0].protectedf = 1; /* became protected after the last poll */
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED);
  CHECK(nres == 1 && LAST.outcome == DZ_OUT_FAILED && LAST.fail == DZ_FAIL_PROTECTED && cmd_calls == 0 && mcp_inits == 0 && applied == 0);
  CHECK(S[0].status[0] == 'O' && !strcmp(S[0].status, "Off"));
  /* the pre-flight also refreshes the pre-state: the lock became an inverted one (type 20) */
  setup(true);
  S[5].stv = 20; strcpy(S[5].status, "Unlocked");
  CHECK(run(DZ_CMD_ON, "6", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && strstr(LASTB, "\"state\":\"Off\"") && LAST.outcome == DZ_OUT_CONFIRMED);
  /* the pre-flight read fails: nothing is sent */
  setup(true);
  xm = X_PREFLIGHT_404;
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_FAILED && LAST.fail == DZ_FAIL_BAD_RESPONSE && cmd_calls == 0);
  /* a scene that is protected */
  setup(true);
  find("1", true)->protectedf = 1;
  CHECK(run(DZ_CMD_SCENE_ON, "1", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.fail == DZ_FAIL_PROTECTED && cmd_calls == 0);
}

int main(void) {
  test_mcp_encode();
  test_kinds_table();
  test_mcp_gates();
  test_setpoint_unknown_units();
  test_mcp_session_and_mapping();
  test_preflight();
  test_basic_confirm();
  test_timing();
  test_levels();
  test_level_percent();
  test_blinds_push();
  test_risk();
  test_refusals();
  test_scene_group_semantics();
  test_failures();
  MT_DONE();
}
