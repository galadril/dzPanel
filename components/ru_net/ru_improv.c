#include "ru_improv.h"

#include <stdio.h>
#include <string.h>

enum { T_STATE = 1, T_ERROR = 2, T_RPC = 3, T_RESULT = 4 };
enum { C_WIFI = 1, C_STATE = 2, C_INFO = 3, C_SCAN = 4 };

static const char HDR[6] = {'I', 'M', 'P', 'R', 'O', 'V'};

static void emit(ru_improv_t *im, uint8_t type, const uint8_t *data, size_t n) {
  uint8_t f[RU_IMPROV_MAX_PACKET + 2];
  if (n > 255 || !im->ops.write) return;
  size_t o = 0;
  f[o++] = '\n';
  memcpy(f + o, HDR, 6); o += 6;
  f[o++] = 1;
  f[o++] = type;
  f[o++] = (uint8_t)n;
  if (n) memcpy(f + o, data, n);
  o += n;
  uint8_t sum = 0;
  for (size_t i = 1; i < o; i++) sum = (uint8_t)(sum + f[i]);
  f[o++] = sum;
  f[o++] = '\n';
  im->ops.write(im->ops.user, f, o);
}

static void send_state(ru_improv_t *im) {
  uint8_t s = (uint8_t)im->state;
  emit(im, T_STATE, &s, 1);
}
static void send_error(ru_improv_t *im, ru_improv_error_t e) {
  uint8_t b = (uint8_t)e;
  im->error = e;
  im->errs++;
  emit(im, T_ERROR, &b, 1);
}
/* RPC result: command, n, then strings. Returns false when the strings do not fit (never truncates). */
static bool send_result(ru_improv_t *im, uint8_t cmd, const char *const *strs, size_t ns) {
  uint8_t d[258];
  size_t o = 2;
  for (size_t i = 0; i < ns; i++) {
    size_t l = strlen(strs[i]);
    if (l > 255 || o + 1 + l > 2 + 255) return false;
    d[o++] = (uint8_t)l;
    memcpy(d + o, strs[i], l);
    o += l;
  }
  d[0] = cmd;
  d[1] = (uint8_t)(o - 2);
  emit(im, T_RESULT, d, o);
  return true;
}

void ru_improv_init(ru_improv_t *im, const ru_improv_ops_t *ops, bool authorized) {
  memset(im, 0, sizeof *im);
  im->ops = *ops;
  im->authorized = authorized;
  im->state = authorized ? RU_IMPROV_STATE_AUTHORIZED : RU_IMPROV_STATE_AUTH_REQUIRED;
}

void ru_improv_set_authorized(ru_improv_t *im, bool a) {
  im->authorized = a;
  if (im->state == RU_IMPROV_STATE_AUTH_REQUIRED || im->state == RU_IMPROV_STATE_AUTHORIZED)
    im->state = a ? RU_IMPROV_STATE_AUTHORIZED : RU_IMPROV_STATE_AUTH_REQUIRED;
  send_state(im);
}

static const char *nz(const char *s) { return s ? s : ""; }

static void cmd_state(ru_improv_t *im) {
  bool up = im->ops.wifi_connected && im->ops.wifi_connected(im->ops.user);
  if (up && im->state != RU_IMPROV_STATE_PROVISIONING) im->state = RU_IMPROV_STATE_PROVISIONED;
  else if (!up && im->state == RU_IMPROV_STATE_PROVISIONED) im->state = im->authorized ? RU_IMPROV_STATE_AUTHORIZED : RU_IMPROV_STATE_AUTH_REQUIRED;
  send_state(im);
  if (im->state == RU_IMPROV_STATE_PROVISIONED) {
    const char *u = nz(im->ops.redirect_url);
    send_result(im, C_STATE, u[0] ? &u : NULL, u[0] ? 1 : 0);
  }
}

static void cmd_info(ru_improv_t *im) {
  const char *s[4] = {nz(im->ops.fw_name), nz(im->ops.fw_version), nz(im->ops.chip), nz(im->ops.device_name)};
  send_result(im, C_INFO, s, 4);
}

static void cmd_scan(ru_improv_t *im) {
  ru_improv_net_t nets[RU_IMPROV_MAX_NETWORKS];
  size_t n = im->ops.wifi_scan ? im->ops.wifi_scan(im->ops.user, nets, RU_IMPROV_MAX_NETWORKS) : 0;
  if (n > RU_IMPROV_MAX_NETWORKS) n = RU_IMPROV_MAX_NETWORKS;
  for (size_t i = 0; i < n; i++) {
    char rssi[12];
    snprintf(rssi, sizeof rssi, "%d", nets[i].rssi);
    nets[i].ssid[RU_IMPROV_SSID_MAX] = 0;
    const char *s[3] = {nets[i].ssid, rssi, nets[i].secured ? "YES" : "NO"};
    send_result(im, C_SCAN, s, 3);
  }
  send_result(im, C_SCAN, NULL, 0); /* end of list */
}

