/* ru_cfg: kv stores, URL/token/config validation (vectors mirror tests/site/test_lib.mjs), JSON parser limits,
 * storage atomicity and secret hygiene. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini_test.h"
#include "ru_cfg.h"

static const char *JWT = "eyJhbGciOiJIUzI1NiJ9.eyJyb2xlcyI6WzBdfQ.c2lnbmF0dXJl"; /* synthetic */

static void test_kv(void) {
  static ru_kv_mem_t m;
  ru_kv_mem_init(&m);
  ru_kv_t *kv = &m.base;
  char b[32];
  CHECK(ru_kv_get_str(kv, "ns", "k", b, sizeof b) == RU_KV_NOT_FOUND);
  CHECK(ru_kv_set_str(kv, "ns", "k", "hello") == RU_KV_OK);
  CHECK(ru_kv_get_str(kv, "ns", "k", b, sizeof b) == RU_KV_OK && !strcmp(b, "hello"));
  CHECK(ru_kv_get_str(kv, "ns", "k", b, 3) == RU_KV_TOO_SMALL && b[0] == 0);
  CHECK(ru_kv_get_str(kv, "other", "k", b, sizeof b) == RU_KV_NOT_FOUND); /* namespaces are separate */
  CHECK(ru_kv_set_str(kv, "other", "k", "x") == RU_KV_OK);
  CHECK(ru_kv_set_str(kv, "ns", "k", "again") == RU_KV_OK && ru_kv_mem_count(&m, "ns") == 1); /* overwrite */
  const unsigned char blob[] = {0, 1, 2, 255, 0};
  unsigned char ob[8];
  size_t len = 0;
  CHECK(ru_kv_set_blob(kv, "ns", "bl", blob, sizeof blob) == RU_KV_OK);
  CHECK(ru_kv_get_blob(kv, "ns", "bl", ob, sizeof ob, &len) == RU_KV_OK && len == 5 && !memcmp(ob, blob, 5));
  CHECK(ru_kv_get_blob(kv, "ns", "bl", ob, 2, &len) == RU_KV_TOO_SMALL && len == 5);
  CHECK(ru_kv_get_str(kv, "ns", "bl", b, sizeof b) == RU_KV_NOT_FOUND); /* type mismatch */
  CHECK(ru_kv_set_str(kv, "", "k", "x") == RU_KV_INVALID);
  CHECK(ru_kv_set_str(kv, "ns", "0123456789abcdef", "x") == RU_KV_INVALID); /* 16 chars: over the NVS limit */
  CHECK(ru_kv_set_str(kv, "n s", "k", "x") == RU_KV_INVALID);
  CHECK(ru_kv_erase_key(kv, "ns", "k") == RU_KV_OK && ru_kv_erase_key(kv, "ns", "k") == RU_KV_NOT_FOUND);
  CHECK(ru_kv_erase_ns(kv, "ns") == RU_KV_OK && ru_kv_mem_count(&m, "ns") == 0 && ru_kv_mem_count(&m, "other") == 1);
  char key[8];
  ru_kv_err_t last = RU_KV_OK;
  for (int i = 0; i < RU_KV_MEM_MAX_ENTRIES + 2; i++) { snprintf(key, sizeof key, "k%d", i); last = ru_kv_set_str(kv, "full", key, "v"); }
  CHECK(last == RU_KV_FULL);
}

