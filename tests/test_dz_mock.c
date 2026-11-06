/* Integration: real socket transport + client + model + commands against sim/mock/domoticz_mock.js.
 * Run through tests/run_with_mock.sh (ctest does). The clock is simulated; the sockets are real. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dz_client.h"
#include "dz_cmd.h"
#include "mini_test.h"
#include "ru_osal.h"

static char base[64];
static char rbuf[400 * 1024];
static char small[2048];

#define CAP 64
static dz_entry_t ent[CAP];
static dz_device_t stg[CAP];
static dz_model_t M;
static dz_client_t C;
static dz_cmd_t K;
static dz_transport_t *TR;
static dz_cmd_result_t LASTR;
static int nres;
static void on_res(const dz_cmd_result_t *r, void *u) { (void)u; LASTR = *r; nres++; }

static dz_net_err_t get(const char *path, const char *tok, uint32_t tmo, char *buf, size_t cap, dz_http_result_t *r) {
  char url[256], hdr[128];
  const char *h[2] = {NULL, NULL};
  snprintf(url, sizeof url, "%s%s", base, path);
  if (tok) { snprintf(hdr, sizeof hdr, "Authorization: Bearer %s", tok); h[0] = hdr; }
  return TR->get(TR, url, tok ? h : NULL, tmo, buf, cap, r);
}
static void ctl(const char *q) {
  char p[128];
  dz_http_result_t r;
  snprintf(p, sizeof p, "/__mock/%s", q);
  CHECK(get(p, NULL, 3000, small, sizeof small, &r) == DZ_NET_OK && r.status == 200);
}

static void test_transport(void) {
  dz_http_result_t r;
  const char *V = "viewer-token";
  ctl("reset");
  CHECK(get("/json.htm?type=command&param=getversion", V, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_OK && r.status == 200);
  CHECK(strstr(rbuf, "2026.4 (build 18568)") && r.len == strlen(rbuf));
  CHECK(get("/json.htm?type=command&param=getversion", NULL, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_OK && r.status == 401);
  CHECK(get("/nothing", V, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_OK && r.status == 404);
  ctl("set?mode=chunked");
  CHECK(get("/json.htm?type=command&param=getversion", V, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_OK && strstr(rbuf, "\"title\":\"GetVersion\""));
  ctl("set?mode=html");
  CHECK(get("/json.htm?type=command&param=getversion", V, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_OK && r.status == 200 && rbuf[0] == '<');
  ctl("set?mode=drop");
  CHECK(get("/json.htm?type=command&param=getversion", V, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_CONNECT && r.len == 0 && rbuf[0] == 0);
  ctl("set?mode=truncate");
  CHECK(get("/json.htm?type=command&param=getversion", V, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_CONNECT && rbuf[0] == 0);
  ctl("set?mode=hang&count=1");
  uint32_t t0 = ru_now_ms();
  CHECK(get("/json.htm?type=command&param=getversion", V, 400, rbuf, sizeof rbuf, &r) == DZ_NET_TIMEOUT);
  CHECK(ru_now_ms() - t0 < 1500);
  ctl("set?mode=slowbody&count=1");
  t0 = ru_now_ms();
  CHECK(get("/json.htm?type=command&param=getversion", V, 400, rbuf, sizeof rbuf, &r) == DZ_NET_TIMEOUT && ru_now_ms() - t0 < 1500);
  ctl("set?mode=huge&count=1");
  CHECK(get("/json.htm?type=command&param=getversion", V, 3000, small, sizeof small, &r) == DZ_NET_TOO_LARGE && r.status == 200
        && r.len == sizeof small - 1 && small[sizeof small - 1] == 0);
  ctl("set?mode=garbage&count=1");
  CHECK(get("/json.htm?type=command&param=getversion", V, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_OK && dz_parse_response(rbuf, r.len, NULL, NULL, NULL) == DZ_ERR_SYNTAX);
  ctl("reset");
  /* refusals that never touch the network */
  CHECK(TR->get(TR, "https://127.0.0.1/", NULL, 1000, small, sizeof small, &r) == DZ_NET_TLS);
  CHECK(TR->get(TR, "ftp://x/", NULL, 1000, small, sizeof small, &r) == DZ_NET_CONNECT);
  CHECK(TR->get(TR, "http://127.0.0.1:1/", NULL, 1000, small, sizeof small, &r) == DZ_NET_CONNECT); /* refused */
  const char *evil[] = {"Authorization: Bearer x\r\nX-Evil: 1", NULL};
  CHECK(TR->get(TR, "http://127.0.0.1:1/", evil, 1000, small, sizeof small, &r) == DZ_NET_CONNECT);
  char url[300];
  snprintf(url, sizeof url, "%s/json.htm?x=1 HTTP/1.1\r\nEvil: 1", base);
  CHECK(TR->get(TR, url, NULL, 1000, small, sizeof small, &r) == DZ_NET_CONNECT);
  /* buffer of 1 */
  char one[1];
  CHECK(get("/json.htm?type=command&param=getversion", V, 3000, one, 1, &r) == DZ_NET_TOO_LARGE && one[0] == 0);
}

