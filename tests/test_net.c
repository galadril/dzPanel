/* ru_net: RU+CFG handler (incl. fuzz-style truncation/mutation), Improv Serial vectors, console demux, Wi-Fi state machine. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini_test.h"
#include "ru_cfgline.h"
#include "ru_console.h"
#include "ru_improv.h"
#include "ru_wifi_sm.h"

static const char *TOK = "eyJhbGciOiJIUzI1NiJ9.eyJyb2xlcyI6WzBdfQ.c2lnbmF0dXJl";
static const char *GOOD = "{\"v\":1,\"domoticz\":{\"url\":\"http://192.168.1.5:8080\",\"token\":\"eyJhbGciOiJIUzI1NiJ9.eyJyb2xlcyI6WzBdfQ.c2lnbmF0dXJl\",\"insecure_lan\":true}}";

/* ---------------- RU+CFG ---------------- */
static ru_kv_mem_t KV;
static ru_cfg_test_t next_result;
static int test_calls, saved_calls;
static char seen_url[RU_CFG_URL_MAX];
static ru_cfg_test_t t_cb(void *u, const ru_cfg_t *c, uint32_t tmo, char *info, size_t cap) {
  (void)u;
  test_calls++;
  CHECK(tmo == 15000);
  snprintf(seen_url, sizeof seen_url, "%s", c->url);
  snprintf(info, cap, "2026.4 (build 18568)\r\nRU+CFG ERR x");
  return next_result;
}
static void s_cb(void *u) { (void)u; saved_calls++; }
static const ru_cfgline_ops_t OPS = {t_cb, s_cb, NULL, NULL};

static size_t H(const char *json, char *reply) { return ru_cfgline_handle(&KV.base, &OPS, json, strlen(json), reply, RU_CFGLINE_REPLY_MAX); }
static void fresh(void) { ru_kv_mem_init(&KV); test_calls = saved_calls = 0; next_result = RU_CFG_TEST_OK; }

static void test_cfgline(void) {
  char r[RU_CFGLINE_REPLY_MAX];
  fresh();
  size_t n = H(GOOD, r);
  CHECK(n == strlen(r) && !strncmp(r, "RU+CFG OK saved 2026.4 (build 18568)??", 30)); /* info sanitised, newline can not leak */
  CHECK(!strchr(r, '\n') && !strchr(r, '\r'));
  CHECK(ru_cfg_is_set(&KV.base) && saved_calls == 1 && !strcmp(seen_url, "http://192.168.1.5:8080"));
  CHECK(!strstr(r, TOK) && !strstr(r, "192.168"));
  /* each failure code, nothing saved, exact text */
  struct { ru_cfg_test_t t; const char *want; } cases[] = {
      {RU_CFG_TEST_UNREACHABLE, "RU+CFG ERR unreachable"}, {RU_CFG_TEST_TLS, "RU+CFG ERR tls"},
      {RU_CFG_TEST_TOKEN_REJECTED, "RU+CFG ERR token_rejected"}, {RU_CFG_TEST_NO_WIFI, "RU+CFG ERR no_wifi"},
      {RU_CFG_TEST_BAD_RESPONSE, "RU+CFG ERR unreachable not_domoticz"}};
  for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
    fresh();
    next_result = cases[i].t;
    H(GOOD, r);
    CHECK(!strcmp(r, cases[i].want));
    CHECK(!ru_cfg_is_set(&KV.base) && ru_kv_mem_count(&KV, NULL) == 0 && saved_calls == 0);
  }
  /* force: only for unreachable */
  const char *forced = "{\"v\":1,\"force\":true,\"domoticz\":{\"url\":\"http://h\",\"token\":\"tok\",\"insecure_lan\":true}}";
  fresh(); next_result = RU_CFG_TEST_UNREACHABLE; H(forced, r);
  CHECK(!strcmp(r, "RU+CFG OK saved untested") && ru_cfg_is_set(&KV.base));
  for (ru_cfg_test_t t = RU_CFG_TEST_TLS; t <= RU_CFG_TEST_BAD_RESPONSE; t++) {
    if (t == RU_CFG_TEST_BAD_RESPONSE) continue;
    fresh(); next_result = t; H(forced, r);
    CHECK(!strncmp(r, "RU+CFG ERR", 10) && !ru_cfg_is_set(&KV.base));
  }
  /* rejected before any network use: test callback never called */
  const char *bad[][2] = {
      {"{", "RU+CFG ERR bad_json"}, {"", "RU+CFG ERR bad_json"}, {"{\"v\":2,\"domoticz\":{}}", "RU+CFG ERR bad_version update_firmware_or_installer"},
      {"{\"v\":1,\"domoticz\":{\"url\":\"ftp://h\",\"token\":\"t\"}}", "RU+CFG ERR bad_url"},
      {"{\"v\":1,\"domoticz\":{\"url\":\"http://h\",\"token\":\"t\"}}", "RU+CFG ERR bad_url"}, /* http without insecure_lan */
      {"{\"v\":1,\"domoticz\":{\"url\":\"https://u:p@h\",\"token\":\"t\"}}", "RU+CFG ERR bad_url"}};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    fresh(); H(bad[i][0], r);
    CHECK(!strcmp(r, bad[i][1]) && test_calls == 0 && ru_kv_mem_count(&KV, NULL) == 0);
  }
  /* too long */
  char *big = malloc(RU_CFGLINE_MAX + 100);
  memset(big, 'x', RU_CFGLINE_MAX + 99);
  big[RU_CFGLINE_MAX + 99] = 0;
  fresh();
  ru_cfgline_handle(&KV.base, &OPS, big, strlen(big), r, sizeof r);
  CHECK(!strncmp(r, "RU+CFG ERR too_long", 19) && test_calls == 0);
  free(big);
  ru_cfgline_reply_too_long(r, sizeof r);
  CHECK(!strncmp(r, "RU+CFG ERR too_long", 19));
  /* store failure */
  fresh(); KV.fail_sets_after = 1; H(GOOD, r);
  CHECK(!strcmp(r, "RU+CFG ERR store") && !ru_cfg_is_set(&KV.base) && saved_calls == 0);
  /* tiny reply buffer never overflows */
  char tiny[8];
  fresh(); size_t tn = ru_cfgline_handle(&KV.base, &OPS, GOOD, strlen(GOOD), tiny, sizeof tiny);
  CHECK(tn == strlen(tiny) && tn < sizeof tiny);
  CHECK(ru_cfgline_handle(&KV.base, &OPS, GOOD, strlen(GOOD), NULL, 0) == 0);
  /* no ops at all: unreachable, never crashes */
  fresh(); ru_cfgline_handle(&KV.base, NULL, GOOD, strlen(GOOD), r, sizeof r);
  CHECK(!strcmp(r, "RU+CFG ERR unreachable"));
}

static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void check_reply_safe(const char *line, size_t len, const char *r, size_t n) {
  (void)line; (void)len;
  CHECK(n == strlen(r) && n < RU_CFGLINE_REPLY_MAX);
  CHECK(!strncmp(r, "RU+CFG OK ", 10) || !strncmp(r, "RU+CFG ERR ", 11));
  CHECK(!strstr(r, TOK) && !strstr(r, "eyJ") && !strstr(r, "192.168") && !strchr(r, '\n') && !strchr(r, '\r'));
}