static void test_url(void) {
  char o[RU_CFG_URL_MAX];
  bool https = true;
  CHECK(ru_cfg_normalise_url("192.168.1.5:8080", o, sizeof o, &https) == RU_CFG_OK && !strcmp(o, "http://192.168.1.5:8080") && !https);
  CHECK(ru_cfg_normalise_url(" https://dz.example.org/ ", o, sizeof o, &https) == RU_CFG_OK && !strcmp(o, "https://dz.example.org") && https);
  CHECK(ru_cfg_normalise_url("http://host/domoticz/", o, sizeof o, NULL) == RU_CFG_OK && !strcmp(o, "http://host/domoticz")); /* sub-path kept */
  CHECK(ru_cfg_normalise_url("HTTP://Host.Example:80", o, sizeof o, NULL) == RU_CFG_OK && !strcmp(o, "http://host.example")); /* default port dropped */
  CHECK(ru_cfg_normalise_url("https://h:443/x//", o, sizeof o, NULL) == RU_CFG_OK && !strcmp(o, "https://h/x"));
  CHECK(ru_cfg_normalise_url("https://h:8443", o, sizeof o, NULL) == RU_CFG_OK && !strcmp(o, "https://h:8443"));
  CHECK(ru_cfg_normalise_url("http://[fe80::1]:8080/", o, sizeof o, NULL) == RU_CFG_OK && !strcmp(o, "http://[fe80::1]:8080"));
  CHECK(ru_cfg_normalise_url("https://u:p@host", o, sizeof o, NULL) == RU_CFG_BAD_URL); /* user info refused */
  CHECK(ru_cfg_normalise_url("https://u@host", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://host/?x=1", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://host?x=1", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://host/#f", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://host#f", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("ftp://host", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("   ", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://:80", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://h:99999", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://h:0", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://h:8a", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://h/../x", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://h/a b", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://h\\x", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://h\r\nHost: x", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://h\xc3\xa9", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://h_x.local", o, sizeof o, NULL) == RU_CFG_OK);
  CHECK(ru_cfg_normalise_url("http://[zz]", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  CHECK(ru_cfg_normalise_url("http://[::1", o, sizeof o, NULL) == RU_CFG_BAD_URL);
  char longu[400];
  memset(longu, 'a', sizeof longu);
  memcpy(longu, "http://", 7);
  longu[300] = 0;
  CHECK(ru_cfg_normalise_url(longu, o, sizeof o, NULL) == RU_CFG_TOO_LONG);
  longu[399] = 0;
  longu[390] = 0;
  longu[300] = 'a';
  CHECK(ru_cfg_normalise_url(longu, o, sizeof o, NULL) == RU_CFG_TOO_LONG);
  CHECK(ru_cfg_normalise_url("http://h", o, 4, NULL) == RU_CFG_TOO_LONG);
  CHECK(ru_cfg_normalise_url(NULL, o, sizeof o, NULL) == RU_CFG_BAD_URL);
}

static void test_token(void) {
  bool jwt;
  CHECK(ru_cfg_check_token(JWT, &jwt) == RU_CFG_OK && jwt);
  CHECK(ru_cfg_check_token("not-a-jwt", &jwt) == RU_CFG_OK && !jwt); /* soft: the server decides */
  CHECK(ru_cfg_check_token("", NULL) == RU_CFG_BAD_JSON);
  CHECK(ru_cfg_check_token("has space.x.y", NULL) == RU_CFG_BAD_JSON);
  CHECK(ru_cfg_check_token("a\r\nX-Evil: 1", NULL) == RU_CFG_BAD_JSON);
  CHECK(ru_cfg_check_token("t\xc3\xa9", NULL) == RU_CFG_BAD_JSON);
  char big[RU_CFG_TOKEN_MAX + 1];
  memset(big, 'a', sizeof big);
  big[RU_CFG_TOKEN_MAX] = 0;
  CHECK(ru_cfg_check_token(big, NULL) == RU_CFG_TOO_LONG);
  big[RU_CFG_TOKEN_MAX - 1] = 0;
  CHECK(ru_cfg_check_token(big, NULL) == RU_CFG_OK);
}

static ru_cfg_err_t P(const char *j, ru_cfg_t *c, bool *f) { return ru_cfg_parse_json(j, strlen(j), c, f); }

static void test_parse(void) {
  ru_cfg_t c;
  bool force = true;
  char j[1400];
  snprintf(j, sizeof j, "{\"v\":1,\"domoticz\":{\"url\":\"http://192.168.1.5:8080/\",\"token\":\"%s\",\"insecure_lan\":true}}", JWT);
  CHECK(P(j, &c, &force) == RU_CFG_OK && !strcmp(c.url, "http://192.168.1.5:8080") && !strcmp(c.token, JWT) && c.insecure_lan && !force);
  /* http needs consent */
  snprintf(j, sizeof j, "{\"v\":1,\"domoticz\":{\"url\":\"http://h\",\"token\":\"%s\",\"insecure_lan\":false}}", JWT);
  CHECK(P(j, &c, NULL) == RU_CFG_BAD_URL && c.token[0] == 0 && c.url[0] == 0);
  snprintf(j, sizeof j, "{\"v\":1,\"domoticz\":{\"url\":\"http://h\",\"token\":\"%s\"}}", JWT);
  CHECK(P(j, &c, NULL) == RU_CFG_BAD_URL); /* missing flag == false */
  snprintf(j, sizeof j, "{\"v\":1,\"domoticz\":{\"url\":\"https://dz.example.org\",\"token\":\"%s\"},\"force\":true,\"x\":[1,{\"a\":null}]}", JWT);
  CHECK(P(j, &c, &force) == RU_CFG_OK && force && !c.insecure_lan);
  /* https + insecure_lan (self-signed LAN install) is accepted and kept */
  snprintf(j, sizeof j, "{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"t\",\"insecure_lan\":true}}");
  CHECK(P(j, &c, NULL) == RU_CFG_OK && c.insecure_lan);
  /* versions */
  CHECK(P("{\"v\":2,\"domoticz\":{\"url\":\"https://h\",\"token\":\"t\"}}", &c, NULL) == RU_CFG_BAD_VERSION);
  CHECK(P("{\"v\":0,\"domoticz\":{\"url\":\"https://h\",\"token\":\"t\"}}", &c, NULL) == RU_CFG_BAD_VERSION);
  CHECK(P("{\"v\":-1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"t\"}}", &c, NULL) == RU_CFG_BAD_VERSION);
  CHECK(P("{\"v\":1.5,\"domoticz\":{\"url\":\"https://h\",\"token\":\"t\"}}", &c, NULL) == RU_CFG_BAD_VERSION);
  CHECK(P("{\"v\":\"1\",\"domoticz\":{\"url\":\"https://h\",\"token\":\"t\"}}", &c, NULL) == RU_CFG_BAD_JSON);
  CHECK(P("{\"domoticz\":{\"url\":\"https://h\",\"token\":\"t\"}}", &c, NULL) == RU_CFG_BAD_JSON);
  /* shape errors */
  CHECK(P("", &c, NULL) == RU_CFG_BAD_JSON);
  CHECK(P("[]", &c, NULL) == RU_CFG_BAD_JSON);
  CHECK(P("{}", &c, NULL) == RU_CFG_BAD_JSON);
  CHECK(P("{\"v\":1}", &c, NULL) == RU_CFG_BAD_JSON);
  CHECK(P("{\"v\":1,\"domoticz\":{}}", &c, NULL) == RU_CFG_BAD_JSON);
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":5,\"token\":\"t\"}}", &c, NULL) == RU_CFG_BAD_JSON);
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"t\",\"insecure_lan\":1}}", &c, NULL) == RU_CFG_BAD_JSON);
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"t\"}} x", &c, NULL) == RU_CFG_BAD_JSON); /* trailing garbage */
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"t\"}", &c, NULL) == RU_CFG_BAD_JSON);      /* truncated */
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"\"}}", &c, NULL) == RU_CFG_BAD_JSON);      /* empty token */
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":\"https://u:p@h\",\"token\":\"t\"}}", &c, NULL) == RU_CFG_BAD_URL);
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":\"https://h/?a=1\",\"token\":\"t\"}}", &c, NULL) == RU_CFG_BAD_URL);
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"a b\"}}", &c, NULL) == RU_CFG_BAD_JSON);
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"a\\nb\"}}", &c, NULL) == RU_CFG_BAD_JSON); /* escape decodes to control */
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"a\\u0062c\"}}", &c, NULL) == RU_CFG_OK && !strcmp(c.token, "abc"));
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"a\\ud800\"}}", &c, NULL) == RU_CFG_BAD_JSON); /* lone surrogate */
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"a\\q\"}}", &c, NULL) == RU_CFG_BAD_JSON);
  /* depth cap */
  char deep[600] = "{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"t\"},\"x\":";
  size_t n = strlen(deep);
  for (int i = 0; i < 200; i++) deep[n++] = '[';
  for (int i = 0; i < 200; i++) deep[n++] = ']';
  deep[n++] = '}';
  deep[n] = 0;
  CHECK(P(deep, &c, NULL) == RU_CFG_BAD_JSON);
  CHECK(P("{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"t\"},\"x\":[[[[1]]]]}", &c, NULL) == RU_CFG_OK);
  /* length caps */
  char *big = malloc(4000);
  char *tok = malloc(1000);
  memset(tok, 'a', 999);
  tok[999] = 0;
  snprintf(big, 4000, "{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"%s\"}}", tok);
  CHECK(P(big, &c, NULL) == RU_CFG_TOO_LONG);
  tok[100] = 0;
  char u[400];
  memset(u, 'b', 300);
  u[300] = 0;
  snprintf(big, 4000, "{\"v\":1,\"domoticz\":{\"url\":\"https://%s\",\"token\":\"%s\"}}", u, tok);
  CHECK(P(big, &c, NULL) == RU_CFG_TOO_LONG);
  free(big);
  free(tok);
  /* NUL inside input: length wins, parse fails */
  CHECK(ru_cfg_parse_json("{\"v\":1\0,\"x\":1}", 15, &c, NULL) != RU_CFG_OK);
}

