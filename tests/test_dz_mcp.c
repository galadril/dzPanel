/* MCP session handling in the client (dz_client_mcp_open / dz_client_mcp_call) against a scripted fake transport. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dz_client.h"
#include "dz_fake.h"
#include "mini_test.h"

static const char *SECRET = "SECRET-TOKEN-123";
static const char *SID = "0123456789abcdef0123456789abcdef";

typedef enum { S_OK, S_EXPIRE_ONCE, S_EXPIRE_ALWAYS, S_MISSING_ONCE, S_TIMEOUT_CALL, S_TIMEOUT_INIT, S_CONNECT_INIT, S_401, S_HTML, S_NOSID, S_ERR_INIT, S_503 } smode_t;
static smode_t sm;
static int tools_run;     /* calls that reached the tool */
static int notify_seen;

#define OKR "{\"id\":9,\"jsonrpc\":\"2.0\",\"result\":{\"content\":[{\"text\":\"Switch \\\"x\\\" set to On successfully.\",\"type\":\"text\"}],\"isError\":false}}"

static dz_net_err_t srv(fake_t *f, const char *url, char *buf, size_t cap, dz_http_result_t *out) {
  (void)url;
  bool init = strstr(f->body, "\"initialize\"") != NULL;
  bool notif = strstr(f->body, "notifications/initialized") != NULL;
  if (notif) { notify_seen++; return fake_body("<html><body>Accepted</body></html>", 202, buf, cap, out); }
  if (init) {
    if (sm == S_TIMEOUT_INIT) return DZ_NET_TIMEOUT;
    if (sm == S_CONNECT_INIT) return DZ_NET_CONNECT;
    if (sm == S_503) return fake_body("MCP disabled", 503, buf, cap, out);
    if (sm == S_401) return fake_body("{\"error\":\"Unauthorized\"}", 401, buf, cap, out);
    if (sm == S_EXPIRE_ONCE) sm = S_OK; /* a fresh session heals it */
    if (sm == S_MISSING_ONCE) sm = S_OK;
    if (sm == S_ERR_INIT) return fake_body("{\"id\":1,\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"internal\"}}", 200, buf, cap, out);
    return fake_body("{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{}}}", 200, buf, cap, out);
  }
  switch (sm) {
    case S_EXPIRE_ONCE: case S_EXPIRE_ALWAYS:
      return fake_body("{\"id\":9,\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"Session not found\"}}", 200, buf, cap, out);
    case S_MISSING_ONCE:
      return fake_body("{\"id\":9,\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"Missing session\"}}", 200, buf, cap, out);
    case S_TIMEOUT_CALL: tools_run++; return DZ_NET_TIMEOUT; /* it may have run */
    case S_401: return fake_body("{\"error\":\"Unauthorized\"}", 401, buf, cap, out);
    case S_HTML: return fake_body("<html>proxy</html>", 200, buf, cap, out);
    default: break;
  }
  tools_run++;
  return fake_body(OKR, 200, buf, cap, out);
}

#define CAP 8
static dz_entry_t ent[CAP];
static dz_device_t stg[CAP];
static dz_model_t M;
static char rbuf[4096];
static fake_t F;
static dz_client_t C;

static void setup(void) {
  if (M.mu) dz_model_deinit(&M);
  dz_model_cfg_t mc = {.ttl_ms = 600000, .old_update_s = UINT32_MAX};
  CHECK(dz_model_init(&M, ent, CAP, stg, CAP, &mc));
  fake_init(&F, srv, NULL);
  F.secret = SECRET;
  sm = S_OK;
  F.sid_script = SID;
  tools_run = notify_seen = 0;
  dz_client_cfg_t cfg = {.base_url = "http://dz.example:8080/", .token = SECRET, .transport = &F.base, .model = &M, .buf = rbuf,
                         .buf_cap = sizeof rbuf};
  CHECK(dz_client_init(&C, &cfg));
}

static const char *BODY = "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"toggle_switch_state\",\"arguments\":{\"idx\":1}}}";

static void no_leaks(void) {
  CHECK(!F.auth_in_url && !F.post_secret_leak);
  for (int i = 0; i < F.posts && i < 16; i++) CHECK(!strstr(F.bodies[i], SID) && !strstr(F.bodies[i], SECRET));
  for (int i = 0; i < F.calls && i < 32; i++) CHECK(!strstr(F.urls[i], SID) && !strstr(F.urls[i], SECRET) && !strstr(F.urls[i], "?"));
  CHECK(!F.proto_hdr);
}

