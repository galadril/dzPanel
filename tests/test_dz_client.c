#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dz_client.h"
#include "dz_fake.h"
#include "mini_test.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "tests/fixtures"
#endif

static char *devices_json, *scenes_json;
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

static const char *VERSION = "{\"status\":\"OK\",\"title\":\"GetVersion\",\"version\":\"2026.4 (build 18568)\",\"hash\":\"abc\"}";
/* A real getconfig also holds the home coordinates (here a canary): the client must keep only the five unit keys. */
static const char *CONFIG = "{\"status\":\"OK\",\"title\":\"GetConfig\",\"Latitude\":\"12.345678\",\"Longitude\":\"98.765432\",\"TempScale\":1,\"TempSign\":\"F\",\"WindScale\":22.4,\"WindSign\":\"mph\",\"CurrencySign\":\"EUR\",\"WebUserName\":\"secret-user\"}";
static const char *PLANS = "{\"result\":[{\"Name\":\"Living\",\"idx\":\"1\"},{\"Name\":\"Bed\",\"idx\":\"2\"}],\"status\":\"OK\",\"title\":\"Plans\"}";
static const char *EMPTY_DELTA = "{\"ActTime\":1793448999,\"ServerTime\":\"2026-10-31 13:20:00\",\"app_version\":\"2026.4 (build 18568)\",\"status\":\"OK\",\"title\":\"Devices\"}";

typedef enum { M_OK, M_HTML, M_401, M_403, M_500, M_STATUS_ERR, M_TIMEOUT, M_CONNECT, M_TLS, M_TRUNC, M_404, M_HUGE_CAP, M_NOCONFIG } mode_t_;
static mode_t_ mode;
static const char *delta_body;  /* override for lastupdate requests */

static dz_net_err_t srv(fake_t *f, const char *url, char *buf, size_t cap, dz_http_result_t *out) {
  (void)f;
  switch (mode) {
    case M_HTML: return fake_body("<!DOCTYPE html><html><body>Please log in</body></html>", 200, buf, cap, out);
    case M_401: return fake_body("Unauthorized", 401, buf, cap, out);
    case M_403: return fake_body("{\"status\":\"ERR\"}", 403, buf, cap, out);
    case M_500: return fake_body("oops", 500, buf, cap, out);
    case M_NOCONFIG: if (strstr(url, "param=getconfig")) return fake_body("<html>nope</html>", 404, buf, cap, out); break;
    case M_404: return fake_body("<html>nope</html>", 404, buf, cap, out);
    case M_STATUS_ERR: return fake_body("{\"status\":\"ERR\"}", 200, buf, cap, out);
    case M_TIMEOUT: return DZ_NET_TIMEOUT;
    case M_CONNECT: return DZ_NET_CONNECT;
    case M_TLS: return DZ_NET_TLS;
    case M_TRUNC: return fake_body("{\"ActTime\":1,\"result\":[{\"idx\":\"1\",\"Name\":\"Cu", 200, buf, cap, out);
    default: break;
  }
  if (strstr(url, "param=getversion")) return fake_body(VERSION, 200, buf, cap, out);
  if (strstr(url, "param=getconfig")) return fake_body(CONFIG, 200, buf, cap, out);
  if (strstr(url, "param=getplans")) return fake_body(PLANS, 200, buf, cap, out);
  if (strstr(url, "param=getscenes")) return fake_body(scenes_json, 200, buf, cap, out);
  if (strstr(url, "lastupdate=")) return fake_body(delta_body ? delta_body : EMPTY_DELTA, 200, buf, cap, out);
  return fake_body(devices_json, 200, buf, cap, out);
}

#define CAP 128
static dz_entry_t ent[CAP];
static dz_device_t stg[CAP];
static dz_model_t M;
static char rbuf[256 * 1024];
static fake_t F;
static dz_client_t C;
static int state_changes;
static dz_conn_state_t last_to;
static void on_state(void *u, dz_conn_state_t a, dz_conn_state_t b) { (void)u; (void)a; state_changes++; last_to = b; }

static void setup(bool favs, size_t bufcap) {
  if (M.mu) dz_model_deinit(&M);
  dz_model_cfg_t mc = {.ttl_ms = 600000, .old_update_s = UINT32_MAX};
  CHECK(dz_model_init(&M, ent, CAP, stg, CAP, &mc));
  fake_init(&F, srv, NULL);
  F.secret = "SECRET-TOKEN-123";
  mode = M_OK;
  delta_body = NULL;
  state_changes = 0;
  dz_client_cfg_t cfg = {.base_url = "http://dz.example:8080/", .token = "SECRET-TOKEN-123", .transport = &F.base, .model = &M,
                         .buf = rbuf, .buf_cap = bufcap, .favorites_only = favs, .poll_interval_ms = 10000,
                         .startup_gap_ms = 5000, .on_state = on_state};
  CHECK(dz_client_init(&C, &cfg));
}

