#include "ru_cfgline.h"

#include <stdio.h>
#include <string.h>

static size_t put(char *reply, size_t cap, const char *s) {
  int n = snprintf(reply, cap, "%s", s);
  if (n < 0) { if (cap) reply[0] = 0; return 0; }
  return (size_t)n >= cap ? (cap ? cap - 1 : 0) : (size_t)n;
}

static size_t err(char *reply, size_t cap, const char *code, const char *detail) {
  char b[RU_CFGLINE_REPLY_MAX];
  snprintf(b, sizeof b, "RU+CFG ERR %s%s%s", code, detail ? " " : "", detail ? detail : "");
  return put(reply, cap, b);
}

size_t ru_cfgline_reply_too_long(char *reply, size_t cap) { return err(reply, cap, "too_long", "2048"); }

/* only characters that cannot carry structure; server text never reaches the console unfiltered */
static void sanitise(const char *in, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; in && in[i] && o + 1 < cap && o < 40; i++) {
    unsigned char c = (unsigned char)in[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-' || c == '(' || c == ')' || c == ' ';
    out[o++] = ok ? (char)c : '?';
  }
  out[o] = 0;
}

static const char *wifi_detail(ru_wifi_apply_t w) {
  switch (w) {
    case RU_WIFI_APPLY_NO_AP: return "no_ap";
    case RU_WIFI_APPLY_AUTH: return "auth";
    case RU_WIFI_APPLY_TIMEOUT: return "timeout";
    default: return NULL;
  }
}

/* error reply with an optional detail and, when Wi-Fi was already saved, the trailing marker word */
static size_t err_w(char *reply, size_t cap, const char *code, const char *detail, bool wifi_saved) {
  char b[RU_CFGLINE_REPLY_MAX];
  snprintf(b, sizeof b, "RU+CFG ERR %s%s%s%s", code, detail ? " " : "", detail ? detail : "", wifi_saved ? " wifi_saved" : "");
  return put(reply, cap, b);
}

size_t ru_cfgline_handle(ru_kv_t *kv, const ru_cfgline_ops_t *ops, const char *json, size_t len, char *reply, size_t cap) {
  ru_cfg_line_t L;
  size_t rl;
  if (!reply || !cap) return 0;
  reply[0] = 0;
  if (len + RU_CFGLINE_PREFIX_LEN > RU_CFGLINE_MAX) return ru_cfgline_reply_too_long(reply, cap);
  ru_cfg_err_t pe = ru_cfg_parse_line(json, len, &L);
  if (pe != RU_CFG_OK) {
    ru_cfg_line_wipe(&L);
    return err(reply, cap, ru_cfg_err_name(pe), pe == RU_CFG_BAD_VERSION ? "update_firmware_or_installer" : NULL);
  }
  bool wifi_saved = false;
  if (L.have_wifi) {
    ru_wifi_apply_t w = ops && ops->wifi_apply ? ops->wifi_apply(ops->user, L.wifi.ssid, L.wifi.pass, RU_CFGLINE_WIFI_TIMEOUT_MS)
                                               : RU_WIFI_APPLY_FAILED;
    ru_wifi_creds_wipe(&L.wifi);
    if (w == RU_WIFI_APPLY_STORE) { ru_cfg_line_wipe(&L); return err(reply, cap, "store", NULL); }
    if (w != RU_WIFI_APPLY_OK) { ru_cfg_line_wipe(&L); return err(reply, cap, "wifi_failed", wifi_detail(w)); }
    wifi_saved = true;
    if (!L.have_dz) {
      ru_cfg_line_wipe(&L);
      return put(reply, cap, "RU+CFG OK wifi_saved");
    }
  }
  const char *ws_word = wifi_saved ? "wifi_saved " : "";
  char info[48] = "";
  ru_cfg_test_t t = ops && ops->test ? ops->test(ops->user, &L.dz, RU_CFGLINE_TIMEOUT_MS, info, sizeof info) : RU_CFG_TEST_UNREACHABLE;
  bool save = t == RU_CFG_TEST_OK || (t == RU_CFG_TEST_UNREACHABLE && L.force);
  if (save) {
    ru_cfg_err_t se = ru_cfg_save(kv, &L.dz);
    ru_cfg_line_wipe(&L);
    if (se != RU_CFG_OK) return err_w(reply, cap, "store", NULL, wifi_saved);
    if (ops && ops->saved) ops->saved(ops->user);
    char clean[48], b[RU_CFGLINE_REPLY_MAX];
    if (t == RU_CFG_TEST_OK) {
      sanitise(info, clean, sizeof clean);
      snprintf(b, sizeof b, "RU+CFG OK %ssaved%s%s", ws_word, clean[0] ? " " : "", clean);
    } else snprintf(b, sizeof b, "RU+CFG OK %ssaved untested", ws_word);
    rl = put(reply, cap, b);
    return rl;
  }
  ru_cfg_line_wipe(&L);
  switch (t) {
    case RU_CFG_TEST_TLS: return err_w(reply, cap, "tls", NULL, wifi_saved);
    case RU_CFG_TEST_TOKEN_REJECTED: return err_w(reply, cap, "token_rejected", NULL, wifi_saved);
    case RU_CFG_TEST_NO_WIFI: return err_w(reply, cap, "no_wifi", NULL, wifi_saved);
    case RU_CFG_TEST_BAD_RESPONSE: return err_w(reply, cap, "unreachable", "not_domoticz", wifi_saved);
    default: return err_w(reply, cap, "unreachable", NULL, wifi_saved);
  }
}
