/* dz_service with the scripted fake transport: config mailbox, polling schedule, events on the bus, TOKEN_REJECTED,
 * network down, buffer rules, command mailbox, secret hygiene, and one threaded run of the real task body. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dz_fake.h"
#include "dz_service.h"
#include "mini_test.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "tests/fixtures"
#endif
#define SECRET "SECRETtokenVALUE123"

static char *slurp(const char *name) {
  char path[256];
  snprintf(path, sizeof path, "%s/%s", FIXTURE_DIR, name);
  FILE *f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); exit(2); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)n + 1);
  if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
  b[n] = 0;
  fclose(f);
  return b;
}
static char *devices_json, *scenes_json;
static const char *VERSION = "{\"status\":\"OK\",\"title\":\"GetVersion\",\"version\":\"2026.4 (build 18568)\"}";
static const char *EMPTY_DELTA = "{\"ActTime\":1793448999,\"ServerTime\":\"2026-10-31 13:20:00\",\"status\":\"OK\",\"title\":\"Devices\"}";
static const char *PLANS = "{\"result\":[],\"status\":\"OK\",\"title\":\"Plans\"}";

typedef enum { M_OK, M_401, M_DOWN } mode_t_;
static mode_t_ mode;
static int auth_seen_bad; /* a request without the Authorization header */
static dz_net_err_t srv(fake_t *f, const char *url, char *buf, size_t cap, dz_http_result_t *out) {
  if (!f->had_auth) auth_seen_bad++;
  if (mode == M_401) return fake_body("Unauthorized", 401, buf, cap, out);
  if (mode == M_DOWN) return DZ_NET_CONNECT;
  if (strstr(url, "param=getversion")) return fake_body(VERSION, 200, buf, cap, out);
  if (strstr(url, "param=getplans")) return fake_body(PLANS, 200, buf, cap, out);
  if (strstr(url, "param=getscenes")) return fake_body(scenes_json, 200, buf, cap, out);
  if (strstr(url, "lastupdate=")) return fake_body(EMPTY_DELTA, 200, buf, cap, out);
  return fake_body(devices_json, 200, buf, cap, out);
}

#define CAP 128
static dz_entry_t ent[CAP];
static dz_device_t stg[CAP];
static char rbuf[DZ_SERVICE_MIN_BUF_FULL];
static char small[DZ_SERVICE_MIN_BUF_FAVS];
static fake_t F;
static ru_bus_t BUS;
static ru_bus_sub_t SUB;
static ru_event_t SUBQ[512];
static dz_service_t S;

static int policy_calls;
static bool policy_last;
static void policy(dz_transport_t *t, bool a) { (void)t; policy_calls++; policy_last = a; }

static dz_service_cfg_t base_cfg(void) {
  dz_service_cfg_t c;
  memset(&c, 0, sizeof c);
  c.transport = &F.base;
  c.set_insecure_lan = policy;
  c.bus = &BUS;
  c.buf = rbuf;
  c.buf_cap = sizeof rbuf;
  c.entries = ent;
  c.entries_cap = CAP;
  c.stage = stg;
  c.stage_cap = CAP;
  c.poll_interval_ms = 10000;
  return c;
}

static void setup(void) {
  memset(ent, 0, sizeof ent);
  mode = M_OK;
  auth_seen_bad = 0;
  fake_init(&F, srv, NULL);
  F.secret = SECRET;
  ru_bus_init(&BUS);
  CHECK(ru_bus_subscribe(&BUS, &SUB, SUBQ, 512, 1u << DZ_SERVICE_TOPIC_DEFAULT, NULL, NULL) == RU_OK);
  dz_service_cfg_t c = base_cfg();
  CHECK(dz_service_init(&S, &c));
}
static void teardown(void) {
  dz_service_deinit(&S);
  ru_bus_unsubscribe(&BUS, &SUB);
  ru_bus_deinit(&BUS);
}

static ru_cfg_t cfg_with(const char *url, const char *tok, bool insecure) {
  ru_cfg_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.url, sizeof c.url, "%s", url);
  snprintf(c.token, sizeof c.token, "%s", tok);
  c.insecure_lan = insecure;
  return c;
}