/* payload: ssid_len ssid pass_len pass, exactly filling n bytes */
static bool parse_wifi(const uint8_t *p, size_t n, char *ssid, char *pass) {
  if (n < 2) return false;
  size_t sl = p[0];
  if (sl < 1 || sl > RU_IMPROV_SSID_MAX || 1 + sl + 1 > n) return false;
  size_t pl = p[1 + sl];
  if (pl > RU_IMPROV_PASS_MAX || 1 + sl + 1 + pl != n) return false;
  memcpy(ssid, p + 1, sl); ssid[sl] = 0;
  memcpy(pass, p + 2 + sl, pl); pass[pl] = 0;
  if (strlen(ssid) != sl || strlen(pass) != pl) return false; /* embedded NUL */
  return true;
}

static void cmd_wifi(ru_improv_t *im, const uint8_t *p, size_t n) {
  char ssid[RU_IMPROV_SSID_MAX + 1], pass[RU_IMPROV_PASS_MAX + 1];
  if (!im->authorized) { send_error(im, RU_IMPROV_ERR_NOT_AUTHORIZED); return; }
  if (im->state == RU_IMPROV_STATE_PROVISIONING) { send_error(im, RU_IMPROV_ERR_INVALID_RPC); return; }
  if (!parse_wifi(p, n, ssid, pass)) { send_error(im, RU_IMPROV_ERR_INVALID_RPC); return; }
  im->state = RU_IMPROV_STATE_PROVISIONING;
  send_state(im);
  bool started = im->ops.wifi_connect && im->ops.wifi_connect(im->ops.user, ssid, pass);
  memset(pass, 0, sizeof pass);
  if (!started) ru_improv_wifi_result(im, false);
}

void ru_improv_wifi_result(ru_improv_t *im, bool ok) {
  if (im->state != RU_IMPROV_STATE_PROVISIONING) return;
  if (ok) {
    im->state = RU_IMPROV_STATE_PROVISIONED;
    send_state(im);
    const char *u = nz(im->ops.redirect_url);
    send_result(im, C_WIFI, u[0] ? &u : NULL, u[0] ? 1 : 0);
  } else {
    send_error(im, RU_IMPROV_ERR_UNABLE_TO_CONNECT);
    im->state = im->authorized ? RU_IMPROV_STATE_AUTHORIZED : RU_IMPROV_STATE_AUTH_REQUIRED;
    send_state(im);
  }
}

static void dispatch(ru_improv_t *im) {
  uint8_t type = im->buf[7], len = im->buf[8];
  const uint8_t *d = &im->buf[9];
  if (type != T_RPC) return; /* host never sends the other types; ignore */
  if (len < 2 || d[1] != len - 2) { im->bad_packets++; send_error(im, RU_IMPROV_ERR_INVALID_RPC); return; }
  uint8_t cmd = d[0];
  size_t pn = d[1];
  bool had_error = im->error != RU_IMPROV_ERR_NONE;
  uint32_t errs_before = im->errs;
  switch (cmd) {
    case C_WIFI: cmd_wifi(im, d + 2, pn); break;
    case C_STATE: cmd_state(im); break;
    case C_INFO: cmd_info(im); break;
    case C_SCAN: cmd_scan(im); break;
    default: send_error(im, RU_IMPROV_ERR_UNKNOWN_RPC); return;
  }
  /* a command that raised no new error clears the previous one, as the spec describes */
  if (had_error && im->errs == errs_before) send_error(im, RU_IMPROV_ERR_NONE);
}

void ru_improv_reset(ru_improv_t *im) { im->pos = 0; }
bool ru_improv_in_packet(const ru_improv_t *im) { return im->pos > 0; }

ru_improv_feed_t ru_improv_feed(ru_improv_t *im, uint8_t b) {
  size_t p = im->pos;
  if (p < 6) {
    if (b != (uint8_t)HDR[p]) { im->pos = 0; return RU_IMPROV_NOMATCH; }
  } else if (p == 6) {
    if (b != 1) { im->pos = 0; return RU_IMPROV_NOMATCH; }
  }
  im->buf[p] = b;
  im->pos = p + 1;
  if (p < 8) return RU_IMPROV_MORE;
  size_t total = 9 + (size_t)im->buf[8] + 1;
  if (im->pos < total) return RU_IMPROV_MORE;
  uint8_t sum = 0;
  for (size_t i = 0; i + 1 < total; i++) sum = (uint8_t)(sum + im->buf[i]);
  bool ok = sum == im->buf[total - 1];
  if (ok) dispatch(im);
  else { im->bad_packets++; send_error(im, RU_IMPROV_ERR_INVALID_RPC); }
  im->pos = 0;
  return RU_IMPROV_DONE;
}