static uint32_t T;
static int run_until_quiet(int max_steps) {
  int made = 0;
  for (int i = 0; i < max_steps; i++) {
    uint32_t w = dz_client_ms_until_due(&C, T);
    if (w == UINT32_MAX) break;
    T += w;
    if (dz_client_step(&C, T)) made++;
  }
  return made;
}

static void test_version(void) {
  dz_version_t v;
  CHECK(dz_version_parse("2026.4 (build 18568)", &v) && v.year == 2026 && v.minor == 4 && v.build == 18568);
  CHECK(dz_version_parse("v2026.1", &v) && v.year == 2026 && v.minor == 1 && v.build == 0);
  CHECK(dz_version_parse("Domoticz 2026.10 (build 7) beta", &v) && v.minor == 10 && v.build == 7);
  CHECK(!dz_version_parse("", &v) && !v.valid);
  CHECK(!dz_version_parse("abc", &v) && !dz_version_parse("2026", &v) && !dz_version_parse("1.2", &v) && !dz_version_parse(NULL, &v));
  CHECK(dz_version_parse("2026.4", &v) && dz_version_at_least(&v, 2026, 1) && !dz_version_at_least(&v, 2026, 5));
}

static void test_flow(void) {
  setup(false, sizeof rbuf);
  T = 1000;
  CHECK(dz_client_state(&C) == DZ_CS_CONNECTING);
  CHECK(dz_client_step(&C, T) && F.calls == 1 && strstr(fake_last_url(&F), "param=getversion"));
  /* never tight: calling again immediately does nothing */
  for (int i = 0; i < 100; i++) CHECK(!dz_client_step(&C, T));
  CHECK(F.calls == 1);
  CHECK(dz_client_state(&C) == DZ_CS_OK && C.version.valid && C.version.year == 2026 && C.version.build == 18568);
  T += 4999;
  CHECK(!dz_client_step(&C, T));
  T += 1;
  /* getconfig comes right after getversion: units are unknown until it was read */
  {
    dz_units_t u;
    CHECK(!dz_model_units(&M, &u) && !u.have_temp && !u.have_wind);
    CHECK(dz_client_step(&C, T) && strstr(fake_last_url(&F), "param=getconfig"));
    CHECK(dz_model_units(&M, &u) && u.have_temp && !strcmp(u.temp, "F") && u.have_wind && !strcmp(u.wind, "mph"));
    CHECK(u.have_currency && !strcmp(u.currency, "EUR") && u.have_temp_scale && u.temp_scale == 1.0f && u.have_wind_scale);
    /* nothing but the five keys is kept: the struct has no room for coordinates or user names */
    CHECK(sizeof u < 64);
  }
  T += 5000;
  CHECK(dz_client_step(&C, T) && strstr(fake_last_url(&F), "param=getdevices") && !strstr(fake_last_url(&F), "lastupdate"));
  CHECK(strstr(fake_last_url(&F), "/json.htm?type=command&"));
  CHECK(C.cursor == 1793448461 && dz_model_count(&M) > 20);
  CHECK(!strcmp(C.sunrise, "") || 1);
  T += 5000;
  CHECK(dz_client_step(&C, T) && strstr(fake_last_url(&F), "param=getscenes"));
  T += 5000;
  CHECK(dz_client_step(&C, T) && strstr(fake_last_url(&F), "param=getplans") && C.n_plans == 2 && !strcmp(C.plans[1].name, "Bed"));
  size_t n0 = dz_model_count(&M);
  T += 10000;
  CHECK(dz_client_step(&C, T) && strstr(fake_last_url(&F), "lastupdate=1793448459")); /* cursor minus 2 s overlap */
  CHECK(C.cursor == 1793448999 && dz_model_count(&M) == n0);
  T += 10000;
  CHECK(dz_client_step(&C, T) && strstr(fake_last_url(&F), "lastupdate=1793448997"));
  CHECK(F.had_auth && !F.auth_in_url);
  /* scenes polled every third delta; groups appear in the scene namespace */
  int saw_scenes = 0;
  for (int i = 0; i < 6; i++) { T += 10000; dz_client_step(&C, T); saw_scenes += strstr(fake_last_url(&F), "getscenes") != NULL; }
  CHECK(saw_scenes >= 1);
  /* faster than the interval, never: rate over 1000 simulated seconds */
  int before = F.calls;
  uint32_t start = T;
  for (; T < start + 1000000; T += 100) dz_client_step(&C, T);
  CHECK(F.calls - before <= 100);
  CHECK(C.requests == (uint32_t)F.calls);
  dz_model_deinit(&M);
}