static void test_lazy_and_reuse(void) {
  setup();
  CHECK(F.calls == 0 && C.mcp_sid[0] == 0); /* nothing is opened ahead of the first command */
  dz_mcp_result_t r;
  CHECK(dz_client_mcp_call(&C, 1000, BODY, &r) && r.net == DZ_NET_OK && r.http == 200 && !r.reinit && !r.session_lost);
  CHECK(r.reply.has_result && !r.reply.is_error && strstr(r.reply.text, "successfully"));
  CHECK(F.posts == 3 && F.init_calls == 1 && notify_seen == 1); /* initialize, notifications/initialized, the call */
  CHECK(!strcmp(C.mcp_sid, SID));
  CHECK(F.had_auth && F.sid_in_req && !strcmp(F.sid_seen, SID));
  CHECK(!strcmp(fake_last_url(&F), "http://dz.example:8080/mcp"));
  CHECK(strstr(F.bodies[0], "\"initialize\"") && strstr(F.bodies[0], "\"protocolVersion\":\"2025-06-18\""));
  CHECK(strstr(F.bodies[1], "notifications/initialized") && !strstr(F.bodies[1], "\"id\""));
  CHECK(!strcmp(F.bodies[2], BODY));
  for (int i = 0; i < 4; i++) CHECK(dz_client_mcp_call(&C, 2000 + (uint32_t)i * 1000, BODY, &r) && r.reply.has_result);
  CHECK(F.init_calls == 1 && F.posts == 7 && tools_run == 5); /* one session for five commands */
  /* the init request carried no session header; the first call did */
  no_leaks();
}

static void test_idle_retire(void) {
  setup();
  dz_mcp_result_t r;
  CHECK(dz_client_mcp_call(&C, 1000, BODY, &r));
  CHECK(dz_client_mcp_call(&C, 1000 + 54 * 60 * 1000, BODY, &r) && F.init_calls == 1);
  /* idle for more than 55 minutes since the last use: a new session */
  uint32_t last = 1000 + 54 * 60 * 1000;
  CHECK(dz_client_mcp_call(&C, last + 56 * 60 * 1000, BODY, &r) && F.init_calls == 2);
}

static void test_recovery(void) {
  dz_mcp_result_t r;
  setup();
  CHECK(dz_client_mcp_call(&C, 1000, BODY, &r));
  int p0 = F.posts;
  sm = S_EXPIRE_ONCE; /* the server forgot the session: -32001 before the tool runs */
  int run0 = tools_run;
  CHECK(dz_client_mcp_call(&C, 2000, BODY, &r) && r.reinit && !r.session_lost && r.reply.has_result);
  CHECK(F.posts - p0 == 4 && tools_run == run0 + 1); /* call (error), initialize, notification, call: the tool ran once */
  CHECK(F.init_calls == 2);

  setup();
  CHECK(dz_client_mcp_call(&C, 1000, BODY, &r));
  sm = S_MISSING_ONCE;
  CHECK(dz_client_mcp_call(&C, 2000, BODY, &r) && r.reinit && r.reply.has_result && F.init_calls == 2);

  /* a second session error after the retry: reported, session dropped, no third try */
  setup();
  CHECK(dz_client_mcp_call(&C, 1000, BODY, &r));
  sm = S_EXPIRE_ALWAYS;
  p0 = F.posts;
  CHECK(dz_client_mcp_call(&C, 2000, BODY, &r) && r.session_lost && r.reply.has_error && r.reply.code == -32001 && r.reinit);
  CHECK(F.posts - p0 == 4 && C.mcp_sid[0] == 0 && tools_run == 1);
  no_leaks();
}