static uint32_t T;
/* POST over the real socket transport: the session header is captured, the body reaches the mock. */
static void test_post(void) {
  dz_http_result_t r;
  char url[256], auth[64], sid[80] = "x", sidh[120];
  snprintf(url, sizeof url, "%s/mcp", base);
  snprintf(auth, sizeof auth, "Authorization: Bearer %s", "user-token");
  const char *h0[] = {auth, "Accept: application/json, text/event-stream", NULL};
  static const char init2[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"x\"}}}";
  CHECK(TR->post != NULL);
  ctl("reset");
  dz_post_t p = {init2, strlen(init2), "application/json", "Mcp-Session-Id", sid, sizeof sid};
  CHECK(TR->post(TR, url, h0, &p, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_OK && r.status == 200 && strstr(rbuf, "protocolVersion"));
  CHECK(strlen(sid) == 32);
  snprintf(sidh, sizeof sidh, "Mcp-Session-Id: %s", sid);
  const char *h1[] = {auth, "Accept: application/json", sidh, NULL};
  static const char call[] = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"set_switch_state\",\"arguments\":{\"idx\":1,\"state\":\"On\"}}}";
  dz_post_t c = {call, strlen(call), NULL, NULL, NULL, 0};
  CHECK(TR->post(TR, url, h1, &c, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_OK && r.status == 200 && strstr(rbuf, "\"isError\":false"));
  CHECK(get("/json.htm?type=command&param=getdevices&rid=1", "viewer-token", 3000, rbuf, sizeof rbuf, &r) == DZ_NET_OK && strstr(rbuf, "\"Status\": \"On\""));
  /* no session -> JSON-RPC error, still HTTP 200 */
  CHECK(TR->post(TR, url, h0, &c, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_OK && r.status == 200 && strstr(rbuf, "Missing session"));
  /* no capture asked: hdr_out stays untouched/cleared */
  sid[0] = 'q';
  dz_post_t nocap = {call, strlen(call), NULL, NULL, sid, sizeof sid};
  CHECK(TR->post(TR, url, h1, &nocap, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_OK && sid[0] == 0);
  /* no token: 401 and no header */
  const char *none[] = {NULL};
  dz_post_t p2 = {init2, strlen(init2), NULL, "Mcp-Session-Id", sid, sizeof sid};
  CHECK(TR->post(TR, url, none, &p2, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_OK && r.status == 401 && sid[0] == 0);
  /* a dropped connection clears the header and reports connect */
  ctl("set?mode=drop&count=1");
  sid[0] = 'q';
  CHECK(TR->post(TR, url, h0, &p, 3000, rbuf, sizeof rbuf, &r) == DZ_NET_CONNECT && sid[0] == 0 && rbuf[0] == 0);
  ctl("set?mode=hang&count=1");
  uint32_t t0 = ru_now_ms();
  CHECK(TR->post(TR, url, h0, &p, 400, rbuf, sizeof rbuf, &r) == DZ_NET_TIMEOUT && ru_now_ms() - t0 < 1500);
  /* refusals that never touch the network */
  CHECK(TR->post(TR, "https://127.0.0.1/mcp", h0, &p, 1000, small, sizeof small, &r) == DZ_NET_TLS);
  const char *evil[] = {"Authorization: Bearer x\r\nX-Evil: 1", NULL};
  CHECK(TR->post(TR, "http://127.0.0.1:1/mcp", evil, &p, 1000, small, sizeof small, &r) == DZ_NET_CONNECT);
  ctl("reset");
}

static void setup(const char *tok, bool favs) {
  if (M.mu) dz_model_deinit(&M);
  dz_model_cfg_t mc = {.ttl_ms = 600000, .old_update_s = UINT32_MAX};
  dz_model_init(&M, ent, CAP, stg, CAP, &mc);
  dz_client_cfg_t cfg = {.base_url = base, .token = tok, .transport = TR, .model = &M, .buf = rbuf, .buf_cap = sizeof rbuf,
                         .favorites_only = favs, .timeout_ms = 1000, .poll_interval_ms = 10000, .startup_gap_ms = 1000};
  CHECK(dz_client_init(&C, &cfg));
  dz_cmd_init(&K, &C, &M, true, on_res, NULL);
  T = 0;
}
static int pump_client(int n) {
  int made = 0;
  for (int i = 0; i < n; i++) {
    uint32_t w = dz_client_ms_until_due(&C, T);
    if (w == UINT32_MAX) break;
    T += w;
    made += dz_client_step(&C, T);
  }
  return made;
}

static void test_client(void) {
  ctl("reset");
  setup("viewer-token", false);
  CHECK(pump_client(5) == 5);
  CHECK(dz_client_state(&C) == DZ_CS_OK && C.version.year == 2026 && C.version.minor == 4 && C.version.build == 18568);
  CHECK(!strcmp(C.sunrise, "07:35") && !strcmp(C.sunset, "17:14") && C.cursor > 1700000000 && C.n_plans == 1);
  size_t n = dz_model_count(&M);
  CHECK(n >= 20);
  /* getconfig was read once after getversion: the metric mock says C and m/s, nothing else is kept */
  dz_units_t un;
  CHECK(dz_model_units(&M, &un) && un.have_temp && !strcmp(un.temp, "C") && un.have_wind && !strcmp(un.wind, "m/s"));
  dz_device_t d; ru_val_t v; ru_val_state_t st;
  CHECK(dz_model_get(&M, "30", false, &d, &v, &st, T) && st == RU_VAL_STALE && d.timeout);                   /* stale sensor */
  CHECK(dz_model_get(&M, "8", false, &d, &v, &st, T) && d.cap == DZ_CAP_DIMMER);                             /* Color Switch */
  CHECK(dz_model_get(&M, "10", false, &d, &v, &st, T) && d.cap == DZ_CAP_THERMOSTAT && v.valid);            /* Setpoint type */
  CHECK(dz_model_get(&M, "21", false, &d, &v, &st, T) && d.cap == DZ_CAP_ENERGY && !d.controllable);       /* P1 with SwitchTypeVal */
  CHECK(dz_model_get(&M, "23", false, &d, &v, &st, T) && d.cap == DZ_CAP_ENERGY && !d.controllable);       /* RFXMeter, STV 2 */
  CHECK(dz_model_get(&M, "4", false, &d, &v, &st, T) && d.cap == DZ_CAP_SELECTOR && !strcmp(d.level_names, "Off|Low|Med|High"));
  CHECK(dz_model_get(&M, "3", true, &d, &v, &st, T) && d.cap == DZ_CAP_GROUP);                               /* group */
  /* empty delta: the real server omits "result" */
  ru_sleep_ms(1100);
  uint32_t gen = dz_model_generation(&M);
  pump_client(1);
  CHECK(dz_client_state(&C) == DZ_CS_OK && dz_model_count(&M) == n && dz_model_generation(&M) == gen);
}

static void test_client_states(void) {
  ctl("reset");
  setup("viewer-token", false);
  pump_client(5);
  size_t n = dz_model_count(&M);
  ctl("set?mode=html");
  pump_client(2);
  CHECK(dz_client_state(&C) == DZ_CS_BAD_RESPONSE && dz_model_count(&M) == n);
  ctl("set?mode=http500");
  pump_client(1);
  CHECK(dz_client_state(&C) == DZ_CS_SERVER_ERROR);
  ctl("set?mode=truncate");
  pump_client(1);
  CHECK(dz_client_state(&C) == DZ_CS_OFFLINE || dz_client_state(&C) == DZ_CS_SERVER_ERROR || dz_client_state(&C) == DZ_CS_OK);
  ctl("set?mode=drop");
  pump_client(4);
  CHECK(dz_client_state(&C) == DZ_CS_OFFLINE && C.last_err == DZ_CERR_CONNECT);
  ctl("set?mode=none");
  pump_client(1);
  CHECK(dz_client_state(&C) == DZ_CS_OK && dz_model_count(&M) == n);
  ctl("set?mode=403");
  pump_client(1);
  CHECK(dz_client_state(&C) == DZ_CS_TOKEN_REJECTED);
  int calls = (int)C.requests;
  for (int i = 0; i < 50; i++) { T += 100000; dz_client_step(&C, T); }
  CHECK((int)C.requests == calls);
  ctl("set?mode=none");
  dz_client_set_token(&C, "viewer-token");
  pump_client(1);
  CHECK(dz_client_state(&C) == DZ_CS_OK);
  /* wrong token on a healthy server */
  setup("not-a-token", false);
  pump_client(1);
  CHECK(dz_client_state(&C) == DZ_CS_TOKEN_REJECTED);
  /* latency below the timeout is fine, above it is OFFLINE not a hang */
  ctl("set?latency=200");
  setup("viewer-token", false);
  pump_client(3);
  CHECK(dz_client_state(&C) == DZ_CS_OK);
  ctl("set?latency=1500");
  setup("viewer-token", false);
  pump_client(1);
  CHECK(C.last_err == DZ_CERR_TIMEOUT);
  ctl("reset");
  /* favourites only */
  setup("viewer-token", true);
  pump_client(3);
  CHECK(dz_model_count(&M) > 0 && dz_model_count(&M) < n);
  /* tiny buffer: bounded, reported, model untouched */
  setup("viewer-token", false);
  C.cfg.buf_cap = 4096;
  pump_client(3);
  CHECK(C.last_err == DZ_CERR_TOO_LARGE && dz_model_count(&M) == 0 && dz_client_state(&C) == DZ_CS_BAD_RESPONSE);
}

static dz_cmd_err_t run(dz_cmd_kind_t kind, const char *idx, int level, float sp, bool conf) {
  dz_cmd_req_t q = {.kind = kind, .idx = idx, .level = level, .setpoint = sp, .confirmed = conf};
  nres = 0;
  dz_cmd_err_t e = dz_cmd_submit(&K, &q, T, NULL);
  for (int i = 0; e == DZ_CMD_ACCEPTED && i < 10 && dz_cmd_pending(&K); i++) {
    T += dz_cmd_ms_until_due(&K, T);
    dz_cmd_pump(&K, T);
  }
  return e;
}

static int nposts(void) {
  dz_http_result_t r;
  const char *k = "\"mcpInits\":";
  (void)k;
  if (get("/__mock/stats", NULL, 3000, small, sizeof small, &r) != DZ_NET_OK) return -1;
  const char *p = strstr(small, "\"mcpCalls\":[");
  int n = 0;
  for (; p && (p = strstr(p + 1, "\"tool\"")); ) n++;
  return n;
}

static void test_commands(void) {
  ctl("reset");
  setup("user-token", false);
  pump_client(5);
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false) == DZ_CMD_ACCEPTED && nres == 1 && LASTR.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_SET_LEVEL, "2", 250, 0, false) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED && LASTR.level_sent == 100);
  CHECK(run(DZ_CMD_SET_LEVEL, "8", 60, 0, false) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED);   /* Color Switch */
  CHECK(run(DZ_CMD_SET_LEVEL, "2", 0, 0, false) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_SELECTOR, "4", 30, 0, false) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_BLINDS_CLOSE, "3", 0, 0, false) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED);
  /* push button: the mock (like the real tool, we assume) skips a call whose state already matches: its own outcome */
  CHECK(run(DZ_CMD_PUSH, "5", 0, 0, false) == DZ_CMD_ACCEPTED && nres == 1 && LASTR.outcome == DZ_OUT_NO_ACTION && LASTR.no_action);
  ctl("set?push=force");
  CHECK(run(DZ_CMD_PUSH, "5", 0, 0, false) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED);
  ctl("set?push=skip");
  CHECK(run(DZ_CMD_OFF, "6", 0, 0, false) == DZ_CMDE_NEEDS_CONFIRM);
  CHECK(run(DZ_CMD_OFF, "6", 0, 0, true) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED && !strcmp(LASTR.after.status, "Unlocked"));
  CHECK(run(DZ_CMD_ON, "6", 0, 0, true) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED && !strcmp(LASTR.after.status, "Locked"));
  CHECK(run(DZ_CMD_ON, "9", 0, 0, true) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED);           /* Lighting 2 type */
  CHECK(run(DZ_CMD_SETPOINT, "10", 0, 22.5f, true) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_SCENE_ON, "3", 0, 0, true) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_SCENE_OFF, "3", 0, 0, true) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED);
  CHECK(run(DZ_CMD_ON, "7", 0, 0, true) == DZ_CMDE_PROTECTED);
  CHECK(run(DZ_CMD_ON, "11", 0, 0, true) == DZ_CMDE_NOT_CONTROLLABLE);
  /* the mock changes behind our back: the next delta shows it */
  ctl("touch?idx=1&status=Off");
  ru_sleep_ms(1100);
  pump_client(1);
  dz_device_t d;
  dz_http_result_t r0;
  CHECK(dz_model_get(&M, "1", false, &d, NULL, NULL, T) && !strcmp(d.status, "Off"));
  /* the mock refuses and says HTTP OK + status ERR: FAILED, not CONFIRMED */
  ctl("set?mode=statuserr&count=1");
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_FAILED && LASTR.fail == DZ_FAIL_SERVER);
  /* the endpoint skips the Protected check: the pre-flight read catches a device that became protected after the last poll */
  ctl("touch?idx=1&protected=1");
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_FAILED && LASTR.fail == DZ_FAIL_PROTECTED);
  CHECK(get("/json.htm?type=command&param=getdevices&rid=1", "viewer-token", 3000, rbuf, sizeof rbuf, &r0) == DZ_NET_OK && strstr(rbuf, "\"Status\": \"Off\""));
  ctl("touch?idx=1&protected=0");
  ru_sleep_ms(1100);
  pump_client(1); /* the model learns that it is unprotected again */
  /* a server started with -nomcp */
  ctl("set?mode=nomcp");
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_FAILED && LASTR.fail == DZ_FAIL_NO_MCP);
  ctl("set?mode=nomcp404");
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_FAILED && LASTR.fail == DZ_FAIL_NO_MCP);
  ctl("set?mode=none");
  /* the session is dropped by the server: one re-initialize, the command still lands */
  ctl("set?mcp=expire");
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_CONFIRMED);
  /* a viewer token cannot switch: 403 -> FAILED(forbidden), polling is NOT stopped */
  ctl("reset");
  setup("viewer-token", false);
  pump_client(5);
  CHECK(run(DZ_CMD_ON, "1", 0, 0, false) == DZ_CMD_ACCEPTED && LASTR.outcome == DZ_OUT_FAILED && LASTR.fail == DZ_FAIL_FORBIDDEN);
  CHECK(dz_client_state(&C) == DZ_CS_OK && !C.halted);
  /* the token never appeared in anything the mock logged */
  dz_http_result_t r;
  CHECK(get("/__mock/stats", NULL, 3000, small, sizeof small, &r) == DZ_NET_OK && !strstr(small, "viewer-token") && !strstr(small, "user-token"));
  ctl("reset");
}