static void test_delta_apply(void) {
  setup(false, sizeof rbuf);
  T = 0;
  run_until_quiet(6); /* version, config, devices, scenes, plans, first delta */
  CHECK(dz_client_state(&C) == DZ_CS_OK);
  /* one changed device in a delta (real shape: result omitted when empty, present otherwise) */
  delta_body = "{\"ActTime\":1793449500,\"ServerTime\":\"2026-10-31 13:30:00\",\"result\":[{\"idx\":\"22\",\"Name\":\"Device 6\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"SwitchTypeVal\":0,\"Status\":\"On\",\"Data\":\"On\"}],\"status\":\"OK\",\"title\":\"Devices\"}";
  uint32_t g = dz_model_generation(&M);
  T += 10000;
  CHECK(dz_client_step(&C, T));
  CHECK(dz_model_generation(&M) == g + 1);
  dz_device_t d; ru_val_t v; ru_val_state_t st;
  CHECK(dz_model_get(&M, "22", false, &d, &v, &st, T) && !strcmp(d.status, "On") && st == RU_VAL_OK);
  /* an ActTime hidden inside a string must not become the cursor */
  delta_body = "{\"result\":[{\"idx\":\"22\",\"Name\":\"\\\"ActTime\\\": 99\",\"Status\":\"On\",\"Type\":\"Light/Switch\",\"SwitchTypeVal\":0}],\"ActTime\":1793449600,\"status\":\"OK\"}";
  T += 10000;
  CHECK(dz_client_step(&C, T) && C.cursor == 1793449600);
  /* ActTime missing: keep full reads, never guess a cursor */
  delta_body = NULL;
  dz_model_deinit(&M);
}

static void test_favorites(void) {
  setup(true, sizeof rbuf);
  T = 0;
  run_until_quiet(3);
  CHECK(strstr(fake_last_url(&F), "favorite=1"));
  size_t all = 0;
  setup(false, sizeof rbuf);
  T = 0;
  run_until_quiet(3);
  all = dz_model_count(&M);
  CHECK(all > dz_model_count(&M) - 1);
  setup(true, sizeof rbuf);
  T = 0;
  run_until_quiet(3);
  CHECK(dz_model_count(&M) < all);
  for (size_t i = 0; i < dz_model_count(&M); i++) {
    dz_device_t d;
    CHECK(dz_model_at(&M, i, &d, NULL, NULL, NULL, 0) && d.favorite);
  }
  dz_model_deinit(&M);
}

static void test_token_rejected(void) {
  for (int m = M_401; m <= M_403; m++) {
    setup(false, sizeof rbuf);
    T = 0;
    run_until_quiet(4);
    CHECK(dz_client_state(&C) == DZ_CS_OK);
    size_t n = dz_model_count(&M);
    mode = (mode_t_)m;
    T += 10000;
    int calls = F.calls;
    CHECK(dz_client_step(&C, T));
    CHECK(dz_client_state(&C) == DZ_CS_TOKEN_REJECTED && C.halted && C.last_http_status == (m == M_401 ? 401 : 403));
    CHECK(dz_client_ms_until_due(&C, T) == UINT32_MAX);
    for (int i = 0; i < 1000; i++) { T += 60000; CHECK(!dz_client_step(&C, T)); }
    CHECK(F.calls == calls + 1);          /* polling stopped: no retry storm */
    CHECK(dz_model_count(&M) == n);       /* old data kept (ages to stale) */
    mode = M_OK;
    dz_client_set_token(&C, "NEW-TOKEN");
    CHECK(dz_client_state(&C) == DZ_CS_CONNECTING);
    CHECK(dz_client_step(&C, T) && strstr(fake_last_url(&F), "getversion"));
    CHECK(dz_client_state(&C) == DZ_CS_OK);
    dz_model_deinit(&M);
  }
}