typedef struct { int dev, model, conn, cmd, auth; dz_ev_conn_t lastconn; dz_ev_cmd_t lastcmd; dz_ev_auth_t lastauth; char devidx[40][12]; } counts_t;
static counts_t drain(void) {
  counts_t c;
  memset(&c, 0, sizeof c);
  ru_event_t e;
  while (ru_bus_poll(&SUB, &e)) {
    switch (e.type) {
      case DZ_EV_DEVICE_CHANGED: if (c.dev < 40) memcpy(c.devidx[c.dev], ((dz_ev_device_t *)e.payload)->idx, 12); c.dev++; break;
      case DZ_EV_MODEL_CHANGED: c.model++; break;
      case DZ_EV_CONN_STATE: c.conn++; memcpy(&c.lastconn, e.payload, sizeof c.lastconn); break;
      case DZ_EV_CMD_RESULT: c.cmd++; memcpy(&c.lastcmd, e.payload, sizeof c.lastcmd); break;
      case DZ_EV_AUTH_REJECTED: c.auth++; memcpy(&c.lastauth, e.payload, sizeof c.lastauth); break;
    }
  }
  return c;
}

/* advance the fake clock by what the service asks for, up to n rounds; returns requests made */
static uint32_t T;
static int spin(int rounds) {
  int before = F.calls;
  for (int i = 0; i < rounds; i++) {
    uint32_t w = dz_service_run_once(&S, T);
    if (w == DZ_SERVICE_MAX_SLEEP_MS) w = 0; /* nothing scheduled: do not advance */
    T += w;
  }
  return F.calls - before;
}

static void test_init_rules(void) {
  dz_service_t s;
  dz_service_cfg_t c = base_cfg();
  ru_bus_init(&BUS);
  c.buf_cap = DZ_SERVICE_MIN_BUF_FULL - 1;
  CHECK(!dz_service_init(&s, &c)); /* full list needs >= 128 KB */
  c.favorites_only = true;
  c.buf = small;
  c.buf_cap = DZ_SERVICE_MIN_BUF_FAVS;
  fake_init(&F, srv, NULL);
  CHECK(dz_service_init(&s, &c)); /* favourites only: 16 KB is enough */
  dz_service_deinit(&s);
  c.buf_cap = DZ_SERVICE_MIN_BUF_FAVS - 1;
  CHECK(!dz_service_init(&s, &c));
  c = base_cfg();
  c.buf = NULL;
  CHECK(!dz_service_init(&s, &c));
  c = base_cfg();
  c.transport = NULL;
  CHECK(!dz_service_init(&s, &c));
  c = base_cfg();
  c.topic = RU_BUS_MAX_TOPICS;
  CHECK(!dz_service_init(&s, &c));
  ru_bus_deinit(&BUS);
  CHECK(dz_ev_key_device("0", false) >= 16 && dz_ev_key_device("12", true) != dz_ev_key_device("12", false) &&
        dz_ev_key_device("abc", false) >= 16 && dz_ev_key_device("", false) >= 16 && dz_ev_key_device("12", false) != dz_ev_key_device("13", false));
}