static void test_store(void) {
  static ru_kv_mem_t m;
  ru_kv_mem_init(&m);
  ru_cfg_t c, back;
  memset(&c, 0, sizeof c);
  CHECK(ru_cfg_load(&m.base, &back) == RU_CFG_NOT_SET && !ru_cfg_is_set(&m.base));
  snprintf(c.url, sizeof c.url, "http://192.168.1.5:8080");
  snprintf(c.token, sizeof c.token, "%s", JWT);
  c.insecure_lan = true;
  CHECK(ru_cfg_save(&m.base, &c) == RU_CFG_OK && ru_cfg_is_set(&m.base));
  CHECK(ru_kv_mem_count(&m, "secrets") == 4 && ru_kv_mem_count(&m, NULL) == 4); /* everything secret is in "secrets" */
  CHECK(ru_cfg_load(&m.base, &back) == RU_CFG_OK && !strcmp(back.url, c.url) && !strcmp(back.token, c.token) && back.insecure_lan);
  /* masked accessor reveals nothing */
  char mk[32];
  ru_cfg_token_masked(&back, mk, sizeof mk);
  CHECK(!strcmp(mk, "********") && !strstr(mk, "eyJ"));
  ru_cfg_token_masked(&back, mk, 4);
  CHECK(strlen(mk) == 3);
  ru_cfg_wipe(&back);
  ru_cfg_token_masked(&back, mk, sizeof mk);
  CHECK(!strcmp(mk, "(none)"));
  CHECK(back.token[0] == 0 && back.token[5] == 0 && back.url[0] == 0);
  /* invalid never saved */
  ru_cfg_t bad = c;
  bad.insecure_lan = false;
  CHECK(ru_cfg_save(&m.base, &bad) == RU_CFG_BAD_URL);
  snprintf(bad.url, sizeof bad.url, "http://H/");
  bad.insecure_lan = true;
  CHECK(ru_cfg_save(&m.base, &bad) == RU_CFG_BAD_URL); /* not normalised */
  /* write failure at every step leaves nothing loadable */
  for (int fail = 0; fail < 4; fail++) {
    ru_kv_mem_init(&m);
    m.fail_sets_after = fail;
    CHECK(ru_cfg_save(&m.base, &c) == RU_CFG_STORE);
    CHECK(!ru_cfg_is_set(&m.base) && ru_cfg_load(&m.base, &back) == RU_CFG_NOT_SET);
    CHECK(!ru_kv_mem_contains(&m, JWT, strlen(JWT)));
  }
  /* failed re-save must not leave a mix of old and new */
  ru_kv_mem_init(&m);
  CHECK(ru_cfg_save(&m.base, &c) == RU_CFG_OK);
  m.fail_sets_after = (int)m.set_calls + 2;
  ru_cfg_t c2 = c;
  snprintf(c2.url, sizeof c2.url, "https://other.example");
  CHECK(ru_cfg_save(&m.base, &c2) == RU_CFG_STORE && ru_cfg_load(&m.base, &back) == RU_CFG_NOT_SET);
  /* tampered store is rejected on load */
  ru_kv_mem_init(&m);
  CHECK(ru_cfg_save(&m.base, &c) == RU_CFG_OK);
  CHECK(ru_kv_set_str(&m.base, "secrets", "dz_url", "ftp://x") == RU_KV_OK);
  CHECK(ru_cfg_load(&m.base, &back) == RU_CFG_NOT_SET && back.token[0] == 0);
  ru_kv_mem_init(&m);
  CHECK(ru_cfg_save(&m.base, &c) == RU_CFG_OK && ru_cfg_clear(&m.base) == RU_CFG_OK && !ru_cfg_is_set(&m.base) &&
        ru_kv_mem_count(&m, NULL) == 0);
  /* wifi */
  CHECK(ru_wifi_creds_valid("net", "") && ru_wifi_creds_valid("net", "12345678") && !ru_wifi_creds_valid("net", "short") &&
        !ru_wifi_creds_valid("", "") && !ru_wifi_creds_valid("0123456789012345678901234567890123", ""));
  ru_wifi_creds_t w;
  CHECK(ru_wifi_creds_save(&m.base, "home", "hunter2hunter2") == RU_CFG_OK);
  CHECK(ru_wifi_creds_load(&m.base, &w) == RU_CFG_OK && !strcmp(w.ssid, "home") && !strcmp(w.pass, "hunter2hunter2"));
  ru_wifi_creds_wipe(&w);
  CHECK(w.pass[0] == 0 && w.pass[3] == 0);
  CHECK(ru_wifi_creds_clear(&m.base) == RU_CFG_OK && ru_wifi_creds_load(&m.base, &w) == RU_CFG_NOT_SET);
  CHECK(ru_wifi_creds_save(&m.base, "home", "short") == RU_CFG_BAD_JSON);
  unsigned char buf[16] = "secret";
  ru_secure_zero(buf, sizeof buf);
  CHECK(buf[0] == 0 && buf[5] == 0);
}