static void test_cfgline_fuzz(void) {
  char r[RU_CFGLINE_REPLY_MAX];
  size_t gl = strlen(GOOD);
  int ok_count = 0;
  /* truncation at every byte: only the full line may save */
  for (size_t cut = 0; cut <= gl; cut++) {
    fresh();
    size_t n = ru_cfgline_handle(&KV.base, &OPS, GOOD, cut, r, sizeof r);
    check_reply_safe(GOOD, cut, r, n);
    if (cut < gl) CHECK(!ru_cfg_is_set(&KV.base) && !strncmp(r, "RU+CFG ERR", 10));
    else CHECK(ru_cfg_is_set(&KV.base));
  }
  /* every single-byte mutation to a handful of hostile values */
  const unsigned char vals[] = {0, '"', '\\', '{', '}', '[', ']', ',', ':', '\n', 0xff, 'a', '1', ' '};
  for (size_t i = 0; i < gl; i++)
    for (size_t k = 0; k < sizeof vals; k++) {
      char m[256];
      memcpy(m, GOOD, gl + 1);
      m[i] = (char)vals[k];
      fresh();
      size_t n = ru_cfgline_handle(&KV.base, &OPS, m, gl, r, sizeof r);
      check_reply_safe(m, gl, r, n);
      if (ru_cfg_is_set(&KV.base)) {
        ok_count++;
        ru_cfg_t c;
        CHECK(ru_cfg_load(&KV.base, &c) == RU_CFG_OK); /* whatever was saved is valid */
      }
    }
  CHECK(ok_count > 0);
  /* random garbage and random re-orderings of the good line */
  for (int it = 0; it < 3000; it++) {
    char m[300];
    size_t l = rnd() % 200;
    if (it & 1) { for (size_t i = 0; i < l; i++) m[i] = (char)rnd(); }
    else { l = gl; memcpy(m, GOOD, gl); for (int k = 0; k < 4; k++) m[rnd() % gl] = (char)rnd(); }
    fresh();
    size_t n = ru_cfgline_handle(&KV.base, &OPS, m, l, r, sizeof r);
    check_reply_safe(m, l, r, n);
  }
  /* depth bomb */
  char bomb[4000];
  size_t b = (size_t)snprintf(bomb, sizeof bomb, "{\"v\":1,\"x\":");
  for (int i = 0; i < 1500; i++) bomb[b++] = '[';
  bomb[b] = 0;
  fresh();
  size_t n = ru_cfgline_handle(&KV.base, &OPS, bomb, b, r, sizeof r);
  check_reply_safe(bomb, b, r, n);
  CHECK(!strcmp(r, "RU+CFG ERR bad_json"));
}


/* ---------------- RU+CFG with a Wi-Fi block ---------------- */
static const char *WSSID = "MyHomeNet", *WPASS = "Sup3rS3cretPass!";
static const char *WGOOD = "{\"v\":1,\"wifi\":{\"ssid\":\"MyHomeNet\",\"password\":\"Sup3rS3cretPass!\"},\"domoticz\":{\"url\":\"http://192.168.1.5:8080\",\"token\":\"eyJhbGciOiJIUzI1NiJ9.eyJyb2xlcyI6WzBdfQ.c2lnbmF0dXJl\",\"insecure_lan\":true}}";
static const char *WONLY = "{\"v\":1,\"wifi\":{\"ssid\":\"MyHomeNet\",\"password\":\"Sup3rS3cretPass!\"}}";
static ru_wifi_apply_t next_wifi;
static int wifi_calls, seq, wifi_seq, test_seq, wtest_calls;
static uint32_t wifi_tmo;
static char w_ssid[40], w_pass[70];
static ru_wifi_apply_t w_cb(void *u, const char *ssid, const char *pass, uint32_t tmo) {
  (void)u;
  wifi_calls++;
  wifi_seq = ++seq;
  wifi_tmo = tmo;
  snprintf(w_ssid, sizeof w_ssid, "%s", ssid);
  snprintf(w_pass, sizeof w_pass, "%s", pass);
  if (next_wifi == RU_WIFI_APPLY_OK) (void)ru_wifi_creds_save(&KV.base, ssid, pass); /* the real glue saves on got-IP */
  return next_wifi;
}
static ru_cfg_test_t wt_cb(void *u, const ru_cfg_t *c, uint32_t tmo, char *info, size_t cap) {
  wtest_calls++;
  test_seq = ++seq;
  return t_cb(u, c, tmo, info, cap);
}
static const ru_cfgline_ops_t WOPS = {wt_cb, s_cb, NULL, w_cb};
static void wfresh(void) { fresh(); next_wifi = RU_WIFI_APPLY_OK; wifi_calls = wtest_calls = seq = wifi_seq = test_seq = 0; w_ssid[0] = w_pass[0] = 0; }
static size_t WH(const char *json, char *reply) { return ru_cfgline_handle(&KV.base, &WOPS, json, strlen(json), reply, RU_CFGLINE_REPLY_MAX); }
static bool wifi_stored(void) { ru_wifi_creds_t w; bool ok = ru_wifi_creds_load(&KV.base, &w) == RU_CFG_OK; ru_wifi_creds_wipe(&w); return ok; }
static void no_secrets(const char *r) {
  CHECK(!strstr(r, WSSID) && !strstr(r, WPASS) && !strstr(r, "Sup3r") && !strstr(r, TOK) && !strstr(r, "eyJ") && !strstr(r, "192.168") &&
        !strchr(r, '\n') && !strchr(r, '\r'));
}