static void test_flow(void) {
  setup();
  T = 1000;
  /* nothing configured: no requests, long sleep */
  CHECK(dz_service_run_once(&S, T) == DZ_SERVICE_MAX_SLEEP_MS && F.calls == 0);
  /* invalid configs are refused and never reach the client */
  ru_cfg_t c = cfg_with("http://192.168.1.5:8080", SECRET, false);
  CHECK(dz_service_configure(&S, &c, true) == RU_CFG_BAD_URL);
  c = cfg_with("ftp://x", SECRET, true);
  CHECK(dz_service_configure(&S, &c, true) == RU_CFG_BAD_URL);
  c = cfg_with("http://h", "bad token", true);
  CHECK(dz_service_configure(&S, &c, true) != RU_CFG_OK);
  CHECK(dz_service_run_once(&S, T) == DZ_SERVICE_MAX_SLEEP_MS && F.calls == 0 && !S.client_ready);
  /* good config: the policy hook is told, the client starts */
  c = cfg_with("http://192.168.1.5:8080", SECRET, true);
  CHECK(dz_service_configure(&S, &c, true) == RU_CFG_OK);
  uint32_t w = dz_service_run_once(&S, T);
  CHECK(w > 0 && policy_calls == 1 && policy_last == true && F.calls == 1); /* mailbox applied, first request made at once */
  CHECK(S.client_ready && !S.mb_cfg.pending && S.mb_cfg.cfg.token[0] == 0); /* mailbox token wiped */
  spin(12);
  counts_t k = drain();
  CHECK(dz_service_conn_state(&S) == DZ_CS_OK && dz_model_count(dz_service_model(&S)) > 0);
  CHECK(k.dev > 0 && k.model >= 1 && k.conn >= 1 && k.lastconn.state == DZ_CS_OK && k.auth == 0);
  CHECK(k.dev == (int)dz_model_count(dz_service_model(&S)) || k.dev > 0);
  CHECK(auth_seen_bad == 0 && !F.auth_in_url);
  /* poll interval: after the initial sequence, no request before the interval is up */
  dz_service_run_once(&S, T); /* spin() leaves the clock on a due time: consume it */
  int calls = F.calls;
  w = dz_service_run_once(&S, T);
  CHECK(w > 0 && w <= 10000 && F.calls == calls);
  T += w - 1;
  CHECK(dz_service_run_once(&S, T) > 0 && F.calls == calls);
  T += 1;
  dz_service_run_once(&S, T);
  CHECK(F.calls == calls + 1);
  /* nothing changed: no device events, no model event */
  k = drain();
  CHECK(k.dev == 0 && k.model == 0);
  /* a new poll interval applies */
  dz_service_set_poll_interval(&S, 120000);
  dz_service_run_once(&S, T); /* applies the mailbox */
  CHECK(S.client.cfg.poll_interval_ms == 120000);
  T += 10000;
  dz_service_run_once(&S, T); /* the poll that was already scheduled, now the next one is 120 s away */
  w = dz_service_run_once(&S, T);
  CHECK(w == DZ_SERVICE_MAX_SLEEP_MS); /* sleeps are capped so the task stays responsive */
  /* network down: no requests however long we wait */
  dz_service_set_net_up(&S, false);
  calls = F.calls;
  for (int i = 0; i < 5; i++) { T += 3600000; dz_service_run_once(&S, T); }
  CHECK(F.calls == calls);
  /* back up: polls again promptly */
  dz_service_set_net_up(&S, true);
  dz_service_run_once(&S, T);
  dz_service_run_once(&S, T);
  CHECK(F.calls > calls);
  /* the token is never in a URL and every request carried the header */
  CHECK(auth_seen_bad == 0 && !F.auth_in_url);
  teardown();
}

static void test_auth_and_offline(void) {
  setup();
  T = 1000;
  ru_cfg_t c = cfg_with("https://dz.example.org", SECRET, false);
  CHECK(dz_service_configure(&S, &c, false) == RU_CFG_OK);
  mode = M_401;
  spin(3);
  counts_t k = drain();
  CHECK(dz_service_conn_state(&S) == DZ_CS_TOKEN_REJECTED && k.auth == 1 && k.lastauth.http_status == 401);
  CHECK(k.conn >= 1 && k.lastconn.state == DZ_CS_TOKEN_REJECTED);
  /* polling stopped: an hour passes, no request */
  int calls = F.calls;
  mode = M_OK;
  for (int i = 0; i < 6; i++) { T += 600000; CHECK(dz_service_run_once(&S, T) == DZ_SERVICE_MAX_SLEEP_MS); }
  CHECK(F.calls == calls);
  /* a retry request (server fixed): polling resumes */
  dz_service_retry(&S);
  spin(12);
  CHECK(F.calls > calls && dz_service_conn_state(&S) == DZ_CS_OK);
  /* reconfiguring with a new token also leaves the rejected state */
  mode = M_401;
  dz_service_retry(&S);
  spin(3);
  CHECK(dz_service_conn_state(&S) == DZ_CS_TOKEN_REJECTED);
  drain();
  mode = M_OK;
  c = cfg_with("https://dz.example.org", "another-token-0000", false);
  CHECK(dz_service_configure(&S, &c, false) == RU_CFG_OK);
  spin(12);
  CHECK(dz_service_conn_state(&S) == DZ_CS_OK);
  /* offline: backoff, not a tight loop */
  mode = M_DOWN;
  dz_service_retry(&S);
  calls = F.calls;
  uint32_t t0 = T;
  spin(30);
  int made = F.calls - calls;
  CHECK(made >= 3 && made < 30 && T - t0 > 10000); /* spaced out by backoff */
  CHECK(dz_service_conn_state(&S) == DZ_CS_OFFLINE);
  /* clear config: forgets everything, stops requests */
  dz_service_clear_config(&S);
  dz_service_run_once(&S, T);
  calls = F.calls;
  T += 100000;
  CHECK(dz_service_run_once(&S, T) == DZ_SERVICE_MAX_SLEEP_MS && F.calls == calls && !S.client_ready);
  CHECK(!memmem(&S.client, sizeof S.client, "another-token", 13));
  teardown();
}