/* Imperial install with the hand-written climate set: units, plan lookup, F setpoints, thermostat mode. */
static void test_climate_mock(void) {
  ctl("reset");
  ctl("set?units=imperial&climate=1");
  setup("user-token", false);
  pump_client(5);
  dz_units_t un;
  CHECK(dz_model_units(&M, &un) && un.have_temp && !strcmp(un.temp, "F") && un.have_wind && !strcmp(un.wind, "mph"));
  dz_device_t d, s2;
  ru_val_t v;
  ru_val_state_t st;
  CHECK(dz_model_get(&M, "118", false, &d, &v, &st, T) && d.cap == DZ_CAP_THERMOSTAT && d.can_set && !strcmp(d.vunit, "\xC2\xB0" "F"));
  CHECK(d.setpoint_min > 40.9f && d.setpoint_max < 86.1f);
  /* plan lookup: one sensor in plan 7, two in plan 8, only wind and UV in plan 9 */
  CHECK(dz_model_plan_temp(&M, &d, &s2, &v, &st, T) && !strcmp(s2.idx, "100") && dz_dev_room_temp(&s2) && dz_dev_room_temp(&s2)->v > 70.6f);
  CHECK(dz_model_get(&M, "121", false, &d, &v, &st, T) && !dz_model_plan_temp(&M, &d, &s2, &v, &st, T));
  CHECK(dz_model_get(&M, "122", false, &d, &v, &st, T) && !dz_model_plan_temp(&M, &d, &s2, &v, &st, T));
  CHECK(dz_model_get(&M, "104", false, &d, &v, &st, T) && dz_dev_reading(&d, DZ_RK_AUXTEMP) && !dz_dev_room_temp(&d));
  /* setpoint in Fahrenheit: read-after-write confirms, values above max are clamped to the device's range */
  /* Fahrenheit server: the MCP setpoint tool takes Celsius only (it would store 70 as 70 C), so every setpoint is read-only,
   * and so is the thermostat mode (no MCP tool). Nothing is sent. */
  int sent = nposts();
  CHECK(!dz_cmd_supported(&d, DZ_CMD_SETPOINT, NULL));
  CHECK(run(DZ_CMD_SETPOINT, "118", 0, 70.0f, true) == DZ_CMDE_UNSUPPORTED);
  CHECK(run(DZ_CMD_SETPOINT, "123", 0, 70.0f, true) == DZ_CMDE_UNSUPPORTED && run(DZ_CMD_SETPOINT, "124", 0, 70.0f, true) == DZ_CMDE_UNSUPPORTED);
  CHECK(run(DZ_CMD_MODE, "125", 2, 0, true) == DZ_CMDE_UNSUPPORTED && run(DZ_CMD_MODE, "125", 9, 0, true) == DZ_CMDE_UNSUPPORTED);
  CHECK(nposts() == sent);
  CHECK(dz_model_get(&M, "125", false, &d, &v, &st, T) && d.cap == DZ_CAP_MODE && !dz_cmd_supported(&d, DZ_CMD_MODE, NULL));
  /* a setpoint to a mode device is refused */
  CHECK(run(DZ_CMD_SETPOINT, "125", 0, 20.0f, true) == DZ_CMDE_UNSUPPORTED || run(DZ_CMD_SETPOINT, "125", 0, 20.0f, true) == DZ_CMDE_NOT_CONTROLLABLE);
  ctl("reset");
}