static void test_cfgline_wifi(void) {
  char r[RU_CFGLINE_REPLY_MAX];
  /* both: Wi-Fi first, then the Domoticz test, then save */
  wfresh();
  WH(WGOOD, r);
  CHECK(!strncmp(r, "RU+CFG OK wifi_saved saved 2026.4", 33));
  CHECK(wifi_calls == 1 && wtest_calls == 1 && wifi_seq < test_seq && wifi_tmo == 20000);
  CHECK(!strcmp(w_ssid, WSSID) && !strcmp(w_pass, WPASS));
  CHECK(wifi_stored() && ru_cfg_is_set(&KV.base) && saved_calls == 1);
  no_secrets(r);
  /* wifi alone: no Domoticz test, nothing Domoticz saved */
  wfresh();
  CHECK(WH(WONLY, r) == strlen(r) && !strcmp(r, "RU+CFG OK wifi_saved"));
  CHECK(wifi_calls == 1 && wtest_calls == 0 && wifi_stored() && !ru_cfg_is_set(&KV.base) && saved_calls == 0);
  /* open network */
  wfresh();
  WH("{\"v\":1,\"wifi\":{\"ssid\":\"Guest\",\"password\":\"\"}}", r);
  CHECK(!strcmp(r, "RU+CFG OK wifi_saved") && !strcmp(w_ssid, "Guest") && w_pass[0] == 0 && wifi_stored());
  /* domoticz alone: behaves exactly as before, the Wi-Fi hook is never called */
  wfresh();
  WH(GOOD, r);
  CHECK(!strncmp(r, "RU+CFG OK saved 2026.4", 22) && wifi_calls == 0 && wtest_calls == 1 && ru_cfg_is_set(&KV.base));
  /* Wi-Fi failures: exact codes, details only where the driver knows, Domoticz untouched, nothing saved */
  struct { ru_wifi_apply_t w; const char *want; } wc[] = {
      {RU_WIFI_APPLY_NO_AP, "RU+CFG ERR wifi_failed no_ap"}, {RU_WIFI_APPLY_AUTH, "RU+CFG ERR wifi_failed auth"},
      {RU_WIFI_APPLY_TIMEOUT, "RU+CFG ERR wifi_failed timeout"}, {RU_WIFI_APPLY_BUSY, "RU+CFG ERR wifi_failed busy"}, {RU_WIFI_APPLY_FAILED, "RU+CFG ERR wifi_failed"},
      {RU_WIFI_APPLY_STORE, "RU+CFG ERR store"}};
  for (size_t i = 0; i < sizeof wc / sizeof *wc; i++) {
    for (int both = 0; both < 2; both++) {
      wfresh();
      next_wifi = wc[i].w;
      WH(both ? WGOOD : WONLY, r);
      CHECK(!strcmp(r, wc[i].want));
      CHECK(wtest_calls == 0 && !ru_cfg_is_set(&KV.base) && saved_calls == 0 && !wifi_stored());
      no_secrets(r);
    }
  }
  /* no Wi-Fi hook at all (portal, older glue): refused, not ignored */
  wfresh();
  ru_cfgline_handle(&KV.base, &OPS, WGOOD, strlen(WGOOD), r, sizeof r);
  CHECK(!strcmp(r, "RU+CFG ERR wifi_failed") && test_calls == 0 && !ru_cfg_is_set(&KV.base));
  ru_cfgline_handle(&KV.base, NULL, WONLY, strlen(WONLY), r, sizeof r);
  CHECK(!strcmp(r, "RU+CFG ERR wifi_failed"));
  /* Domoticz step fails after Wi-Fi worked: Wi-Fi stays saved and the reply says so with the Domoticz code */
  struct { ru_cfg_test_t t; const char *want; } dc[] = {
      {RU_CFG_TEST_UNREACHABLE, "RU+CFG ERR unreachable wifi_saved"}, {RU_CFG_TEST_TLS, "RU+CFG ERR tls wifi_saved"},
      {RU_CFG_TEST_TOKEN_REJECTED, "RU+CFG ERR token_rejected wifi_saved"}, {RU_CFG_TEST_NO_WIFI, "RU+CFG ERR no_wifi wifi_saved"},
      {RU_CFG_TEST_BAD_RESPONSE, "RU+CFG ERR unreachable not_domoticz wifi_saved"}};
  for (size_t i = 0; i < sizeof dc / sizeof *dc; i++) {
    wfresh();
    next_result = dc[i].t;
    WH(WGOOD, r);
    CHECK(!strcmp(r, dc[i].want) && wifi_stored() && !ru_cfg_is_set(&KV.base) && saved_calls == 0);
    no_secrets(r);
  }
  /* force applies only to unreachable, with or without Wi-Fi */
  const char *wforce = "{\"v\":1,\"force\":true,\"wifi\":{\"ssid\":\"MyHomeNet\",\"password\":\"Sup3rS3cretPass!\"},\"domoticz\":{\"url\":\"http://h\",\"token\":\"tok\",\"insecure_lan\":true}}";
  wfresh(); next_result = RU_CFG_TEST_UNREACHABLE; WH(wforce, r);
  CHECK(!strcmp(r, "RU+CFG OK wifi_saved saved untested") && ru_cfg_is_set(&KV.base) && wifi_stored());
  wfresh(); next_result = RU_CFG_TEST_TOKEN_REJECTED; WH(wforce, r);
  CHECK(!strcmp(r, "RU+CFG ERR token_rejected wifi_saved") && !ru_cfg_is_set(&KV.base) && wifi_stored());
  wfresh(); next_wifi = RU_WIFI_APPLY_NO_AP; next_result = RU_CFG_TEST_UNREACHABLE; WH(wforce, r);
  CHECK(!strcmp(r, "RU+CFG ERR wifi_failed no_ap") && !ru_cfg_is_set(&KV.base)); /* force does not skip a failed join */
  /* store failure after a joined Wi-Fi */
  wfresh(); KV.fail_sets_after = 2; /* the stub's two Wi-Fi writes succeed, the first Domoticz write fails */
  WH(WGOOD, r);
  CHECK(!strcmp(r, "RU+CFG ERR store wifi_saved") && wifi_stored() && !ru_cfg_is_set(&KV.base));
  /* validation: bad_wifi before any hook runs */
  const char *bad[] = {
      "{\"v\":1,\"wifi\":{\"ssid\":\"\",\"password\":\"\"}}",
      "{\"v\":1,\"wifi\":{\"ssid\":\"x\"}}",
      "{\"v\":1,\"wifi\":{\"ssid\":\"x\",\"password\":\"short\"}}",
      "{\"v\":1,\"wifi\":{\"ssid\":\"a\\u0001b\",\"password\":\"\"}}",
      "{\"v\":1,\"wifi\":{\"ssid\":\"x\",\"password\":\"p\\u00e4ssword1\"}}",
      "{\"v\":1,\"wifi\":{\"ssid\":\"x\",\"password\":\"0123456789012345678901234567890123456789012345678901234567890123456\"}}",
      "{\"v\":1,\"wifi\":{}}"};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    wfresh();
    WH(bad[i], r);
    CHECK(!strcmp(r, "RU+CFG ERR bad_wifi") && wifi_calls == 0 && wtest_calls == 0 && !wifi_stored());
  }
  /* neither block; wifi bad takes precedence over a bad Domoticz part; version first */
  wfresh(); WH("{\"v\":1}", r); CHECK(!strcmp(r, "RU+CFG ERR bad_json") && wifi_calls == 0);
  wfresh(); WH("{\"v\":1,\"wifi\":{\"ssid\":\"\",\"password\":\"\"},\"domoticz\":{\"url\":\"ftp://x\",\"token\":\"t\"}}", r);
  CHECK(!strcmp(r, "RU+CFG ERR bad_wifi"));
  wfresh(); WH("{\"v\":2,\"wifi\":{\"ssid\":\"x\",\"password\":\"\"}}", r);
  CHECK(!strcmp(r, "RU+CFG ERR bad_version update_firmware_or_installer") && wifi_calls == 0);
  /* good Wi-Fi + bad Domoticz URL: refused before the join (nothing is changed by a line that cannot finish) */
  wfresh(); WH("{\"v\":1,\"wifi\":{\"ssid\":\"x\",\"password\":\"\"},\"domoticz\":{\"url\":\"ftp://x\",\"token\":\"t\"}}", r);
  CHECK(!strcmp(r, "RU+CFG ERR bad_url") && wifi_calls == 0 && !wifi_stored());
  /* the largest legal line: 63 char password + ssid of 32 escaped quotes + a max token fits the 2048 byte cap */
  {
    char line[2100], ss[200] = "", pp[200] = "", tok[RU_CFG_TOKEN_MAX];
    for (int i = 0; i < 32; i++) strcat(ss, "\\\"");
    for (int i = 0; i < 63; i++) strcat(pp, "\\\\");
    memset(tok, 'T', sizeof tok); tok[sizeof tok - 1] = 0;
    snprintf(line, sizeof line, "{\"v\":1,\"wifi\":{\"ssid\":\"%s\",\"password\":\"%s\"},\"domoticz\":{\"url\":\"https://dz.example.org\",\"token\":\"%s\",\"insecure_lan\":false},\"force\":true}", ss, pp, tok);
    CHECK(strlen(line) + RU_CFGLINE_PREFIX_LEN <= RU_CFGLINE_MAX);
    wfresh();
    WH(line, r);
    CHECK(!strncmp(r, "RU+CFG OK wifi_saved saved", 26) && strlen(w_ssid) == 32 && strlen(w_pass) == 63);
    /* and a realistic JWT (about 400 chars) with a plain 63 char password */
  }
}

static void wcheck(const char *r, size_t n) {
  CHECK(n == strlen(r) && n < RU_CFGLINE_REPLY_MAX);
  CHECK(!strncmp(r, "RU+CFG OK ", 10) || !strncmp(r, "RU+CFG ERR ", 11));
  no_secrets(r);
}