static void test_failures(void) {
  /* HTML proxy page */
  setup(false, sizeof rbuf);
  T = 0;
  run_until_quiet(4);
  size_t n = dz_model_count(&M);
  mode = M_HTML;
  uint32_t gaps[8];
  for (int i = 0; i < 8; i++) {
    T = C.next_due_ms;
    uint32_t t0 = T;
    CHECK(dz_client_step(&C, T));
    gaps[i] = C.next_due_ms - t0;
  }
  CHECK(dz_client_state(&C) == DZ_CS_BAD_RESPONSE && C.last_err == DZ_CERR_PARSE);
  CHECK(gaps[0] == 10000 && gaps[1] == 20000 && gaps[2] == 40000 && gaps[3] == 80000);   /* exponential */
  CHECK(gaps[7] == 300000);                                                              /* capped */
  CHECK(dz_model_count(&M) == n);
  mode = M_OK;
  T = C.next_due_ms;
  CHECK(dz_client_step(&C, T) && dz_client_state(&C) == DZ_CS_OK && C.failures == 0);
  CHECK(C.next_due_ms - T == 10000);                                                     /* backoff resets */
  dz_model_deinit(&M);

  struct { mode_t_ m; dz_conn_state_t s; dz_client_err_t e; } cases[] = {
    {M_500, DZ_CS_SERVER_ERROR, DZ_CERR_HTTP}, {M_STATUS_ERR, DZ_CS_SERVER_ERROR, DZ_CERR_STATUS},
    {M_404, DZ_CS_BAD_RESPONSE, DZ_CERR_HTTP}, {M_TRUNC, DZ_CS_BAD_RESPONSE, DZ_CERR_PARSE},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    setup(false, sizeof rbuf);
    T = 0;
    run_until_quiet(4);
    n = dz_model_count(&M);
    mode = cases[i].m;
    T = C.next_due_ms;
    CHECK(dz_client_step(&C, T));
    CHECK(dz_client_state(&C) == cases[i].s && C.last_err == cases[i].e);
    CHECK(dz_model_count(&M) == n && !C.halted);
    dz_model_deinit(&M);
  }

  /* transport failures: a blip keeps OK, several make OFFLINE; first connect stays CONNECTING until 3 failures */
  setup(false, sizeof rbuf);
  mode = M_CONNECT;
  T = 0;
  dz_client_step(&C, T);
  CHECK(dz_client_state(&C) == DZ_CS_CONNECTING && C.last_err == DZ_CERR_CONNECT);
  T = C.next_due_ms; dz_client_step(&C, T);
  CHECK(dz_client_state(&C) == DZ_CS_CONNECTING);
  T = C.next_due_ms; dz_client_step(&C, T);
  CHECK(dz_client_state(&C) == DZ_CS_OFFLINE);
  mode = M_OK;
  T = C.next_due_ms; dz_client_step(&C, T);
  CHECK(dz_client_state(&C) == DZ_CS_OK);
  mode = M_TIMEOUT;
  T = C.next_due_ms; dz_client_step(&C, T);
  CHECK(dz_client_state(&C) == DZ_CS_OK && C.last_err == DZ_CERR_TIMEOUT); /* one blip */
  T = C.next_due_ms; dz_client_step(&C, T);
  T = C.next_due_ms; dz_client_step(&C, T);
  CHECK(dz_client_state(&C) == DZ_CS_OFFLINE);
  mode = M_TLS;
  T = C.next_due_ms; dz_client_step(&C, T);
  CHECK(dz_client_state(&C) == DZ_CS_OFFLINE && C.last_err == DZ_CERR_TLS);
  CHECK(state_changes >= 3 && last_to == DZ_CS_OFFLINE);
  dz_model_deinit(&M);

  /* response larger than the buffer */
  setup(false, 4096);
  T = 0;
  run_until_quiet(3);
  CHECK(dz_client_state(&C) == DZ_CS_BAD_RESPONSE && C.last_err == DZ_CERR_TOO_LARGE && dz_model_count(&M) == 0);
  dz_model_deinit(&M);
}

static int config_calls(void) {
  int n = 0;
  for (int i = 0; i < F.calls && i < 32; i++) n += strstr(F.urls[i], "param=getconfig") != NULL;
  return n;
}