/* ---------------- Wi-Fi block of RU+CFG (same vectors as tests/site/test_lib.mjs 'wifi vectors') ---------------- */
static void test_wifi_line(void) {
  char x33[34], x32[33], p63[64], p64[65], p64bad[65], p65[66];
  memset(x33, 'x', 33); x33[33] = 0;
  memset(x32, 'x', 32); x32[32] = 0;
  memset(p63, 'a', 63); p63[63] = 0;
  memcpy(p64, "0123456789abcdefABCDEF0123456789abcdefABCDEF0123456789abcdefABCD", 65);
  memcpy(p64bad, p64, 65); p64bad[63] = 'g';
  memset(p65, 'a', 65); p65[65] = 0;
  CHECK(strlen(p64) == 64);
  const char *ssid_ok[] = {"home", "a", x32, "Caf\xC3\xA9", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" "wifi",
                           "\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80",
                           " with spaces ", "quo\"te'\\"};
  for (size_t i = 0; i < sizeof ssid_ok / sizeof *ssid_ok; i++) CHECK(ru_wifi_ssid_check(ssid_ok[i]));
  const char *ssid_bad[] = {"", x33, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80x",
                            "tab\there", "nl\n", "del\x7f", "c1\xC2\x85x", "\xED\xA0\x80", "\xC0\xAF", "\xE0\x80\x80", "\xF4\x90\x80\x80",
                            "\xFF", "trunc\xE6\x97", "bare\x80"};
  for (size_t i = 0; i < sizeof ssid_bad / sizeof *ssid_bad; i++) CHECK(!ru_wifi_ssid_check(ssid_bad[i]));
  CHECK(!ru_wifi_ssid_check(NULL));
  const char *pass_ok[] = {"", "12345678", p63, p64, "pass word with spaces", "p@$$w0rd!#%^&*()~`", "\"quoted\\"};
  for (size_t i = 0; i < sizeof pass_ok / sizeof *pass_ok; i++) CHECK(ru_wifi_pass_check(pass_ok[i]));
  const char *pass_bad[] = {"1234567", p64bad, p65, "p\xC3\xA4ssw\xC3\xB6rd1", "pass\tword12", "del\x7f" "pass12", "line\nbreak12"};
  for (size_t i = 0; i < sizeof pass_bad / sizeof *pass_bad; i++) CHECK(!ru_wifi_pass_check(pass_bad[i]));
  CHECK(!ru_wifi_pass_check(NULL));

  /* line parser */
  ru_cfg_line_t L;
  char j[1800];
  snprintf(j, sizeof j, "{\"v\":1,\"wifi\":{\"ssid\":\"home\",\"password\":\"hunter2hunter2\"}}");
  CHECK(ru_cfg_parse_line(j, strlen(j), &L) == RU_CFG_OK && L.have_wifi && !L.have_dz && !strcmp(L.wifi.ssid, "home") &&
        !strcmp(L.wifi.pass, "hunter2hunter2"));
  snprintf(j, sizeof j, "{\"v\":1,\"wifi\":{\"ssid\":\"open\",\"password\":\"\"},\"x\":{\"a\":[1]}}");
  CHECK(ru_cfg_parse_line(j, strlen(j), &L) == RU_CFG_OK && L.have_wifi && L.wifi.pass[0] == 0);
  snprintf(j, sizeof j, "{\"v\":1,\"domoticz\":{\"url\":\"https://h\",\"token\":\"%s\"}}", JWT);
  CHECK(ru_cfg_parse_line(j, strlen(j), &L) == RU_CFG_OK && !L.have_wifi && L.have_dz);
  snprintf(j, sizeof j, "{\"v\":1,\"wifi\":{\"ssid\":\"home\",\"password\":\"hunter2hunter2\"},\"domoticz\":{\"url\":\"https://h\",\"token\":\"%s\"},\"force\":true}", JWT);
  CHECK(ru_cfg_parse_line(j, strlen(j), &L) == RU_CFG_OK && L.have_wifi && L.have_dz && L.force && !strcmp(L.dz.token, JWT));
  /* the old Domoticz-only view is not fooled by a wifi-only line */
  ru_cfg_t c;
  snprintf(j, sizeof j, "{\"v\":1,\"wifi\":{\"ssid\":\"home\",\"password\":\"\"}}");
  CHECK(ru_cfg_parse_json(j, strlen(j), &c, NULL) == RU_CFG_BAD_JSON && c.token[0] == 0);
  /* neither block */
  CHECK(ru_cfg_parse_line("{\"v\":1}", 7, &L) == RU_CFG_BAD_JSON);
  CHECK(ru_cfg_parse_line("{\"v\":1,\"wifi\":{}}", 17, &L) == RU_CFG_BAD_WIFI);
  CHECK(ru_cfg_parse_line("{\"v\":1,\"wifi\":null}", 19, &L) == RU_CFG_BAD_JSON);
  /* bad wifi values and the error order (wifi before domoticz; version first) */
  const char *bad[] = {
      "{\"v\":1,\"wifi\":{\"ssid\":\"\",\"password\":\"\"}}",
      "{\"v\":1,\"wifi\":{\"ssid\":\"home\"}}",                        /* password missing: not silently open */
      "{\"v\":1,\"wifi\":{\"password\":\"\"}}",
      "{\"v\":1,\"wifi\":{\"ssid\":\"home\",\"password\":\"short\"}}",
      "{\"v\":1,\"wifi\":{\"ssid\":\"a\\u0000b\",\"password\":\"\"}}",   /* embedded NUL */
      "{\"v\":1,\"wifi\":{\"ssid\":\"a\\u0007b\",\"password\":\"\"}}",
      "{\"v\":1,\"wifi\":{\"ssid\":\"home\",\"password\":\"p\\u00e4ssword1\"}}",
      "{\"v\":1,\"wifi\":{\"ssid\":\"home\",\"password\":\"12345678\\u0000\"}}",
      "{\"v\":1,\"wifi\":{\"ssid\":\"home\",\"password\":\"0123456789012345678901234567890123456789012345678901234567890123456789\"}}",
      "{\"v\":1,\"wifi\":{\"ssid\":\"home\",\"password\":\"\"},\"domoticz\":{\"url\":\"ftp://x\",\"token\":\"t\"}}", /* wifi checked first? no: wifi ok, dz bad -> bad_url */
  };
  for (size_t i = 0; i + 1 < sizeof bad / sizeof *bad; i++) {
    CHECK(ru_cfg_parse_line(bad[i], strlen(bad[i]), &L) == RU_CFG_BAD_WIFI);
    CHECK(!L.have_wifi && L.wifi.ssid[0] == 0 && L.wifi.pass[0] == 0);
  }
  CHECK(ru_cfg_parse_line(bad[9], strlen(bad[9]), &L) == RU_CFG_BAD_URL && !L.have_wifi && L.wifi.ssid[0] == 0); /* nothing half-filled */
  snprintf(j, sizeof j, "{\"v\":1,\"wifi\":{\"ssid\":\"\",\"password\":\"\"},\"domoticz\":{\"url\":\"ftp://x\",\"token\":\"t\"}}");
  CHECK(ru_cfg_parse_line(j, strlen(j), &L) == RU_CFG_BAD_WIFI);
  snprintf(j, sizeof j, "{\"v\":2,\"wifi\":{\"ssid\":\"\",\"password\":\"\"}}");
  CHECK(ru_cfg_parse_line(j, strlen(j), &L) == RU_CFG_BAD_VERSION);
  /* oversized ssid / password: refused, no truncation into a valid value */
  {
    char big[1200];
    char fill[700];
    memset(fill, 'a', sizeof fill); fill[sizeof fill - 1] = 0;
    snprintf(big, sizeof big, "{\"v\":1,\"wifi\":{\"ssid\":\"%s\",\"password\":\"\"}}", fill);
    CHECK(ru_cfg_parse_line(big, strlen(big), &L) == RU_CFG_BAD_WIFI);
    snprintf(big, sizeof big, "{\"v\":1,\"wifi\":{\"ssid\":\"home\",\"password\":\"%s\"}}", fill);
    CHECK(ru_cfg_parse_line(big, strlen(big), &L) == RU_CFG_BAD_WIFI);
  }
  /* escaped but valid */
  snprintf(j, sizeof j, "{\"v\":1,\"wifi\":{\"ssid\":\"Caf\\u00e9 \\\"x\\\"\",\"password\":\"a\\\\b\\\"c12345\"}}");
  CHECK(ru_cfg_parse_line(j, strlen(j), &L) == RU_CFG_OK && !strcmp(L.wifi.ssid, "Caf\xC3\xA9 \"x\"") && !strcmp(L.wifi.pass, "a\\b\"c12345"));
  /* depth */
  snprintf(j, sizeof j, "{\"v\":1,\"wifi\":{\"ssid\":\"h\",\"password\":\"\",\"x\":[[[[[[[1]]]]]]]}}");
  CHECK(ru_cfg_parse_line(j, strlen(j), &L) == RU_CFG_BAD_JSON);
  /* masking: fixed text, no content, no length */
  ru_wifi_creds_t w;
  char m[16];
  memset(&w, 0, sizeof w);
  ru_wifi_pass_masked(&w, m, sizeof m);
  CHECK(!strcmp(m, "(none)"));
  snprintf(w.pass, sizeof w.pass, "hunter2hunter2");
  ru_wifi_pass_masked(&w, m, sizeof m);
  CHECK(!strcmp(m, "********") && !strstr(m, "hunter"));
  snprintf(w.pass, sizeof w.pass, "%s", p63);
  char m2[16];
  ru_wifi_pass_masked(&w, m2, sizeof m2);
  CHECK(!strcmp(m, m2)); /* same text for any length */
  ru_wifi_pass_masked(&w, m, 4);
  CHECK(!strcmp(m, "***"));
  ru_wifi_pass_masked(NULL, m, sizeof m);
  CHECK(!strcmp(m, "(none)"));
  ru_wifi_pass_masked(&w, NULL, 0); /* must not crash */
  /* wipe */
  ru_cfg_line_wipe(&L);
  CHECK(L.wifi.ssid[0] == 0 && L.wifi.pass[5] == 0 && L.dz.token[3] == 0);
  /* the biggest line the console accepts still parses: 32 byte ssid made only of escaped quotes, 63 char password of
   * escaped backslashes, max url and a max size token (see RU_CFGLINE_MAX 2048 in ru_cfgline.h) */
  {
    char ss[200] = "", pp[400] = "", url[256], tok[RU_CFG_TOKEN_MAX], line[2200];
    for (int i = 0; i < 32; i++) strcat(ss, "\\\"");
    for (int i = 0; i < 63; i++) strcat(pp, "\\\\");
    snprintf(url, sizeof url, "https://dz.example.org/%.150s", "pppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp");
    memset(tok, 'T', sizeof tok); tok[sizeof tok - 1] = 0;
    snprintf(line, sizeof line, "{\"v\":1,\"wifi\":{\"ssid\":\"%s\",\"password\":\"%s\"},\"domoticz\":{\"url\":\"%s\",\"token\":\"%s\",\"insecure_lan\":false},\"force\":true}", ss, pp, url, tok);
    CHECK(strlen(line) + 7 <= 2048); /* fits with the "RU+CFG " prefix even with every character escaped */
    CHECK(ru_cfg_parse_line(line, strlen(line), &L) == RU_CFG_OK && strlen(L.wifi.ssid) == 32 && strlen(L.wifi.pass) == 63);
    ru_cfg_line_wipe(&L);
  }
}