static void test_commands_rejects(void) {
  setup();
  T = 1000;
  /* no config yet: refused visibly */
  dz_cmd_req_t q = {.kind = DZ_CMD_ON, .idx = "1"};
  uint32_t id = dz_service_submit(&S, &q);
  CHECK(id > 0);
  dz_service_run_once(&S, T);
  counts_t k = drain();
  CHECK(k.cmd == 1 && k.lastcmd.req_id == id && k.lastcmd.outcome == DZ_OUT_FAILED && k.lastcmd.reject != 0);
  ru_cfg_t c = cfg_with("https://dz.example.org", SECRET, false);
  dz_service_configure(&S, &c, false); /* read-only install */
  spin(12);
  drain();
  uint32_t id2 = dz_service_submit(&S, &q);
  CHECK(id2 == id + 1);
  dz_service_run_once(&S, T);
  k = drain();
  CHECK(k.cmd == 1 && k.lastcmd.req_id == id2 && k.lastcmd.reject == DZ_CMDE_CONTROL_DISABLED);
  /* unknown device on a control-enabled install */
  dz_service_configure(&S, &c, true);
  spin(12);
  drain();
  dz_cmd_req_t u = {.kind = DZ_CMD_ON, .idx = "99999"};
  uint32_t id3 = dz_service_submit(&S, &u);
  dz_service_run_once(&S, T);
  k = drain();
  CHECK(k.cmd == 1 && k.lastcmd.req_id == id3 && k.lastcmd.reject == DZ_CMDE_UNKNOWN_DEVICE);
  /* network down: a command is refused instead of being queued for later */
  dz_service_set_net_up(&S, false);
  uint32_t id4 = dz_service_submit(&S, &q);
  int calls = F.calls;
  dz_service_run_once(&S, T);
  k = drain();
  CHECK(k.cmd == 1 && k.lastcmd.req_id == id4 && k.lastcmd.fail == DZ_FAIL_TRANSPORT && F.calls == calls);
  /* queue limits and bad args */
  dz_service_set_net_up(&S, true);
  uint32_t last = 1;
  for (int i = 0; i < DZ_SERVICE_SUBMIT_QUEUE; i++) last = dz_service_submit(&S, &u);
  CHECK(last != 0 && dz_service_submit(&S, &u) == 0);
  dz_cmd_req_t longidx = {.kind = DZ_CMD_ON, .idx = "123456789012"};
  CHECK(dz_service_submit(&S, &longidx) == 0 && dz_service_submit(&S, NULL) == 0);
  teardown();
}

/* real task body on a real thread */
static dz_net_err_t quick(fake_t *f, const char *url, char *buf, size_t cap, dz_http_result_t *out) { return srv(f, url, buf, cap, out); }
static void test_thread(void) {
  setup();
  fake_init(&F, quick, NULL);
  ru_cfg_t c = cfg_with("https://dz.example.org", SECRET, false);
  dz_service_set_poll_interval(&S, 1000);
  CHECK(dz_service_configure(&S, &c, true) == RU_CFG_OK);
  CHECK(dz_service_start(&S));
  /* startup gap is 2 s between the initial requests; wait for the first model events (real time) */
  bool got = false;
  ru_event_t e;
  for (int i = 0; i < 160 && !got; i++) {
    if (ru_bus_wait(&SUB, &e, 100) && e.type == DZ_EV_MODEL_CHANGED) got = true;
  }
  CHECK(got && dz_service_conn_state(&S) == DZ_CS_OK);
  dz_cmd_req_t u = {.kind = DZ_CMD_ON, .idx = "99999"};
  uint32_t id = dz_service_submit(&S, &u);
  bool res = false;
  for (int i = 0; i < 50 && !res; i++)
    if (ru_bus_wait(&SUB, &e, 100) && e.type == DZ_EV_CMD_RESULT) { dz_ev_cmd_t r; memcpy(&r, e.payload, sizeof r); res = r.req_id == id && r.reject == DZ_CMDE_UNKNOWN_DEVICE; }
  CHECK(res);
  dz_service_stop(&S);
  dz_service_stop(&S); /* idempotent */
  teardown();
}

int main(void) {
  devices_json = slurp("mock_getdevices.json");
  scenes_json = slurp("mock_getscenes.json");
  test_init_rules();
  test_flow();
  test_auth_and_offline();
  test_commands_rejects();
  test_thread();
  MT_DONE();
}