static void test_failures(void) {
  dz_mcp_result_t r;
  /* timeout on the call: NEVER retried, the stage says it may have run */
  setup();
  CHECK(dz_client_mcp_call(&C, 1000, BODY, &r));
  sm = S_TIMEOUT_CALL;
  int p0 = F.posts;
  CHECK(!dz_client_mcp_call(&C, 2000, BODY, &r) && r.net == DZ_NET_TIMEOUT && r.stage == DZ_MCP_STAGE_CALL && !r.reinit);
  CHECK(F.posts - p0 == 1 && tools_run == 2);
  CHECK(C.mcp_sid[0] != 0); /* the session itself is still good */
  /* timeout / connect failure in initialize */
  setup(); sm = S_TIMEOUT_INIT;
  CHECK(!dz_client_mcp_call(&C, 1000, BODY, &r) && r.net == DZ_NET_TIMEOUT && r.stage == DZ_MCP_STAGE_INIT && F.posts == 1 && tools_run == 0);
  setup(); sm = S_CONNECT_INIT;
  CHECK(!dz_client_mcp_call(&C, 1000, BODY, &r) && r.net == DZ_NET_CONNECT && r.stage == DZ_MCP_STAGE_INIT && C.mcp_sid[0] == 0);
  /* 401 on initialize and on a call; the session is wiped */
  setup(); sm = S_401;
  CHECK(!dz_client_mcp_call(&C, 1000, BODY, &r) && r.http == 401 && r.stage == DZ_MCP_STAGE_INIT && C.mcp_sid[0] == 0);
  setup();
  CHECK(dz_client_mcp_call(&C, 1000, BODY, &r));
  sm = S_401;
  CHECK(!dz_client_mcp_call(&C, 2000, BODY, &r) && r.http == 401 && r.stage == DZ_MCP_STAGE_CALL && C.mcp_sid[0] == 0 && F.init_calls == 1);
  /* HTTP 503 (server started with -nomcp) */
  setup(); sm = S_503;
  CHECK(!dz_client_mcp_call(&C, 1000, BODY, &r) && r.http == 503 && r.stage == DZ_MCP_STAGE_INIT);
  /* non-JSON 200 on the call */
  setup();
  CHECK(dz_client_mcp_call(&C, 1000, BODY, &r));
  sm = S_HTML;
  CHECK(!dz_client_mcp_call(&C, 2000, BODY, &r) && r.http == 200 && r.parse == DZ_ERR_SYNTAX);
  /* initialize answered, but no session header: no session, reported as not-a-session */
  setup(); sm = S_NOSID; F.sid_script = "";
  CHECK(!dz_client_mcp_call(&C, 1000, BODY, &r) && r.stage == DZ_MCP_STAGE_INIT && r.parse == DZ_ERR_STATUS && C.mcp_sid[0] == 0 && tools_run == 0);
  /* initialize answered with a JSON-RPC error */
  setup(); sm = S_ERR_INIT;
  CHECK(!dz_client_mcp_call(&C, 1000, BODY, &r) && r.stage == DZ_MCP_STAGE_INIT && r.reply.has_error && r.reply.code == -32603);
  /* a body that does not fit is refused without a request */
  setup();
  char big[400];
  memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0;
  CHECK(!dz_client_mcp_call(&C, 1000, big, &r) && F.calls == 0);
  no_leaks();
}

static void test_wipes(void) {
  dz_mcp_result_t r;
  setup();
  CHECK(dz_client_mcp_call(&C, 1000, BODY, &r) && C.mcp_sid[0]);
  dz_client_set_token(&C, "OTHER");
  CHECK(C.mcp_sid[0] == 0 && C.mcp_sid[10] == 0); /* wiped, not just terminated */
  CHECK(dz_client_mcp_call(&C, 2000, BODY, &r) && F.init_calls == 2);
  dz_client_retry_now(&C, 3000);
  CHECK(C.mcp_sid[0] == 0 && C.mcp_sid[10] == 0);
  CHECK(dz_client_mcp_call(&C, 4000, BODY, &r) && F.init_calls == 3);
  dz_client_note_auth_failure(&C);
  CHECK(C.mcp_sid[0] == 0);
  /* a forced open (connection test) starts a new session even when one exists */
  setup();
  CHECK(dz_client_mcp_open(&C, 1000, false, &r) && F.init_calls == 1);
  CHECK(dz_client_mcp_open(&C, 1100, false, &r) && F.init_calls == 1);
  CHECK(dz_client_mcp_open(&C, 1200, true, &r) && F.init_calls == 2);
  /* a transport without post (cannot do MCP) fails cleanly */
  F.base.post = NULL;
  dz_client_mcp_reset(&C);
  CHECK(!dz_client_mcp_open(&C, 1300, false, &r) && r.net == DZ_NET_CONNECT);
}

int main(void) {
  test_lazy_and_reuse();
  test_idle_retire();
  test_recovery();
  test_failures();
  test_wipes();
  if (M.mu) dz_model_deinit(&M);
  MT_DONE();
}
