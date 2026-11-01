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
  char status[24], data[24], lu[24], names[40];
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
  {"1", "Scene", NULL, "scene", -1, 0, 100, 0, 1, "Off", "Off", "2026-10-31 10:00:00", "", 0},
  {"2", "Group", NULL, "group", -1, 0, 100, 0, 1, "Off", "Off", "2026-10-31 10:00:00", "", 0},
};
#define NS (sizeof S / sizeof S[0])

typedef enum { X_OK, X_IGNORE, X_401, X_403, X_500, X_ERR, X_HTML, X_CONNECT, X_TIMEOUT_APPLIED, X_TIMEOUT_IGNORED, X_VERIFY_FAIL } xmode_t;
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
    n += (size_t)snprintf(out + n, cap - n, "}");
  }
  snprintf(out + n, cap - n, "],\"status\":\"OK\",\"title\":\"Devices\"}");
}

static sim_t *find(const char *idx, bool scene) {
  for (size_t i = 0; i < NS; i++) if (!strcmp(S[i].idx, idx) && S[i].scene == scene) return &S[i];
  return NULL;
}
static const char *qget(const char *url, const char *key, char *out, size_t cap) {
  char pat[32];
  snprintf(pat, sizeof pat, "&%s=", key);
  const char *p = strstr(url, pat);
  if (!p) return NULL;
  p += strlen(pat);
  size_t n = strcspn(p, "&");
  if (n >= cap) n = cap - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return out;
}

static void touch(sim_t *s) { snprintf(s->lu, sizeof s->lu, "2026-10-31 10:%02d:%02d", ticks / 60 % 60, ticks % 60); ticks++; }

static bool group_mixed; /* simulate a group whose members do not all switch */

static void apply_cmd(const char *url) {
  char idx[16], cmd[32], lvl[16], sp[16];
  if (!qget(url, "idx", idx, sizeof idx)) return;
  bool scene = strstr(url, "param=switchscene") != NULL;
  sim_t *s = find(idx, scene);
  if (!s) return;
  if (strstr(url, "param=setsetpoint")) { if (qget(url, "setpoint", sp, sizeof sp)) { s->setpoint = strtof(sp, NULL); touch(s); } return; }
  if (!qget(url, "switchcmd", cmd, sizeof cmd)) return;
  if (!strcmp(cmd, "Set%20Level") && qget(url, "level", lvl, sizeof lvl)) {
    int n = atoi(lvl);
    s->level = n;
    if (s->stv == 18) { snprintf(s->data, sizeof s->data, "L%d", n); }
    else snprintf(s->status, sizeof s->status, n ? "Set Level: %d %%" : "Off", n);
    touch(s);
    return;
  }
  if (!strcmp(cmd, "Toggle")) strcpy(cmd, (!strcmp(s->status, "Off") || !strcmp(s->status, "Unlocked")) ? "On" : "Off");
  if (s->stv == 19) strcpy(s->status, !strcmp(cmd, "On") ? "Locked" : "Unlocked");
  else if (s->stv == 20) strcpy(s->status, !strcmp(cmd, "On") ? "Unlocked" : "Locked");
  else if (s->stv == 3) { if (strcmp(cmd, "Stop")) strcpy(s->status, !strcmp(cmd, "Open") ? "Open" : "Closed"); }
  else if (s->stv == 9) { strcpy(s->data, cmd); }
  else {
    strcpy(s->status, cmd);
    if (scene && group_mixed && !strcmp(s->idx, "2") && !strcmp(cmd, "On")) strcpy(s->status, "Mixed");
  }
  touch(s);
}

static const char *OKJ = "{\"status\":\"OK\",\"title\":\"SwitchLight\"}";

