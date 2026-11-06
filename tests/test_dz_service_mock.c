/* dz_service against the Node mock over real sockets (run through tests/run_with_mock.sh). The clock is simulated,
 * the HTTP is real. Covers read-after-write commands through the service mailbox, Group toggle vs Scene push,
 * viewer 403, and TOKEN_REJECTED with the events posted on the bus. Plain http to 127.0.0.1 is allowed here because
 * the config says insecure_lan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dz_service.h"
#include "mini_test.h"

#define CAP 64
static dz_entry_t ent[CAP];
static dz_device_t stg[CAP];
static char rbuf[DZ_SERVICE_MIN_BUF_FULL];
static ru_bus_t BUS;
static ru_bus_sub_t SUB;
static ru_event_t SUBQ[512];
static dz_service_t S;
static dz_transport_t *TR;
static char url[64];
static uint32_t T = 1000;

static void ctl(const char *q) {
  char u[160], b[512];
  dz_http_result_t r;
  snprintf(u, sizeof u, "%s/__mock/%s", url, q);
  CHECK(TR->get(TR, u, NULL, 3000, b, sizeof b, &r) == DZ_NET_OK && r.status == 200);
}

static void start(const char *token, bool control) {
  memset(ent, 0, sizeof ent);
  ru_bus_init(&BUS);
  CHECK(ru_bus_subscribe(&BUS, &SUB, SUBQ, 512, 1u << DZ_SERVICE_TOPIC_DEFAULT, NULL, NULL) == RU_OK);
  dz_service_cfg_t c;
  memset(&c, 0, sizeof c);
  c.transport = TR;
  c.bus = &BUS;
  c.buf = rbuf;
  c.buf_cap = sizeof rbuf;
  c.entries = ent;
  c.entries_cap = CAP;
  c.stage = stg;
  c.stage_cap = CAP;
  c.poll_interval_ms = 10000;
  CHECK(dz_service_init(&S, &c));
  ru_cfg_t cfg;
  memset(&cfg, 0, sizeof cfg);
  snprintf(cfg.url, sizeof cfg.url, "%s", url);
  snprintf(cfg.token, sizeof cfg.token, "%s", token);
  cfg.insecure_lan = true;
  CHECK(dz_service_configure(&S, &cfg, control) == RU_CFG_OK);
  ru_cfg_wipe(&cfg);
}
static void stop(void) {
  dz_service_deinit(&S);
  ru_bus_unsubscribe(&BUS, &SUB);
  ru_bus_deinit(&BUS);
}
static void spin(int rounds) {
  for (int i = 0; i < rounds; i++) {
    uint32_t w = dz_service_run_once(&S, T);
    if (w == DZ_SERVICE_MAX_SLEEP_MS) w = 0;
    T += w;
  }
}

typedef struct { int dev, model, cmd, auth; dz_ev_cmd_t cmdev; bool saw_idx[16]; } seen_t;
static seen_t drain(void) {
  seen_t s;
  memset(&s, 0, sizeof s);
  ru_event_t e;
  while (ru_bus_poll(&SUB, &e)) {
    if (e.type == DZ_EV_DEVICE_CHANGED) { s.dev++; int i = atoi(((dz_ev_device_t *)e.payload)->idx); if (i >= 0 && i < 16 && !((dz_ev_device_t *)e.payload)->is_scene) s.saw_idx[i] = true; }
    else if (e.type == DZ_EV_MODEL_CHANGED) s.model++;
    else if (e.type == DZ_EV_CMD_RESULT) { s.cmd++; memcpy(&s.cmdev, e.payload, sizeof s.cmdev); }
    else if (e.type == DZ_EV_AUTH_REJECTED) s.auth++;
  }
  return s;
}

static seen_t run_cmd(dz_cmd_kind_t k, const char *idx, bool confirmed) {
  dz_cmd_req_t q = {.kind = k, .idx = idx, .confirmed = confirmed};
  uint32_t id = dz_service_submit(&S, &q);
  CHECK(id > 0);
  seen_t s;
  memset(&s, 0, sizeof s);
  for (int i = 0; i < 12 && !s.cmd; i++) {
    spin(1);
    seen_t x = drain();
    s.dev += x.dev; s.model += x.model; s.cmd += x.cmd;
    for (int j = 0; j < 16; j++) s.saw_idx[j] |= x.saw_idx[j];
    if (x.cmd) s.cmdev = x.cmdev;
  }
  CHECK(s.cmd == 1 && s.cmdev.req_id == id);
  return s;
}

int main(void) {
  const char *p = getenv("MOCK_PORT");
  if (!p) { printf("run through tests/run_with_mock.sh\n"); return 2; }
  snprintf(url, sizeof url, "http://127.0.0.1:%s", p);
  TR = dz_transport_socket_create();
  ctl("reset");

  /* user token: full flow */
  start("user-token", true);
  spin(14);
  seen_t s = drain();
  CHECK(dz_service_conn_state(&S) == DZ_CS_OK && s.dev > 5 && s.model >= 1 && s.saw_idx[1]);
  s = run_cmd(DZ_CMD_ON, "1", false);
  CHECK(s.cmdev.reject == 0 && s.cmdev.outcome == DZ_OUT_CONFIRMED && s.dev >= 1 && s.saw_idx[1]); /* read-after-write changed the entry */
  s = run_cmd(DZ_CMD_ON, "7", true);
  CHECK(s.cmdev.reject == DZ_CMDE_PROTECTED && s.cmdev.outcome == DZ_OUT_FAILED);
  s = run_cmd(DZ_CMD_OFF, "6", false);
  CHECK(s.cmdev.reject == DZ_CMDE_NEEDS_CONFIRM);
  /* Group (scene namespace, idx 3 in the mock fixtures): toggles by dz_cmd_scene_tap_kind */
  dz_device_t g;
  CHECK(dz_model_get(dz_service_model(&S), "3", true, &g, NULL, NULL, T) && g.cap == DZ_CAP_GROUP);
  dz_cmd_kind_t k;
  CHECK(dz_cmd_scene_tap_kind(&g, &k) && k == DZ_CMD_SCENE_ON);
  s = run_cmd(k, "3", true);
  CHECK(s.cmdev.reject == 0 && s.cmdev.outcome == DZ_OUT_CONFIRMED && s.cmdev.kind == DZ_CMD_SCENE_ON);
  CHECK(dz_model_get(dz_service_model(&S), "3", true, &g, NULL, NULL, T) && dz_device_state(&g) == DZ_STATE_ON);
  CHECK(dz_cmd_scene_tap_kind(&g, &k) && k == DZ_CMD_SCENE_OFF);
  s = run_cmd(k, "3", true);
  CHECK(s.cmdev.outcome == DZ_OUT_CONFIRMED && dz_model_get(dz_service_model(&S), "3", true, &g, NULL, NULL, T) && dz_device_state(&g) == DZ_STATE_OFF);
  /* Scene (idx 1) is push only: its tap kind is always On */
  dz_device_t sc;
  CHECK(dz_model_get(dz_service_model(&S), "1", true, &sc, NULL, NULL, T) && sc.cap == DZ_CAP_SCENE);
  CHECK(dz_cmd_scene_tap_kind(&sc, &k) && k == DZ_CMD_SCENE_ON);
  s = run_cmd(k, "1", true);
  CHECK(s.cmdev.reject == 0 && s.cmdev.outcome != DZ_OUT_FAILED);
  stop();

  /* viewer token: commands are refused by the server (403), polling continues */
  ctl("reset");
  start("viewer-token", true);
  spin(14);
  drain();
  s = run_cmd(DZ_CMD_ON, "1", false);
  CHECK(s.cmdev.outcome == DZ_OUT_FAILED && s.cmdev.fail == DZ_FAIL_FORBIDDEN && dz_service_conn_state(&S) == DZ_CS_OK);
  stop();

  /* wrong token: AUTH_REJECTED once, polling stops */
  ctl("reset");
  start("nope-token", true);
  spin(4);
  s = drain();
  CHECK(dz_service_conn_state(&S) == DZ_CS_TOKEN_REJECTED && s.auth == 1);
  CHECK(dz_service_run_once(&S, T + 3600000) == DZ_SERVICE_MAX_SLEEP_MS);
  stop();

  /* probe (the RU+CFG connection test) against the mock */
  ru_cfg_t c;
  char info[32];
  memset(&c, 0, sizeof c);
  snprintf(c.url, sizeof c.url, "%s", url);
  c.insecure_lan = true;
  snprintf(c.token, sizeof c.token, "viewer-token");
  CHECK(dz_service_probe(TR, NULL, &c, 3000, info, sizeof info) == RU_CFG_TEST_OK && !strcmp(info, "2026.4 control MCP ok"));
  ctl("set?mode=nomcp");
  CHECK(dz_service_probe(TR, NULL, &c, 3000, info, sizeof info) == RU_CFG_TEST_OK && !strcmp(info, "2026.4 control MCP unavailable"));
  ctl("set?mode=nomcp404");
  CHECK(dz_service_probe(TR, NULL, &c, 3000, info, sizeof info) == RU_CFG_TEST_OK && !strcmp(info, "2026.4 control MCP unavailable"));
  ctl("set?mode=none");
  snprintf(c.token, sizeof c.token, "wrong");
  CHECK(dz_service_probe(TR, NULL, &c, 3000, info, sizeof info) == RU_CFG_TEST_TOKEN_REJECTED && info[0] == 0);
  snprintf(c.token, sizeof c.token, "viewer-token");
  ctl("set?mode=html&count=1");
  CHECK(dz_service_probe(TR, NULL, &c, 3000, info, sizeof info) == RU_CFG_TEST_BAD_RESPONSE);
  ctl("set?mode=drop&count=1");
  CHECK(dz_service_probe(TR, NULL, &c, 3000, info, sizeof info) == RU_CFG_TEST_UNREACHABLE);
  ctl("set?mode=hang&count=1");
  CHECK(dz_service_probe(TR, NULL, &c, 400, info, sizeof info) == RU_CFG_TEST_UNREACHABLE);
  ctl("reset");
  snprintf(c.url, sizeof c.url, "https://127.0.0.1:1"); /* the socket transport cannot do TLS */
  c.insecure_lan = false;
  CHECK(dz_service_probe(TR, NULL, &c, 3000, info, sizeof info) == RU_CFG_TEST_TLS);
  c.insecure_lan = true;
  snprintf(c.url, sizeof c.url, "http://127.0.0.1:1");
  CHECK(dz_service_probe(TR, NULL, &c, 3000, info, sizeof info) == RU_CFG_TEST_UNREACHABLE);
  /* adapter: wifi down short-circuits */
  static bool up;
  bool (*wup)(void *) = NULL;
  dz_probe_ctx_t pc = {TR, NULL, wup, &up};
  snprintf(c.url, sizeof c.url, "%s", url);
  CHECK(dz_service_cfgline_test(&pc, &c, 3000, info, sizeof info) == RU_CFG_TEST_OK);
  ctl("reset");
  dz_transport_destroy(TR);
  MT_DONE();
}