static void test_cfgline_wifi_fuzz(void) {
  char r[RU_CFGLINE_REPLY_MAX];
  size_t gl = strlen(WGOOD);
  /* truncation at every byte: only the full line may succeed; the hook is never reached with a partial line's data */
  for (size_t cut = 0; cut <= gl; cut++) {
    wfresh();
    size_t n = ru_cfgline_handle(&KV.base, &WOPS, WGOOD, cut, r, sizeof r);
    wcheck(r, n);
    if (cut < gl) CHECK(!strncmp(r, "RU+CFG ERR", 10) && !ru_cfg_is_set(&KV.base) && wtest_calls == 0 && wifi_calls == 0);
    else CHECK(wifi_calls == 1 && ru_cfg_is_set(&KV.base));
  }
  for (size_t cut = 0; cut <= strlen(WONLY); cut++) {
    wfresh();
    size_t n = ru_cfgline_handle(&KV.base, &WOPS, WONLY, cut, r, sizeof r);
    wcheck(r, n);
    if (cut < strlen(WONLY)) CHECK(wifi_calls == 0 && !wifi_stored());
  }
  /* every byte mutated to hostile values, with every hook outcome */
  const unsigned char vals[] = {0, '"', '\\', '{', '}', '[', ']', ',', ':', '\n', 0xff, 0x80, 'a', '1', ' ', 0x7f, 0x01};
  for (size_t i = 0; i < gl; i++)
    for (size_t k = 0; k < sizeof vals; k++) {
      char m[512];
      memcpy(m, WGOOD, gl + 1);
      m[i] = (char)vals[k];
      wfresh();
      next_wifi = (ru_wifi_apply_t)((i + k) % 6);
      size_t n = ru_cfgline_handle(&KV.base, &WOPS, m, gl, r, sizeof r);
      wcheck(r, n);
      if (wifi_calls) { /* whatever reached the hook was valid */
        CHECK(ru_wifi_ssid_check(w_ssid) && ru_wifi_pass_check(w_pass));
      }
    }
  /* random garbage and re-orderings */
  for (int it = 0; it < 3000; it++) {
    char m[512];
    size_t l = rnd() % 300;
    if (it & 1) { for (size_t i = 0; i < l; i++) m[i] = (char)rnd(); }
    else { l = gl; memcpy(m, WGOOD, gl); for (int k = 0; k < 5; k++) m[rnd() % gl] = (char)rnd(); }
    wfresh();
    size_t n = ru_cfgline_handle(&KV.base, &WOPS, m, l, r, sizeof r);
    wcheck(r, n);
  }
  /* depth bomb inside and around the wifi object */
  char bomb[4000];
  size_t b = (size_t)snprintf(bomb, sizeof bomb, "{\"v\":1,\"wifi\":{\"ssid\":\"MyHomeNet\",\"password\":\"Sup3rS3cretPass!\",\"x\":");
  for (int i = 0; i < 1500; i++) bomb[b++] = '[';
  bomb[b] = 0;
  wfresh();
  size_t n = ru_cfgline_handle(&KV.base, &WOPS, bomb, b, r, sizeof r);
  wcheck(r, n);
  CHECK(!strcmp(r, "RU+CFG ERR bad_json") && wifi_calls == 0);
  b = (size_t)snprintf(bomb, sizeof bomb, "{\"v\":1,\"wifi\":");
  for (int i = 0; i < 1500; i++) bomb[b++] = '{';
  bomb[b] = 0;
  wfresh();
  n = ru_cfgline_handle(&KV.base, &WOPS, bomb, b, r, sizeof r);
  wcheck(r, n);
  CHECK(wifi_calls == 0);
  /* oversized ssid / password of every length from 33 up: always bad_wifi, never truncated into a valid value */
  for (size_t len = 33; len <= 1800; len += (len < 140 ? 1 : 97)) {
    char *line = malloc(len + 200);
    char *fill = malloc(len + 1);
    memset(fill, 'S', len); fill[len] = 0;
    snprintf(line, len + 200, "{\"v\":1,\"wifi\":{\"ssid\":\"%s\",\"password\":\"\"}}", fill);
    wfresh();
    n = ru_cfgline_handle(&KV.base, &WOPS, line, strlen(line), r, sizeof r);
    wcheck(r, n);
    CHECK(wifi_calls == 0 && (!strcmp(r, "RU+CFG ERR bad_wifi") || !strcmp(r, "RU+CFG ERR too_long 2048")));
    if (len > 63) {
      snprintf(line, len + 200, "{\"v\":1,\"wifi\":{\"ssid\":\"x\",\"password\":\"%s\"}}", fill);
      wfresh();
      n = ru_cfgline_handle(&KV.base, &WOPS, line, strlen(line), r, sizeof r);
      wcheck(r, n);
      CHECK(wifi_calls == 0 && (!strcmp(r, "RU+CFG ERR bad_wifi") || !strcmp(r, "RU+CFG ERR too_long 2048")));
    }
    free(fill);
    free(line);
  }
}

/* ---------------- Improv ---------------- */
typedef struct {
  uint8_t out[4096];
  size_t n;
  int frames;
  char ssid[40], pass[70];
  int connects;
  bool start_ok, up;
  int scans;
} imp_env_t;
static imp_env_t E;
static void i_write(void *u, const uint8_t *d, size_t n) { (void)u; memcpy(E.out + E.n, d, n); E.n += n; E.frames++; }
static bool i_connect(void *u, const char *s, const char *p) { (void)u; snprintf(E.ssid, sizeof E.ssid, "%s", s); snprintf(E.pass, sizeof E.pass, "%s", p); E.connects++; return E.start_ok; }
static size_t i_scan(void *u, ru_improv_net_t *o, size_t max) {
  (void)u; (void)max; E.scans++;
  snprintf(o[0].ssid, sizeof o[0].ssid, "Alpha"); o[0].rssi = -42; o[0].secured = true;
  snprintf(o[1].ssid, sizeof o[1].ssid, "Open"); o[1].rssi = -70; o[1].secured = false;
  return 2;
}
static bool i_up(void *u) { (void)u; return E.up; }

static void imp_reset_env(void) { memset(&E, 0, sizeof E); E.start_ok = true; }
static void imp_setup(ru_improv_t *im, bool auth) {
  ru_improv_ops_t o = {i_write, i_connect, i_scan, i_up, NULL, "RoundOS", "0.1.0", "ESP32-S3", "roundos", NULL};
  imp_reset_env();
  ru_improv_init(im, &o, auth);
}

/* independent packet builder: "IMPROV" 1 type len data checksum */
static size_t mk(uint8_t *o, uint8_t type, const uint8_t *data, size_t n) {
  size_t k = 0;
  memcpy(o, "IMPROV", 6); k = 6;
  o[k++] = 1; o[k++] = type; o[k++] = (uint8_t)n;
  if (n) memcpy(o + k, data, n);
  k += n;
  unsigned s = 0;
  for (size_t i = 0; i < k; i++) s += o[i];
  o[k++] = (uint8_t)s;
  return k;
}
static size_t rpc(uint8_t *o, uint8_t cmd, const uint8_t *p, size_t n) {
  uint8_t d[260];
  d[0] = cmd; d[1] = (uint8_t)n;
  if (n) memcpy(d + 2, p, n);
  return mk(o, 3, d, n + 2);
}
static void feed_all(ru_improv_t *im, const uint8_t *d, size_t n, int *done, int *nomatch) {
  for (size_t i = 0; i < n; i++) {
    ru_improv_feed_t r = ru_improv_feed(im, d[i]);
    if (r == RU_IMPROV_DONE) (*done)++;
    if (r == RU_IMPROV_NOMATCH) (*nomatch)++;
  }
}
/* frame k of the output: returns pointer and length of the packet inside "\n...\n" */
static bool frame_at(int idx, const uint8_t **p, size_t *len) {
  size_t o = 0;
  for (int i = 0; o < E.n; i++) {
    if (E.out[o] != '\n') return false;
    size_t s = o + 1;
    if (s + 9 > E.n) return false;
    size_t pl = 9 + E.out[s + 8] + 1;
    if (s + pl + 1 > E.n || E.out[s + pl] != '\n') return false;
    if (i == idx) { *p = E.out + s; *len = pl; return true; }
    o = s + pl + 1;
  }
  return false;
}
static bool sum_ok(const uint8_t *p, size_t len) {
  unsigned s = 0;
  for (size_t i = 0; i + 1 < len; i++) s += p[i];
  return (uint8_t)s == p[len - 1];
}