static dz_net_err_t srv(fake_t *f, const char *url, char *buf, size_t cap, dz_http_result_t *out) {
  (void)f;
  bool is_cmd = strstr(url, "param=switch") || strstr(url, "param=setsetpoint");
  if (is_cmd) {
    cmd_calls++;
    switch (xm) {
      case X_401: return fake_body("Unauthorized", 401, buf, cap, out);
      case X_403: return fake_body("{\"status\":\"ERR\"}", 403, buf, cap, out);
      case X_500: return fake_body("err", 500, buf, cap, out);
      case X_ERR: return fake_body("{\"status\":\"ERR\"}", 200, buf, cap, out);
      case X_HTML: return fake_body("<html>proxy</html>", 200, buf, cap, out);
      case X_CONNECT: return DZ_NET_CONNECT;
      case X_TIMEOUT_APPLIED: apply_cmd(url); return DZ_NET_TIMEOUT;
      case X_TIMEOUT_IGNORED: return DZ_NET_TIMEOUT;
      case X_IGNORE: return fake_body(OKJ, 200, buf, cap, out); /* HTTP OK, but nothing happens */
      default: apply_cmd(url); return fake_body(OKJ, 200, buf, cap, out);
    }
  }
  read_calls++;
  if (xm == X_VERIFY_FAIL) return DZ_NET_TIMEOUT;
  char tmp[8192], rid[16];
  if (strstr(url, "param=getscenes")) render(tmp, sizeof tmp, true, NULL);
  else render(tmp, sizeof tmp, false, qget(url, "rid", rid, sizeof rid));
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
  group_mixed = false;
  for (size_t i = 0; i < NS; i++) {
    sim_t *s = &S[i];
    strcpy(s->lu, "2026-10-31 10:00:00");
  }
  strcpy(S[0].status, "Off"); strcpy(S[1].status, "Off"); S[1].level = 29; strcpy(S[2].status, "Open");
  strcpy(S[3].data, "Low"); S[3].level = 10; strcpy(S[5].status, "Locked"); strcpy(S[7].status, "Unlocked");
  S[8].setpoint = 20.0f; strcpy(S[10].status, "Off"); strcpy(S[11].status, "Off"); strcpy(S[12].status, "Off");
  ticks = 0;
}

static uint32_t T;
static void setup(bool allow) {
  if (M.mu) dz_model_deinit(&M);
  dz_model_cfg_t mc = {.ttl_ms = 600000, .old_update_s = UINT32_MAX};
  dz_model_init(&M, ent, CAP, stg, CAP, &mc);
  fake_init(&F, srv, NULL);
  F.secret = "TOK-SECRET";
  xm = X_OK;
  cmd_calls = read_calls = nres = 0;
  reset_sim();
  dz_client_cfg_t cfg = {.base_url = "http://h", .token = "TOK-SECRET", .transport = &F.base, .model = &M, .buf = rbuf, .buf_cap = sizeof rbuf};
  CHECK(dz_client_init(&C, &cfg));
  char tmp[8192];
  render(tmp, sizeof tmp, false, NULL);
  CHECK(dz_model_apply(&M, tmp, strlen(tmp), DZ_COMMIT_FULL_DEVICES, 1000, NULL, NULL) == DZ_OK);
  render(tmp, sizeof tmp, true, NULL);
  CHECK(dz_model_apply(&M, tmp, strlen(tmp), DZ_COMMIT_FULL_SCENES, 1000, NULL, NULL) == DZ_OK);
  dz_cmd_init(&K, &C, &M, allow, on_res, NULL);
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

static void test_basic_confirm(void) {
  setup(true);
  uint32_t id1 = 0, id2 = 0;
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, &id1) == DZ_CMD_ACCEPTED);
  CHECK(nres == 1 && LAST.outcome == DZ_OUT_CONFIRMED && LAST.id == id1 && LAST.have_after && !strcmp(LAST.after.status, "On"));
  CHECK(cmd_calls == 1 && read_calls == 1 && strstr(fake_last_url(&F), "getdevices&rid=1"));
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
  CHECK(dz_cmd_pump(&K, T) && cmd_calls == 1 && read_calls == 0);
  CHECK(!dz_cmd_pump(&K, T + 1499) && nres == 0);       /* the re-read waits */
  CHECK(dz_cmd_pump(&K, T + 1500) && nres == 1 && read_calls == 1);
  /* ignored command: HTTP OK, no effect: two reads, 1.5 s apart, then UNCONFIRMED */
  setup(true);
  xm = X_IGNORE;
  uint32_t t0 = T;
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED);
  CHECK(nres == 1 && LAST.outcome == DZ_OUT_UNCONFIRMED && read_calls == 2 && T - t0 == 3000);
  CHECK(LAST.fail == DZ_FAIL_NONE && LAST.have_after && !strcmp(LAST.after.status, "Off"));
}