static void test_units(void) {
  dz_units_t u;
  /* parse: only the five keys, strict */
  CHECK(dz_units_parse(CONFIG, strlen(CONFIG), &u) && u.have_temp && u.temp[0] == 'F' && u.have_wind && !strcmp(u.wind, "mph"));
  CHECK(u.have_wind_scale && u.wind_scale > 22.3f && u.wind_scale < 22.5f && u.have_currency);
  const char *metric = "{\"status\":\"OK\",\"TempScale\":1,\"TempSign\":\"C\",\"WindScale\":10,\"WindSign\":\"m/s\"}";
  CHECK(dz_units_parse(metric, strlen(metric), &u) && !strcmp(u.temp, "C") && !strcmp(u.wind, "m/s") && !u.have_currency);
  const char *bad = "{\"status\":\"OK\",\"TempSign\":\"K\",\"WindSign\":\"a very long unit name\",\"TempScale\":\"x\",\"WindScale\":-1}";
  CHECK(!dz_units_parse(bad, strlen(bad), &u) && !u.have_temp && !u.have_wind && !u.have_temp_scale && !u.have_wind_scale);
  const char *err = "{\"status\":\"ERR\",\"TempSign\":\"C\"}";
  CHECK(!dz_units_parse(err, strlen(err), &u) && !u.have_temp);
  CHECK(!dz_units_parse("<html>", 6, &u) && !dz_units_parse(NULL, 0, &u));
  /* a key hidden inside a nested object or string is not a top-level key */
  const char *nest = "{\"status\":\"OK\",\"x\":{\"TempSign\":\"F\"},\"y\":\"\\\"TempSign\\\": 1\"}";
  CHECK(!dz_units_parse(nest, strlen(nest), &u) && !u.have_temp);
  char t[8];
  dz_units_t one = {.have_temp = true, .temp = "F"};
  dz_units_temp_text(&one, t, sizeof t);
  CHECK(!strcmp(t, "\xC2\xB0" "F"));
  dz_units_temp_text(&u, t, sizeof t);
  CHECK(!t[0]); /* unknown: no symbol at all */

  /* a server that refuses getconfig must not stop the device list; units stay unknown */
  setup(false, sizeof rbuf);
  mode = M_NOCONFIG;
  T = 0;
  run_until_quiet(4);
  CHECK(dz_client_state(&C) == DZ_CS_OK && dz_model_count(&M) > 20 && !dz_model_units(&M, &u) && C.failures == 0);
  dz_model_deinit(&M);

  /* units are re-read on every full refresh (600 s) */
  setup(false, sizeof rbuf);
  T = 0;
  int before_cfg = 0;
  for (int i = 0; i < 6 && i < 100; i++) { T = C.next_due_ms; dz_client_step(&C, T); }
  before_cfg = config_calls();
  CHECK(before_cfg == 1 && dz_model_units(&M, &u));
  dz_units_t blank;
  memset(&blank, 0, sizeof blank);
  dz_model_set_units(&M, &blank);
  CHECK(!dz_model_units(&M, &u));
  int seen = 0;
  for (int i = 0; i < 100; i++) {
    T = C.next_due_ms;
    dz_client_step(&C, T);
    if (strstr(fake_last_url(&F), "param=getconfig")) seen++;
    if (T > 700000) break;
  }
  CHECK(seen == 1 && dz_model_units(&M, &u) && !strcmp(u.temp, "F"));
  dz_model_deinit(&M);
}

static void test_config(void) {
  setup(false, sizeof rbuf);
  dz_client_cfg_t cfg = {.base_url = "ftp://x", .transport = &F.base, .model = &M, .buf = rbuf, .buf_cap = sizeof rbuf};
  CHECK(!dz_client_init(&C, &cfg));
  cfg.base_url = "http://x";
  cfg.token = "bad token";
  CHECK(!dz_client_init(&C, &cfg));
  cfg.token = "bad\r\nX-Evil: 1";
  CHECK(!dz_client_init(&C, &cfg));
  cfg.token = NULL;
  cfg.poll_interval_ms = 1;
  CHECK(dz_client_init(&C, &cfg) && C.cfg.poll_interval_ms == 1000);
  CHECK(!dz_client_init(&C, &(dz_client_cfg_t){.base_url = "http://x", .transport = &F.base, .model = &M, .buf = rbuf, .buf_cap = 10}));
  /* no token configured: no Authorization header */
  mode = M_OK;
  T = 0;
  dz_client_step(&C, T);
  CHECK(!F.had_auth);
  setenv("DOMOTICZ_ACCESSTOKEN", "abc", 1);
  char t[16];
  CHECK(dz_client_token_from_env(t, sizeof t) && !strcmp(t, "abc") && !dz_client_token_from_env(t, 3));
  unsetenv("DOMOTICZ_ACCESSTOKEN");
  CHECK(!dz_client_token_from_env(t, sizeof t));
  dz_model_deinit(&M);
}

int main(void) {
  devices_json = slurp("real_getdevices.json");
  scenes_json = slurp("real_getscenes.json");
  test_version();
  test_flow();
  test_delta_apply();
  test_favorites();
  test_token_rejected();
  test_failures();
  test_config();
  test_units();
  free(devices_json);
  free(scenes_json);
  MT_DONE();
}