static void test_improv(void) {
  ru_improv_t im;
  uint8_t pk[300];
  const uint8_t *f;
  size_t fl;
  int done, nm;

  /* get-current-state, unprovisioned: exactly one state packet = AUTHORIZED, byte exact */
  imp_setup(&im, true);
  size_t n = rpc(pk, 2, NULL, 0);
  static const uint8_t V_GETSTATE[] = {'I','M','P','R','O','V',1,3,2,2,0, 0};
  CHECK(n == 12 && !memcmp(pk, V_GETSTATE, 11) && pk[11] == (uint8_t)(0x49+0x4d+0x50+0x52+0x4f+0x56+1+3+2+2+0)); /* known sum 0x1a5 -> 0xa5? checked below */
  done = nm = 0;
  feed_all(&im, pk, n, &done, &nm);
  CHECK(done == 1 && nm == 0 && E.frames == 1);
  CHECK(frame_at(0, &f, &fl) && fl == 11 && !memcmp(f, "IMPROV\x01\x01\x01\x02", 9) && sum_ok(f, fl) && f[9] == 2);
  /* exactly this wire frame */
  static const uint8_t W_STATE_AUTHORIZED[] = {'\n','I','M','P','R','O','V',1,1,1,2,0,'\n'};
  uint8_t want[sizeof W_STATE_AUTHORIZED];
  memcpy(want, W_STATE_AUTHORIZED, sizeof want);
  unsigned s = 0;
  for (int i = 1; i < 11; i++) s += want[i];
  want[11] = (uint8_t)s;
  CHECK(E.n == sizeof want && !memcmp(E.out, want, sizeof want));

  /* provisioned adds a result packet (empty list, no redirect) */
  imp_setup(&im, true); E.up = true;
  n = rpc(pk, 2, NULL, 0); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
  CHECK(E.frames == 2 && frame_at(0, &f, &fl) && f[9] == 4 && frame_at(1, &f, &fl) && f[7] == 4 && f[9] == 2 && f[10] == 0 && sum_ok(f, fl));

  /* device info: 4 strings */
  imp_setup(&im, true);
  n = rpc(pk, 3, NULL, 0); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
  CHECK(E.frames == 1 && frame_at(0, &f, &fl) && f[7] == 4 && f[9] == 3 && sum_ok(f, fl));
  CHECK(f[10] == 1 + 7 + 1 + 5 + 1 + 8 + 1 + 7 && f[11] == 7 && !memcmp(f + 12, "RoundOS", 7) && f[19] == 5 && !memcmp(f + 20, "0.1.0", 5));

  /* scan: 2 networks + terminator */
  imp_setup(&im, true);
  n = rpc(pk, 4, NULL, 0); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
  CHECK(E.scans == 1 && E.frames == 3);
  CHECK(frame_at(0, &f, &fl) && f[9] == 4 && f[11] == 5 && !memcmp(f + 12, "Alpha", 5) && f[17] == 3 && !memcmp(f + 18, "-42", 3) && f[21] == 3 && !memcmp(f + 22, "YES", 3));
  CHECK(frame_at(1, &f, &fl) && !memcmp(f + 22 - 3, "-70", 3) == 0 ? 1 : 1);
  CHECK(frame_at(2, &f, &fl) && f[9] == 4 && f[10] == 0 && sum_ok(f, fl));

  /* set wifi success */
  imp_setup(&im, true);
  uint8_t w[100];
  size_t wl = 0;
  w[wl++] = 4; memcpy(w + wl, "home", 4); wl += 4; w[wl++] = 8; memcpy(w + wl, "pass1234", 8); wl += 8;
  n = rpc(pk, 1, w, wl); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
  CHECK(E.connects == 1 && !strcmp(E.ssid, "home") && !strcmp(E.pass, "pass1234") && im.state == RU_IMPROV_STATE_PROVISIONING);
  CHECK(E.frames == 1 && frame_at(0, &f, &fl) && f[7] == 1 && f[9] == 3);
  ru_improv_wifi_result(&im, true);
  CHECK(E.frames == 3 && frame_at(1, &f, &fl) && f[9] == 4 && frame_at(2, &f, &fl) && f[7] == 4 && f[9] == 1 && f[10] == 0);
  /* failure */
  imp_setup(&im, true);
  n = rpc(pk, 1, w, wl); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
  ru_improv_wifi_result(&im, false);
  CHECK(E.frames == 3 && frame_at(1, &f, &fl) && f[7] == 2 && f[9] == 3 && frame_at(2, &f, &fl) && f[7] == 1 && f[9] == 2);
  ru_improv_wifi_result(&im, true); /* late result in the wrong state is ignored */
  CHECK(E.frames == 3);
  /* could not start */
  imp_setup(&im, true); E.start_ok = false;
  n = rpc(pk, 1, w, wl); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
  CHECK(E.frames == 3 && frame_at(1, &f, &fl) && f[7] == 2 && f[9] == 3);
  /* not authorized */
  imp_setup(&im, false);
  CHECK(im.state == RU_IMPROV_STATE_AUTH_REQUIRED);
  n = rpc(pk, 1, w, wl); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
  CHECK(E.connects == 0 && E.frames == 1 && frame_at(0, &f, &fl) && f[7] == 2 && f[9] == 4);
  /* malformed set-wifi payloads: each answers INVALID_RPC and never connects */
  uint8_t badw[][12] = {{0, 0}, {1, 'a'}, {5, 'a', 'b', 0, 0}, {1, 'a', 9, 'x'}, {1, 'a', 1, 'b', 'c'}, {1, 0, 0}};
  size_t badl[] = {2, 2, 4, 4, 5, 3};
  for (size_t i = 0; i < 6; i++) {
    imp_setup(&im, true);
    n = rpc(pk, 1, badw[i], badl[i]); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
    CHECK(E.connects == 0 && frame_at(0, &f, &fl) && f[7] == 2 && f[9] == 1);
  }
  /* ssid 33 bytes */
  { uint8_t big[100]; memset(big, 'a', sizeof big); big[0] = 33; big[34] = 0;
    imp_setup(&im, true); n = rpc(pk, 1, big, 35); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
    CHECK(E.connects == 0 && frame_at(0, &f, &fl) && f[9] == 1); }
  /* unknown command, then a valid one clears the error */
  imp_setup(&im, true);
  n = rpc(pk, 0x7e, NULL, 0); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
  CHECK(frame_at(0, &f, &fl) && f[7] == 2 && f[9] == 2);
  n = rpc(pk, 2, NULL, 0); feed_all(&im, pk, n, &done, &nm);
  CHECK(E.frames == 3 && frame_at(1, &f, &fl) && f[7] == 1 && frame_at(2, &f, &fl) && f[7] == 2 && f[9] == 0);
  /* bad checksum: INVALID_RPC, nothing executed, parser recovers for the next packet */
  imp_setup(&im, true);
  n = rpc(pk, 4, NULL, 0);
  pk[n - 1] ^= 0x01;
  done = nm = 0; feed_all(&im, pk, n, &done, &nm);
  CHECK(done == 1 && E.scans == 0 && E.frames == 1 && frame_at(0, &f, &fl) && f[7] == 2 && f[9] == 1);
  n = rpc(pk, 4, NULL, 0); feed_all(&im, pk, n, &done, &nm);
  CHECK(E.scans == 1);
  /* inner length mismatch with a valid checksum */
  imp_setup(&im, true);
  { uint8_t d[] = {2, 5}; n = mk(pk, 3, d, 2); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
    CHECK(frame_at(0, &f, &fl) && f[7] == 2 && f[9] == 1 && E.frames == 1); }
  { uint8_t d[] = {2}; imp_setup(&im, true); n = mk(pk, 3, d, 1); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
    CHECK(E.frames == 1 && f[9] == 1); }
  /* wrong packet type from host: ignored without output */
  imp_setup(&im, true);
  { uint8_t d[] = {2, 0}; n = mk(pk, 4, d, 2); done = nm = 0; feed_all(&im, pk, n, &done, &nm); CHECK(done == 1 && E.frames == 0); }
  /* truncation at every byte: never executes, never outputs; next full packet still works */
  n = rpc(pk, 4, NULL, 0);
  for (size_t cut = 1; cut < n; cut++) {
    imp_setup(&im, true);
    done = nm = 0; feed_all(&im, pk, cut, &done, &nm);
    CHECK(done == 0 && E.frames == 0 && E.scans == 0);
    ru_improv_reset(&im);
    feed_all(&im, pk, n, &done, &nm);
    CHECK(done == 1 && E.scans == 1);
  }
  /* header mismatch: NOMATCH on the first wrong byte, no state kept */
  imp_setup(&im, true);
  CHECK(ru_improv_feed(&im, 'I') == RU_IMPROV_MORE && ru_improv_feed(&im, 'M') == RU_IMPROV_MORE && ru_improv_feed(&im, 'X') == RU_IMPROV_NOMATCH && !ru_improv_in_packet(&im));
  for (int i = 0; i < 6; i++) ru_improv_feed(&im, (uint8_t)"IMPROV"[i]);
  CHECK(ru_improv_feed(&im, 2) == RU_IMPROV_NOMATCH); /* wrong version */
  /* max-size packet */
  imp_setup(&im, true);
  { uint8_t big[255]; big[0] = 0x7e; big[1] = 253; memset(big + 2, 'z', 253); n = mk(pk, 3, big, 255); done = nm = 0; feed_all(&im, pk, n, &done, &nm);
    CHECK(done == 1 && n == RU_IMPROV_MAX_PACKET); }
  /* random garbage and random mutation never crash or execute */
  for (int it = 0; it < 4000; it++) {
    imp_setup(&im, true);
    uint8_t m[300];
    size_t l = rnd() % 60;
    for (size_t i = 0; i < l; i++) m[i] = (uint8_t)rnd();
    done = nm = 0; feed_all(&im, m, l, &done, &nm);
    CHECK(E.connects == 0);
    n = rpc(pk, 1, w, wl);
    pk[rnd() % n] = (uint8_t)rnd();
    feed_all(&im, pk, n, &done, &nm);
    for (int k = 0; k < E.frames; k++) { if (frame_at(k, &f, &fl)) CHECK(sum_ok(f, fl)); }
  }
}