/* A NUL escape inside a secret used to cut it short silently: the device would store a DIFFERENT token or address than
 * the one sent. It must be refused instead, in every position, and never produce a stored value. */
static void test_embedded_nul(void) {
  ru_cfg_line_t L;
  char j[600];
  const char *tails[] = {"abc\\u0000def", "\\u0000", "abc\\u0000", "\\u0000abc", "a\\u0000\\u0000b"};
  for (size_t i = 0; i < sizeof tails / sizeof *tails; i++) {
    snprintf(j, sizeof j, "{\"v\":1,\"domoticz\":{\"url\":\"http://192.168.1.5:8080\",\"token\":\"%s\",\"insecure_lan\":true}}", tails[i]);
    CHECK(ru_cfg_parse_line(j, strlen(j), &L) != RU_CFG_OK);                 /* token */
    CHECK(!L.have_dz || L.dz.token[0] == 0);                                 /* nothing half-parsed is handed back */
    snprintf(j, sizeof j, "{\"v\":1,\"domoticz\":{\"url\":\"http://host%s\",\"token\":\"%s\",\"insecure_lan\":true}}", tails[i], JWT);
    CHECK(ru_cfg_parse_line(j, strlen(j), &L) != RU_CFG_OK);                 /* url */
  }
  /* the escape is only a problem for NUL: an ordinary escape still works */
  snprintf(j, sizeof j, "{\"v\":1,\"domoticz\":{\"url\":\"http:\\/\\/192.168.1.5:8080\",\"token\":\"%s\",\"insecure_lan\":true}}", JWT);
  CHECK(ru_cfg_parse_line(j, strlen(j), &L) == RU_CFG_OK && !strcmp(L.dz.url, "http://192.168.1.5:8080"));
}

int main(void) {
  test_kv();
  test_url();
  test_token();
  test_parse();
  test_store();
  test_wifi_line();
  test_embedded_nul();
  MT_DONE();
}