/* filter=light|temp|weather|utility: the mock (written independently in JS from Domoticz's Helper.cpp) and dz_tabs_of
 * must list exactly the same devices, on the basic and on the climate data set. */
typedef struct { dz_device_t d[96]; int n; } all_t;
static all_t ALL;
static bool grab(const dz_device_t *d, void *u) {
  all_t *a = u;
  if (a->n < 96) a->d[a->n++] = *d;
  return true;
}
static int fetch_filter(const char *filter, all_t *out) {
  char url[160];
  dz_http_result_t r;
  out->n = 0;
  snprintf(url, sizeof url, "/json.htm?type=command&param=getdevices&filter=%s&used=true&order=Name", filter);
  if (get(url, "viewer-token", 3000, rbuf, sizeof rbuf, &r) != DZ_NET_OK || r.status != 200) return -1;
  return dz_parse_response(rbuf, r.len, grab, out, NULL) == DZ_OK ? out->n : -1;
}
static void test_tab_filters(void) {
  static all_t part;
  static const struct { const char *name; unsigned bit; } F[] = {
    { "light", DZ_TAB_LIGHT }, { "temp", DZ_TAB_TEMP }, { "weather", DZ_TAB_WEATHER }, { "utility", DZ_TAB_UTILITY },
  };
  for (int pass = 0; pass < 2; pass++) {
    ctl("reset");
    if (pass) ctl("set?climate=1");
    CHECK(fetch_filter("all", &ALL) > 10);
    int overlaps = 0;
    for (size_t f = 0; f < 4; f++) {
      int np = fetch_filter(F[f].name, &part);
      CHECK(np >= 0);
      int want = 0;
      for (int i = 0; i < ALL.n; i++) {
        if (dz_cap_is_scene_ns(ALL.d[i].cap)) continue; /* filter=all appends scenes and groups (their own idx space) */
        bool in_tab = (dz_tabs_of(ALL.d[i].type, ALL.d[i].subtype) & F[f].bit) != 0;
        /* dz_device_t.type is cut at 23 characters, which no Type string reaches; subtype only matters below that */
        bool listed = false;
        for (int k = 0; k < part.n; k++) if (!strcmp(part.d[k].idx, ALL.d[i].idx)) listed = true;
        CHECK(in_tab == listed);
        if (in_tab != listed) printf("  ^ %s idx %s %s / %s: dz_tabs_of=%d mock=%d\n", F[f].name, ALL.d[i].idx, ALL.d[i].type, ALL.d[i].subtype, in_tab, listed);
        want += in_tab;
      }
      CHECK(np == want && np > 0);
    }
    for (int i = 0; i < ALL.n; i++) {
      if (dz_cap_is_scene_ns(ALL.d[i].cap)) continue;
      unsigned t = ALL.d[i].tabs;
      if (t & (t - 1)) overlaps++;
      CHECK(t == dz_tabs_of(ALL.d[i].type, ALL.d[i].subtype)); /* parsed bits equal the bits of the (short enough) strings */
    }
    CHECK(overlaps >= 1); /* THB: Temperature and Weather */
  }
  ctl("reset");
}