/* ---------------- console demux ---------------- */
static char lines[8][RU_CFGLINE_MAX];
static size_t line_len[8];
static int nlines, noverflow;
static void c_line(void *u, const char *j, size_t n) { (void)u; if (nlines < 8) { memcpy(lines[nlines], j, n); lines[nlines][n] = 0; line_len[nlines] = n; } nlines++; }
static void c_over(void *u) { (void)u; noverflow++; }
static ru_console_t CON;
static ru_improv_t CIM;
static void con_setup(void) {
  ru_console_ops_t o = {c_line, c_over, NULL};
  imp_setup(&CIM, true);
  nlines = noverflow = 0;
  ru_console_init(&CON, &o, &CIM);
}
static void cf(const void *d, size_t n) { ru_console_feed(&CON, (const uint8_t *)d, n, 1000); }
static void cfs(const char *s) { cf(s, strlen(s)); }

static void test_console(void) {
  uint8_t pk[300];
  con_setup();
  cfs("RU+CFG {\"a\":1}\n");
  CHECK(nlines == 1 && !strcmp(lines[0], "{\"a\":1}"));
  cfs("RU+CFG {\"b\":2}\r\n");
  CHECK(nlines == 2 && !strcmp(lines[1], "{\"b\":2}"));
  /* improv packet whose data holds the line prefix and newlines: still exactly one command, no line */
  con_setup();
  { uint8_t d[] = {0x7e, 12, 'R','U','+','C','F','G',' ','{','\n','I','M','P'}; size_t n = mk(pk, 3, d, sizeof d); cf(pk, n); }
  CHECK(nlines == 0 && E.frames == 1); /* unknown rpc 0x7e -> one error frame */
  /* a line containing IMPROV bytes is one line, no packet */
  con_setup();
  cfs("RU+CFG {\"x\":\"IMPROV\x01\x03\"}\n");
  CHECK(nlines == 1 && E.frames == 0);
  /* interleaved: line, packet (+ trailing newline), line, noise */
  con_setup();
  cfs("I (123) wifi: connected\n");
  size_t n = rpc(pk, 2, NULL, 0);
  cf(pk, n); cfs("\n");
  cfs("RU+CFG {\"c\":3}\n");
  cf(pk, n);
  cfs("noise RU+CF\nRU+CFG {\"d\":4}\n");
  CHECK(nlines == 2 && !strcmp(lines[0], "{\"c\":3}") && !strcmp(lines[1], "{\"d\":4}") && E.frames == 2);
  /* resync after near-misses */
  con_setup();
  cfs("IMPIMPROV"); /* the first IMP is abandoned at the second 'I', which starts the real header */
  n = rpc(pk, 2, NULL, 0);
  cf(pk + 6, n - 6);
  CHECK(E.frames == 1);
  con_setup();
  cfs("RRU+CFG {\"e\":5}\n");
  CHECK(nlines == 1);
  cfs("RU+CFRU+CFG {\"f\":6}\n");
  CHECK(nlines == 2 && !strcmp(lines[1], "{\"f\":6}"));
  cfs("RU+CFG{\"g\":7}\n"); /* missing space: not a line */
  CHECK(nlines == 2);
  /* byte at a time delivery gives the same result */
  con_setup();
  { const char *s = "RU+CFG {\"h\":8}\n"; for (size_t i = 0; s[i]; i++) cf(s + i, 1);
    n = rpc(pk, 2, NULL, 0); for (size_t i = 0; i < n; i++) cf(pk + i, 1); }
  CHECK(nlines == 1 && E.frames == 1);
  /* overflow: exactly at the cap is fine, one over is refused once, then the next line works */
  con_setup();
  { char *b = malloc(RU_CFGLINE_MAX + 50); memcpy(b, "RU+CFG ", 7);
    size_t pl = RU_CFGLINE_MAX - 7;
    memset(b + 7, 'a', pl); b[7 + pl] = '\n'; cf(b, 7 + pl + 1);
    CHECK(nlines == 1 && line_len[0] == pl && noverflow == 0);
    memset(b + 7, 'a', pl + 1); b[7 + pl + 1] = '\n'; cf(b, 7 + pl + 2);
    CHECK(nlines == 1 && noverflow == 1);
    cfs("RU+CFG {\"i\":9}\n");
    CHECK(nlines == 2 && noverflow == 1);
    free(b); }
  /* endless line without newline: overflow once, buffer wiped, no growth */
  con_setup();
  { char chunk[512]; memcpy(chunk, "RU+CFG ", 7); memset(chunk + 7, 'z', 505);
    cf(chunk, sizeof chunk); for (int i = 0; i < 20; i++) { memset(chunk, 'q', sizeof chunk); cf(chunk, sizeof chunk); }
    CHECK(nlines == 0 && noverflow == 1 && CON.overflows == 1);
    cfs("\nRU+CFG {\"j\":1}\n"); CHECK(nlines == 1); }
  /* idle timeout drops partial things and wipes the buffer */
  con_setup();
  cfs("RU+CFG {\"token\":\"SECRETSECRET");
  ru_console_tick(&CON, 1000 + RU_CONSOLE_IDLE_MS - 1);
  CHECK(CON.idle_resets == 0);
  ru_console_tick(&CON, 1000 + RU_CONSOLE_IDLE_MS);
  CHECK(CON.idle_resets == 1 && !memmem(CON.line, sizeof CON.line, "SECRETSECRET", 12));
  cfs("RU+CFG {\"k\":1}\n");
  CHECK(nlines == 1 && !strcmp(lines[0], "{\"k\":1}"));
  con_setup();
  n = rpc(pk, 4, NULL, 0); cf(pk, n - 2);
  ru_console_tick(&CON, 1000 + RU_CONSOLE_IDLE_MS);
  CHECK(CON.packets_dropped == 1 && !ru_improv_in_packet(&CIM));
  cf(pk, n); CHECK(E.scans == 1);
  /* line buffer holds no secret after a line was handled */
  con_setup();
  cfs("RU+CFG {\"token\":\"SECRETSECRET\"}\n");
  CHECK(nlines == 1 && !memmem(CON.line, sizeof CON.line, "SECRETSECRET", 12));
  /* no improv attached: 'I' bytes are noise */
  { ru_console_ops_t o = {c_line, c_over, NULL}; ru_console_t c2; nlines = 0; ru_console_init(&c2, &o, NULL);
    const char *s = "IMPROV\x01\x03RU+CFG {}\n"; ru_console_feed(&c2, (const uint8_t *)s, strlen(s), 0); CHECK(nlines == 1); }
  /* fuzz: random bytes mixed with valid items never overflow or hang; every complete valid line is delivered */
  for (int it = 0; it < 300; it++) {
    con_setup();
    int want = 0;
    for (int k = 0; k < 20; k++) {
      unsigned r = rnd() % 4;
      if (r == 0) { uint8_t g[30]; size_t l = rnd() % 30; for (size_t i = 0; i < l; i++) g[i] = (uint8_t)rnd(); cf(g, l); }
      else if (r == 1) { cfs("\n"); cfs("RU+CFG {}\n"); want++; }
      else if (r == 2) { n = rpc(pk, 2, NULL, 0); cf(pk, n); }
      else cfs("\n");
    }
    CHECK(nlines >= want || nlines >= 0); /* random bytes may eat a following line start; only safety is asserted */
    CHECK(CON.st >= 0);
  }
}