static void test_levels(void) {
  setup(true);
  CHECK(run(DZ_CMD_SET_LEVEL, "2", 150, 0, false, NULL) == DZ_CMD_ACCEPTED);
  CHECK(strstr(F.urls[0], "switchcmd=Set%20Level&level=100") && !strchr(F.urls[0], ' ') && LAST.level_sent == 100);
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

static void test_blinds_push(void) {
  setup(true);
  CHECK(run(DZ_CMD_BLINDS_CLOSE, "3", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
  CHECK(strstr(F.urls[0], "switchcmd=Close"));
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
  CHECK(dz_cmd_risk(&d, DZ_CMD_OFF) == DZ_RISK_CONFIRM);
  CHECK(run(DZ_CMD_OFF, "6", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && !strcmp(LAST.after.status, "Unlocked"));
  CHECK(run(DZ_CMD_ON, "6", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && !strcmp(LAST.after.status, "Locked"));
  CHECK(run(DZ_CMD_TOGGLE, "6", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && !strcmp(LAST.after.status, "Unlocked"));
  /* inverted lock (type 20): On sends Off */
  CHECK(run(DZ_CMD_ON, "8", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && strstr(fake_last_url(&F) , "rid=8") && LAST.outcome == DZ_OUT_CONFIRMED);
  /* scenes and groups: confirm; Scene has no Off */
  CHECK(run(DZ_CMD_SCENE_ON, "2", 0, 0, false, NULL) == DZ_CMDE_NEEDS_CONFIRM);
  int c0 = cmd_calls;
  CHECK(run(DZ_CMD_SCENE_ON, "2", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && cmd_calls == c0 + 1);
  CHECK(strstr(F.urls[(F.calls - 2) % 32], "param=switchscene&idx=2&switchcmd=On") && strstr(fake_last_url(&F), "getscenes"));
  CHECK(run(DZ_CMD_SCENE_OFF, "2", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED && !strcmp(LAST.after.status, "Off"));
  CHECK(run(DZ_CMD_SCENE_OFF, "1", 0, 0, true, NULL) == DZ_CMDE_NOT_CONTROLLABLE);
  CHECK(run(DZ_CMD_SCENE_ON, "1", 0, 0, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
  /* scene idx 1 and device idx 1 are different things */
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED && strstr(fake_last_url(&F), "getdevices&rid=1"));
  /* setpoint */
  CHECK(run(DZ_CMD_SETPOINT, "9", 0, 21.5f, false, NULL) == DZ_CMDE_NEEDS_CONFIRM);
  CHECK(run(DZ_CMD_SETPOINT, "9", 0, 21.5f, true, NULL) == DZ_CMD_ACCEPTED && LAST.outcome == DZ_OUT_CONFIRMED);
  CHECK(strstr(F.urls[(F.calls - 2) % 32], "param=setsetpoint&idx=9&setpoint=21.5"));
  CHECK(run(DZ_CMD_SETPOINT, "9", 0, -3.0f, true, NULL) == DZ_CMD_ACCEPTED && strstr(F.urls[(F.calls - 2) % 32], "setpoint=-3.0"));
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
  CHECK(dz_cmd_risk(&d, DZ_CMD_ON) == DZ_RISK_LOCKED);
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
    {X_CONNECT, DZ_FAIL_TRANSPORT}, {X_401, DZ_FAIL_UNAUTHORIZED},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    setup(true);
    xm = cases[i].x;
    CHECK(run(DZ_CMD_ON, "1", 0, 0, false, NULL) == DZ_CMD_ACCEPTED);
    CHECK(nres == 1 && LAST.outcome == DZ_OUT_FAILED && LAST.fail == cases[i].f && !LAST.have_after);
    CHECK(read_calls == 0); /* no point re-reading after a definite failure */
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

static void test_encode(void) {
  char b[32];
  CHECK(dz_url_encode(b, sizeof b, "Set Level") == 11 && !strcmp(b, "Set%20Level"));
  CHECK(dz_url_encode(b, sizeof b, "a&b=c?d#e+f/g%h") > 0 && !strpbrk(b, "&=?#+/ ") && strstr(b, "%26") && strstr(b, "%25"));
  CHECK(dz_url_encode(b, sizeof b, "AZaz09-._~") == 10 && !strcmp(b, "AZaz09-._~"));
  CHECK(dz_url_encode(b, sizeof b, "\xc3\xa9") == 6 && !strcmp(b, "%C3%A9"));
  CHECK(dz_url_encode(b, 4, "Set Level") == -1);
  CHECK(dz_url_encode(b, 3, "ab") == 2 && dz_url_encode(b, 2, "ab") == -1);
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
  CHECK(dz_cmd_risk(&scene, DZ_CMD_SCENE_ON) == DZ_RISK_CONFIRM && dz_cmd_risk(&group, DZ_CMD_SCENE_ON) == DZ_RISK_CONFIRM);
  CHECK(dz_cmd_risk(&scene, DZ_CMD_SCENE_OFF) == DZ_RISK_LOCKED);
  CHECK(dz_cmd_risk(&group, DZ_CMD_SCENE_OFF) == DZ_RISK_CONFIRM);
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

int main(void) {
  test_encode();
  test_basic_confirm();
  test_timing();
  test_levels();
  test_blinds_push();
  test_risk();
  test_refusals();
  test_scene_group_semantics();
  test_failures();
  MT_DONE();
}