/* L3: the curl dev transport passes -q FIRST (no ~/.curlrc) and keeps the token off argv. A stub `curl` on PATH records what it got. */
static void test_curl_stub(void) {
  char dir[] = "/tmp/dzcurlXXXXXX";
  if (!mkdtemp(dir)) { CHECK(0); return; }
  char path[200], cmd[400];
  snprintf(path, sizeof path, "%s/curl", dir);
  FILE *f = fopen(path, "w");
  CHECK(f != NULL);
  if (!f) return;
  fprintf(f, "#!/bin/sh\nprintf '%%s\\n' \"$*\" > %s/argv\ncat > %s/cfg\nprintf '{\"a\":1}\\n200'\n", dir, dir);
  fclose(f);
  chmod(path, 0755);
  char *oldp = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
  snprintf(cmd, sizeof cmd, "%s:%s", dir, oldp ? oldp : "/usr/bin:/bin");
  setenv("PATH", cmd, 1);
  dz_transport_t *ct = dz_transport_curl_create();
  const char *h[2] = {"Authorization: Bearer STUB-TOKEN-XYZ", NULL};
  char buf[256];
  dz_http_result_t r;
  dz_net_err_t e = ct->get(ct, "https://dz.example/json.htm?type=command&param=getversion", h, 5000, buf, sizeof buf, &r);
  CHECK(e == DZ_NET_OK && r.status == 200 && !strcmp(buf, "{\"a\":1}"));
  char line[400] = "", cfg[2000] = "";
  snprintf(cmd, sizeof cmd, "%s/argv", dir);
  f = fopen(cmd, "r"); if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
  snprintf(cmd, sizeof cmd, "%s/cfg", dir);
  f = fopen(cmd, "r"); if (f) { size_t n = fread(cfg, 1, sizeof cfg - 1, f); cfg[n] = 0; fclose(f); }
  CHECK(!strncmp(line, "-q ", 3) && !strstr(line, "STUB-TOKEN") && !strstr(line, "dz.example"));
  CHECK(strstr(cfg, "STUB-TOKEN-XYZ") && strstr(cfg, "url = "));
  /* a header that overflows the 8 KB config fails cleanly (the config buffer is wiped on that path too) */
  static char longh[9000];
  strcpy(longh, "X: ");
  memset(longh + 3, 'a', sizeof longh - 4); longh[sizeof longh - 1] = 0;
  const char *hl[2] = {longh, NULL};
  CHECK(ct->get(ct, "https://dz.example/x", hl, 5000, buf, sizeof buf, &r) == DZ_NET_CONNECT);
  dz_transport_destroy(ct);
  if (oldp) { setenv("PATH", oldp, 1); free(oldp); }
  snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
  CHECK(system(cmd) == 0);
}

int main(void) {
  const char *p = getenv("MOCK_PORT");
  if (!p) { printf("run through tests/run_with_mock.sh\n"); return 2; }
  snprintf(base, sizeof base, "http://127.0.0.1:%s", p);
  TR = dz_transport_socket_create();
  CHECK(TR != NULL);
  test_curl_stub();
  test_transport();
  test_post();
  test_client();
  test_client_states();
  test_commands();
  test_climate_mock();
  test_tab_filters();
  dz_model_deinit(&M);
  dz_transport_destroy(TR);
  MT_DONE();
}