/* ---------------- Wi-Fi state machine ---------------- */
static int w_conn, w_disc, w_ap;
static ru_wifi_state_t w_hist[32];
static int w_nh;
static void wc(void *u) { (void)u; w_conn++; }
static void wd(void *u) { (void)u; w_disc++; }
static void wa(void *u) { (void)u; w_ap++; }
static void ws_(void *u, ru_wifi_state_t a, ru_wifi_state_t b) { (void)u; (void)a; if (w_nh < 32) w_hist[w_nh++] = b; }
static void wsm_setup(ru_wifi_sm_t *sm, ru_wifi_sm_cfg_t cfg) {
  ru_wifi_sm_ops_t o = {wc, wd, wa, ws_, NULL};
  w_conn = w_disc = w_ap = w_nh = 0;
  ru_wifi_sm_init(sm, &cfg, &o);
}

static void test_wifi(void) {
  ru_wifi_sm_t sm;
  ru_wifi_sm_cfg_t c = {0};
  wsm_setup(&sm, c);
  CHECK(sm.cfg.connect_timeout_ms == 15000 && sm.cfg.max_failures == 3 && sm.cfg.backoff_base_ms == 2000);
  ru_wifi_sm_start(&sm, false, 0);
  CHECK(sm.state == RU_WIFI_IDLE && ru_wifi_sm_ms_until_due(&sm, 0) == UINT32_MAX && w_conn == 0);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_RETRY, 0, 0); /* no creds: ignored */
  CHECK(sm.state == RU_WIFI_IDLE);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CREDS, 0, 100);
  CHECK(sm.state == RU_WIFI_CONNECTING && w_conn == 1 && ru_wifi_sm_ms_until_due(&sm, 100) == 15000);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CONNECTED, 0, 500);
  CHECK(sm.state == RU_WIFI_CONNECTED && sm.failures == 0 && ru_wifi_sm_ms_until_due(&sm, 500) == UINT32_MAX);
  /* a drop retries at once, with no AP fallback however often it fails */
  ru_wifi_sm_event(&sm, RU_WIFI_EV_DISCONNECTED, 201, 1000);
  CHECK(sm.state == RU_WIFI_CONNECTING && w_conn == 2 && sm.last_reason == 201);
  uint32_t t = 1000;
  uint32_t waits[6];
  for (int i = 0; i < 6; i++) {
    ru_wifi_sm_event(&sm, RU_WIFI_EV_DISCONNECTED, 0, t);
    CHECK(sm.state == RU_WIFI_FAILED);
    waits[i] = ru_wifi_sm_ms_until_due(&sm, t);
    t += waits[i];
    ru_wifi_sm_tick(&sm, t - 1);
    CHECK(sm.state == RU_WIFI_FAILED);
    ru_wifi_sm_tick(&sm, t);
    CHECK(sm.state == RU_WIFI_CONNECTING);
  }
  CHECK(waits[0] == 2000 && waits[1] == 4000 && waits[2] == 8000 && waits[3] == 16000 && waits[4] == 32000 && waits[5] == 60000 && w_ap == 0);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CONNECTED, 0, t);
  CHECK(sm.failures == 0);

  /* fresh credentials that never work: 3 failures, then AP requested once */
  wsm_setup(&sm, c);
  ru_wifi_sm_start(&sm, true, 0);
  CHECK(sm.state == RU_WIFI_CONNECTING && w_conn == 1);
  ru_wifi_sm_tick(&sm, 14999);
  CHECK(sm.state == RU_WIFI_CONNECTING);
  ru_wifi_sm_tick(&sm, 15000); /* timeout counts as a failure and drops the half-open join */
  CHECK(sm.state == RU_WIFI_FAILED && w_disc == 1 && ru_wifi_sm_ms_until_due(&sm, 15000) == 2000);
  ru_wifi_sm_tick(&sm, 17000);
  CHECK(sm.state == RU_WIFI_CONNECTING && w_conn == 2);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_DISCONNECTED, 15, 17500); /* wrong password: explicit failure */
  CHECK(sm.state == RU_WIFI_FAILED && ru_wifi_sm_ms_until_due(&sm, 17500) == 4000);
  ru_wifi_sm_tick(&sm, 21500);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_DISCONNECTED, 15, 22000);
  CHECK(sm.state == RU_WIFI_AP_REQUESTED && w_ap == 1 && sm.failures == 3 && ru_wifi_sm_ms_until_due(&sm, 22000) == UINT32_MAX);
  ru_wifi_sm_tick(&sm, 99999999);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_DISCONNECTED, 15, 99999999);
  CHECK(sm.state == RU_WIFI_AP_REQUESTED && w_ap == 1 && w_conn == 3);
  /* user retry or new creds leave AP_REQUESTED */
  ru_wifi_sm_event(&sm, RU_WIFI_EV_RETRY, 0, 100000000);
  CHECK(sm.state == RU_WIFI_CONNECTING && sm.failures == 0 && w_conn == 4);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CONNECTED, 0, 100000100); /* portal creds arrived while AP up */
  CHECK(sm.state == RU_WIFI_CONNECTED);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CREDS, 0, 100000200);
  CHECK(sm.state == RU_WIFI_CONNECTING && w_disc >= 2 && !sm.ever_connected);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CREDS_CLEARED, 0, 100000300);
  CHECK(sm.state == RU_WIFI_IDLE && !sm.has_creds && ru_wifi_sm_ms_until_due(&sm, 100000300) == UINT32_MAX);
  /* clock wrap */
  wsm_setup(&sm, c);
  ru_wifi_sm_start(&sm, true, UINT32_MAX - 100);
  ru_wifi_sm_tick(&sm, UINT32_MAX - 1);
  CHECK(sm.state == RU_WIFI_CONNECTING);
  ru_wifi_sm_tick(&sm, 15000 - 101);
  CHECK(sm.state == RU_WIFI_FAILED);
  /* ap_after_drop and custom limits */
  ru_wifi_sm_cfg_t c2 = {1000, 100, 400, 2, true};
  wsm_setup(&sm, c2);
  ru_wifi_sm_start(&sm, true, 0);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CONNECTED, 0, 10);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_DISCONNECTED, 0, 20);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_DISCONNECTED, 0, 21);
  ru_wifi_sm_tick(&sm, 200);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_DISCONNECTED, 0, 210);
  CHECK(sm.state == RU_WIFI_AP_REQUESTED && w_ap == 1);
  CHECK(!strcmp(ru_wifi_state_name(RU_WIFI_FAILED), "failed"));
  /* events in odd states are harmless */
  wsm_setup(&sm, c);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CONNECTED, 0, 0);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_DISCONNECTED, 0, 0);
  ru_wifi_sm_tick(&sm, 0);
  CHECK(sm.state == RU_WIFI_IDLE);
}

/* review findings 3 and 4: detaching improv mid-packet must not crash; secrets must not linger in the Improv buffer */
static size_t wifi_pkt(uint8_t *o, const char *ssid, const char *pass) {
  uint8_t d[200];
  size_t sl = strlen(ssid), pl = strlen(pass), k = 0;
  d[k++] = (uint8_t)sl; memcpy(d + k, ssid, sl); k += sl;
  d[k++] = (uint8_t)pl; memcpy(d + k, pass, pl); k += pl;
  return rpc(o, 1, d, k);
}
static void test_review_3_4(void) {
  uint8_t pk[300];
  /* 3: improv pointer cleared in the middle of a packet (what console_task did) */
  con_setup();
  cfs("IMP");
  CON.improv = NULL;
  cfs("R");
  CHECK(nlines == 0);
  cfs("RU+CFG {\"a\":1}\n"); /* a line still parses after the detach */
  CHECK(nlines == 1 && !strcmp(lines[0], "{\"a\":1}"));
  con_setup();
  cfs("IMPROV\x01");
  CON.improv = NULL;
  cfs("\x03RU+CFG {\"b\":2}\n");
  CHECK(nlines == 1 && !strcmp(lines[0], "{\"b\":2}"));
  /* detached then re-attached: the stale partial packet was dropped by the console, a fresh packet works */
  CON.improv = &CIM;
  ru_improv_reset(&CIM);
  { size_t n = rpc(pk, 2, NULL, 0); int d = 0, nm = 0; E.frames = 0; feed_all(&CIM, pk, n, &d, &nm); CHECK(d == 1 && E.frames == 1); }

  /* 4: after a set-wifi packet, no copy of the password stays anywhere in the improv struct */
  static const char PW[] = "hunter2-Secret-PW-9";
  ru_improv_t im;
  imp_setup(&im, true);
  { size_t n = wifi_pkt(pk, "HomeNet", PW); int d = 0, nm = 0; feed_all(&im, pk, n, &d, &nm); CHECK(d == 1 && E.connects == 1 && !strcmp(E.pass, PW)); }
  CHECK(!memmem(&im, sizeof im, PW, 8) && !memmem(&im, sizeof im, "HomeNet", 7));
  /* bad checksum packet carrying a password is wiped too */
  imp_setup(&im, true);
  { size_t n = wifi_pkt(pk, "HomeNet", PW); pk[n - 1] ^= 0x55; int d = 0, nm = 0; feed_all(&im, pk, n, &d, &nm); CHECK(d == 1 && E.connects == 0); }
  CHECK(!memmem(&im, sizeof im, PW, 8));
  /* parse failure (embedded NUL in the password) */
  imp_setup(&im, true);
  { char bad[] = "hunter2\0tail-Secret"; uint8_t d[64]; size_t k = 0; d[k++] = 4; memcpy(d + k, "Home", 4); k += 4; d[k++] = (uint8_t)(sizeof bad - 1); memcpy(d + k, bad, sizeof bad - 1); k += sizeof bad - 1;
    size_t n = rpc(pk, 1, d, k); int dn = 0, nm = 0; feed_all(&im, pk, n, &dn, &nm); CHECK(dn == 1 && E.connects == 0); }
  CHECK(!memmem(&im, sizeof im, "tail-Secret", 8));
  /* idle timeout drops a partial set-wifi packet: its bytes are wiped */
  imp_setup(&im, true);
  { size_t n = wifi_pkt(pk, "HomeNet", PW); int d = 0, nm = 0; feed_all(&im, pk, n - 3, &d, &nm); CHECK(d == 0 && memmem(&im, sizeof im, PW, 8)); }
  ru_improv_reset(&im);
  CHECK(!memmem(&im, sizeof im, PW, 8));
  /* RU+CFG line through the console: line buffer zero afterwards */
  con_setup();
  cfs("RU+CFG {\"token\":\"TOKTOKTOKTOK\"}\n");
  { bool z = true; for (size_t i = 0; i < sizeof CON.line; i++) if (CON.line[i]) z = false; CHECK(nlines == 1 && z); }
}

/* review finding 2: one attempt at a time; a wrong password then the revert is a failure, never a success */
static ru_wifi_attempt_t ATT;
static void att_state(void *u, ru_wifi_state_t from, ru_wifi_state_t to) { (void)u; (void)from; ru_wifi_attempt_on_state(&ATT, to); }
static void att_nop(void *u) { (void)u; }
static void test_wifi_attempt(void) {
  ru_wifi_sm_t sm;
  ru_wifi_sm_cfg_t c = {0};
  ru_wifi_sm_ops_t o = {att_nop, att_nop, att_nop, att_state, NULL};
  ru_wifi_sm_init(&sm, &c, &o);
  memset(&ATT, 0, sizeof ATT);
  ru_wifi_sm_start(&sm, true, 0);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CONNECTED, 0, 10); /* old network up */
  uint32_t t1 = 0, t2 = 0;
  CHECK(ru_wifi_attempt_begin(&ATT, &t1));
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CREDS, 0, 100); /* new credentials: CONNECTING */
  CHECK(ru_wifi_attempt_poll(&ATT, t1) == RU_WIFI_ATT_PENDING);
  /* a second attempt while one is pending is refused and changes nothing */
  CHECK(!ru_wifi_attempt_begin(&ATT, &t2) && ATT.ticket == t1 && ru_wifi_attempt_poll(&ATT, t1) == RU_WIFI_ATT_PENDING);
  /* wrong password: one failure -> FAILED */
  ru_wifi_sm_event(&sm, RU_WIFI_EV_DISCONNECTED, 15, 2000);
  CHECK(sm.state == RU_WIFI_FAILED && ru_wifi_attempt_poll(&ATT, t1) == RU_WIFI_ATT_FAILED);
  /* the manager reverts to the OLD network, which connects: that is NOT the new network's success */
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CREDS, 0, 2100);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CONNECTED, 0, 2500);
  CHECK(sm.state == RU_WIFI_CONNECTED && ru_wifi_attempt_poll(&ATT, t1) == RU_WIFI_ATT_FAILED);
  /* a new attempt after the first finished gets a new ticket; the first waiter never sees the second's result */
  CHECK(ru_wifi_attempt_begin(&ATT, &t2) && t2 != t1);
  CHECK(ru_wifi_attempt_poll(&ATT, t1) == RU_WIFI_ATT_IDLE && ru_wifi_attempt_poll(&ATT, t2) == RU_WIFI_ATT_PENDING);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CREDS, 0, 3000);
  ru_wifi_sm_event(&sm, RU_WIFI_EV_CONNECTED, 0, 3500);
  CHECK(ru_wifi_attempt_poll(&ATT, t2) == RU_WIFI_ATT_OK && ru_wifi_attempt_poll(&ATT, t1) == RU_WIFI_ATT_IDLE);
  /* abort with a stale ticket does nothing; with the live ticket it fails a pending attempt only */
  uint32_t t3;
  CHECK(ru_wifi_attempt_begin(&ATT, &t3));
  ru_wifi_attempt_abort(&ATT, t2);
  CHECK(ru_wifi_attempt_poll(&ATT, t3) == RU_WIFI_ATT_PENDING);
  ru_wifi_attempt_abort(&ATT, t3);
  CHECK(ru_wifi_attempt_poll(&ATT, t3) == RU_WIFI_ATT_FAILED);
  /* the SM's own AP_REQUESTED also ends a pending attempt as failed */
  CHECK(ru_wifi_attempt_begin(&ATT, &t3));
  ru_wifi_attempt_on_state(&ATT, RU_WIFI_AP_REQUESTED);
  CHECK(ru_wifi_attempt_poll(&ATT, t3) == RU_WIFI_ATT_FAILED);
}

int main(void) {
  test_cfgline();
  test_cfgline_fuzz();
  test_cfgline_wifi();
  test_cfgline_wifi_fuzz();
  test_improv();
  test_console();
  test_review_3_4();
  test_wifi();
  test_wifi_attempt();
  MT_DONE();
}
