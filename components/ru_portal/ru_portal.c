#include "ru_portal.h"

#include <stdio.h>
#include <string.h>

/* ---------------- bounded writer and HTML escaping ---------------- */
typedef struct { char *b; size_t cap, n; bool ok; } wr_t;
static void w_init(wr_t *w, char *b, size_t cap) { w->b = b; w->cap = cap; w->n = 0; w->ok = cap > 0; if (cap) b[0] = 0; }
static void w_c(wr_t *w, char c) {
  if (!w->ok) return;
  if (w->n + 1 >= w->cap) { w->ok = false; return; }
  w->b[w->n++] = c;
  w->b[w->n] = 0;
}
static void w_s(wr_t *w, const char *s) { while (*s && w->ok) w_c(w, *s++); }
/* Every dynamic value goes through here. Control characters become '?', so nothing can break out of an attribute or a
 * tag. Bytes >= 0x80 pass through (UTF-8 SSIDs); the page declares utf-8 and a CSP forbids scripts anyway. */
static void w_esc(wr_t *w, const char *s) {
  for (; *s && w->ok; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
      case '&': w_s(w, "&amp;"); break;
      case '<': w_s(w, "&lt;"); break;
      case '>': w_s(w, "&gt;"); break;
      case '"': w_s(w, "&quot;"); break;
      case '\'': w_s(w, "&#39;"); break;
      default: w_c(w, (c < 0x20 || c == 0x7f) ? '?' : (char)c);
    }
  }
}

static void log_(const ru_portal_t *p, const char *m) { if (p->ops.log) p->ops.log(p->ops.user, m); }

/* ---------------- error texts (fixed strings; nothing from the user) ---------------- */
typedef enum {
  E_NONE = 0, E_CSRF, E_INPUT, E_JOIN, E_NEEDS_ACK, E_BAD_URL, E_BAD_TOKEN, E_TOO_LONG, E_UNREACHABLE, E_NOT_DOMOTICZ,
  E_TLS, E_REJECTED, E_NO_WIFI, E_STORE, E_VERSION, E_BUSY
} err_t;
static const char *err_text(err_t e) {
  switch (e) {
    case E_NONE: return "";
    case E_CSRF: return "This page has expired. Reload it and try again.";
    case E_INPUT: return "That input was not valid. Check the name and password (8 to 63 characters, or empty for an open network).";
    case E_JOIN: return "The device could not join that Wi-Fi network. Check the password and try again.";
    case E_NEEDS_ACK: return "Plain http sends your token unencrypted over your network. Tick the box to confirm you understand, or use an https address.";
    case E_BAD_URL: return "That address is not valid. Use a form like http://192.168.1.5:8080 or https://domoticz.example.";
    case E_BAD_TOKEN: return "That token is not valid: it must not be empty or contain spaces.";
    case E_TOO_LONG: return "That value is too long.";
    case E_UNREACHABLE: return "The device could not reach that address. Check the address and port, and that Domoticz is running.";
    case E_NOT_DOMOTICZ: return "That address answered, but not like a Domoticz server.";
    case E_TLS: return "The secure connection failed (certificate or handshake). Check the https address.";
    case E_REJECTED: return "Domoticz rejected the token. Create a new access token and try again.";
    case E_NO_WIFI: return "The device is not connected to Wi-Fi yet. Go back to step 1.";
    case E_STORE: return "The settings could not be saved. Try again.";
    case E_VERSION: return "This firmware cannot read these settings.";
    case E_BUSY: return "The device is still busy with another Wi-Fi attempt (for example from the USB installer). Wait a moment and try again.";
  }
  return "";
}

/* ---------------- pages ---------------- */
static void head(wr_t *w) {
  w_s(w, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>dzPanel setup</title>"
         "<style>body{font-family:sans-serif;max-width:32em;margin:1em auto;padding:0 1em;line-height:1.4}"
         "label{display:block;margin:.8em 0}input,select,button{font-size:1em;width:100%;box-sizing:border-box;padding:.5em}"
         "input[type=checkbox]{width:auto}.err{border:2px solid #b00;padding:.5em}"
         ".warn{border:2px solid #c80;padding:.5em}small{color:#555}</style></head><body><h1>Set up your dzPanel</h1>");
}
static void foot(wr_t *w) {
  w_s(w, "<p><small>This page is served over plain http by the device itself, on its own private Wi-Fi network that is "
         "protected by the password shown on the device. Do not use it on any other network.</small></p></body></html>");
}
static void err_box(wr_t *w, err_t e) {
  if (e == E_NONE) return;
  w_s(w, "<p class=\"err\" role=\"alert\">");
  w_esc(w, err_text(e));
  w_s(w, "</p>");
}
static void csrf_field(wr_t *w, const ru_portal_t *p) {
  w_s(w, "<input type=\"hidden\" name=\"csrf\" value=\"");
  w_esc(w, p->csrf);
  w_s(w, "\">");
}

static void page_wifi(wr_t *w, const ru_portal_t *p, err_t e) {
  head(w);
  w_s(w, "<h2>Step 1 of 2: Wi-Fi</h2><p>Choose the network the device should use at home.</p>");
  err_box(w, e);
  w_s(w, "<form method=\"post\" action=\"/wifi\">");
  csrf_field(w, p);
  ru_portal_net_t nets[RU_PORTAL_MAX_NETS];
  size_t n = 0;
  if (p->ops.scan) n = p->ops.scan(p->ops.user, nets, RU_PORTAL_MAX_NETS);
  if (n > RU_PORTAL_MAX_NETS) n = RU_PORTAL_MAX_NETS;
  if (n) {
    w_s(w, "<label>Network<select name=\"ssid\">");
    for (size_t i = 0; i < n; i++) {
      nets[i].ssid[sizeof nets[i].ssid - 1] = 0;
      w_s(w, "<option value=\"");
      w_esc(w, nets[i].ssid);
      w_s(w, "\">");
      w_esc(w, nets[i].ssid);
      w_s(w, nets[i].secured ? "</option>" : " (open)</option>");
    }
    w_s(w, "</select></label>");
  }
  w_s(w, "<label>");
  w_s(w, n ? "Or type the network name (hidden networks)" : "Network name");
  w_s(w, "<input type=\"text\" name=\"ssid_manual\" maxlength=\"32\" autocomplete=\"off\" autocapitalize=\"none\"></label>"
         "<label>Wi-Fi password<input type=\"password\" name=\"pass\" maxlength=\"64\" autocomplete=\"off\"></label>"
         "<button type=\"submit\">Connect</button></form>");
  foot(w);
}

static void page_domoticz(wr_t *w, const ru_portal_t *p, err_t e, const char *url_prefill) {
  head(w);
  w_s(w, "<h2>Step 2 of 2: Domoticz</h2><p>Connected to Wi-Fi <b>");
  w_esc(w, p->joined_ssid);
  w_s(w, "</b>.</p>");
  err_box(w, e);
  w_s(w, "<p class=\"warn\"><b>Warning:</b> an address that starts with <b>http://</b> is not encrypted, so the access token "
         "travels unprotected over your network. Prefer <b>https://</b>. Use a Viewer token if the display only needs to show things.</p>"
         "<form method=\"post\" action=\"/domoticz\">");
  csrf_field(w, p);
  w_s(w, "<label>Domoticz address<input type=\"text\" name=\"url\" inputmode=\"url\" maxlength=\"191\" autocomplete=\"off\" "
         "autocapitalize=\"none\" placeholder=\"http://192.168.1.5:8080\" value=\"");
  if (url_prefill) w_esc(w, url_prefill);
  w_s(w, "\"></label><label>Access token<input type=\"password\" name=\"token\" maxlength=\"767\" autocomplete=\"off\"></label>"
         "<label><input type=\"checkbox\" name=\"ack\" value=\"1\"> I understand that with an http:// address the token is sent unencrypted.</label>"
         "<button type=\"submit\">Test and save</button></form>"
         "<p><a href=\"/wifi\">Change the Wi-Fi network</a></p>");
  foot(w);
}

static void page_done(wr_t *w) {
  head(w);
  w_s(w, "<h2>Done</h2><p>The settings were tested and saved. You can close this page and switch your phone back to its "
         "normal Wi-Fi. The setup network will switch off in a moment.</p>");
  foot(w);
}
static void page_msg(wr_t *w, const char *title, const char *text) {
  head(w);
  w_s(w, "<h2>");
  w_esc(w, title);
  w_s(w, "</h2><p>");
  w_esc(w, text);
  w_s(w, "</p>");
  foot(w);
}

/* ---------------- lifecycle ---------------- */
void ru_portal_init(ru_portal_t *p, const ru_portal_cfg_t *cfg, const ru_portal_ops_t *ops, ru_kv_t *kv) {
  memset(p, 0, sizeof *p);
  if (cfg) p->cfg = *cfg;
  if (ops) p->ops = *ops;
  p->kv = kv;
  if (!p->cfg.idle_ms) p->cfg.idle_ms = 600000;
  if (!p->cfg.client_idle_ms) p->cfg.client_idle_ms = 120000;
  if (!p->cfg.done_grace_ms) p->cfg.done_grace_ms = 15000;
  if (!p->cfg.join_timeout_ms) p->cfg.join_timeout_ms = 20000;
  if (!p->cfg.test_timeout_ms) p->cfg.test_timeout_ms = RU_CFGLINE_TIMEOUT_MS;
  if (!p->cfg.max_failures) p->cfg.max_failures = 5;
  if (!p->cfg.pw_len) p->cfg.pw_len = RU_PW_MIN;
  if (!p->cfg.ap_ip) p->cfg.ap_ip = "192.168.4.1";
}

bool ru_portal_running(const ru_portal_t *p) { return p->step != RU_PORTAL_OFF; }

bool ru_portal_start(ru_portal_t *p, ru_portal_src_t src, uint32_t now_ms) {
  if (p->step != RU_PORTAL_OFF || src == RU_PORTAL_SRC_NONE || !p->ops.random || !p->kv) return false;
  if (p->cfg.pw_len < RU_PW_MIN || p->cfg.pw_len > RU_PW_MAX) return false;
  char url[RU_PORTAL_URL_MAX], ssid[33], pw[RU_PW_MAX + 1], csrf[sizeof p->csrf];
  if (!ru_portal_url(p->cfg.ap_ip, url, sizeof url)) return false;
  if (!ru_gen_ap_ssid(p->ops.random, p->ops.user, ssid, sizeof ssid)) return false;
  if (!ru_gen_password(p->ops.random, p->ops.user, p->cfg.pw_len, pw, sizeof pw)) return false;
  if (!ru_gen_hex(p->ops.random, p->ops.user, RU_PORTAL_CSRF_BYTES, csrf, sizeof csrf)) { ru_secure_zero(pw, sizeof pw); return false; }
  memcpy(p->url, url, sizeof p->url);
  memcpy(p->ap_ssid, ssid, sizeof p->ap_ssid);
  memcpy(p->ap_pass, pw, sizeof p->ap_pass);
  memcpy(p->csrf, csrf, sizeof p->csrf);
  ru_secure_zero(pw, sizeof pw);
  p->joined_ssid[0] = 0;
  p->failures = 0;
  p->locked = false;
  p->owner = 0;
  p->owner_seen_ms = 0;
  p->last_stop = RU_PORTAL_STOP_NONE;
  p->started_ms = p->last_activity_ms = now_ms;
  p->step = RU_PORTAL_STEP_WIFI;
  log_(p, "portal: started");
  return true;
}

bool ru_portal_start_from_wifi(ru_portal_t *p, const ru_wifi_sm_t *sm, uint32_t now_ms) {
  if (!sm || sm->state != RU_WIFI_AP_REQUESTED) return false;
  return ru_portal_start(p, RU_PORTAL_SRC_WIFI_SM, now_ms);
}

void ru_portal_stop(ru_portal_t *p, ru_portal_stop_t why) {
  if (p->step == RU_PORTAL_OFF) return;
  p->step = RU_PORTAL_OFF;
  p->last_stop = why;
  ru_secure_zero(p->ap_pass, sizeof p->ap_pass);
  ru_secure_zero(p->csrf, sizeof p->csrf);
  p->owner = 0;
  log_(p, why == RU_PORTAL_STOP_SUCCESS ? "portal: stopped (done)" : why == RU_PORTAL_STOP_IDLE ? "portal: stopped (idle)" : "portal: stopped");
}

bool ru_portal_tick(ru_portal_t *p, uint32_t now_ms) {
  if (p->step == RU_PORTAL_OFF) return false;
  if (p->step == RU_PORTAL_DONE) {
    if ((uint32_t)(now_ms - p->done_ms) >= p->cfg.done_grace_ms) ru_portal_stop(p, RU_PORTAL_STOP_SUCCESS);
  } else if ((uint32_t)(now_ms - p->last_activity_ms) >= p->cfg.idle_ms) {
    ru_portal_stop(p, RU_PORTAL_STOP_IDLE);
  }
  return p->step != RU_PORTAL_OFF;
}

void ru_portal_status(const ru_portal_t *p, uint32_t now_ms, ru_portal_status_t *o) {
  memset(o, 0, sizeof *o);
  o->running = p->step != RU_PORTAL_OFF;
  o->step = p->step;
  o->locked = p->locked;
  o->last_stop = p->last_stop;
  if (!o->running) return;
  memcpy(o->ssid, p->ap_ssid, sizeof o->ssid);
  memcpy(o->password, p->ap_pass, sizeof o->password);
  memcpy(o->url, p->url, sizeof o->url);
  uint32_t el = (uint32_t)(now_ms - p->last_activity_ms);
  o->remaining_ms = el >= p->cfg.idle_ms ? 0 : p->cfg.idle_ms - el;
  o->remaining_min = (o->remaining_ms + 59999u) / 60000u;
}

bool ru_portal_qr_wifi(const ru_portal_t *p, char *out, size_t cap) {
  if (p->step == RU_PORTAL_OFF) { if (out && cap) out[0] = 0; return false; }
  return ru_qr_wifi_payload(p->ap_ssid, p->ap_pass, out, cap);
}
bool ru_portal_qr_url(const ru_portal_t *p, char *out, size_t cap) {
  if (!out || !cap) return false;
  out[0] = 0;
  if (p->step == RU_PORTAL_OFF || strlen(p->url) + 1 > cap) return false;
  memcpy(out, p->url, strlen(p->url) + 1);
  return true;
}

/* ---------------- request handling ---------------- */
static void set_loc(ru_portal_resp_t *r, const char *url) { snprintf(r->location, sizeof r->location, "%s", url); }
static void redirect(const ru_portal_t *p, ru_portal_resp_t *r, int status) {
  r->status = status;
  r->html = false;
  r->body_len = 0;
  if (r->body_cap) r->body[0] = 0;
  set_loc(r, p->url);
}
static void finish(ru_portal_resp_t *r, wr_t *w, int status) {
  if (!w->ok) { /* page did not fit: never send a truncated page */
    wr_t w2;
    w_init(&w2, r->body, r->body_cap);
    w_s(&w2, "<!doctype html><title>dzPanel</title><p>Internal error.");
    r->status = 500;
    r->body_len = w2.ok ? w2.n : 0;
    r->html = w2.ok;
    return;
  }
  r->status = status;
  r->html = true;
  r->body_len = w->n;
}
static void send_msg(ru_portal_resp_t *r, int status, const char *title, const char *text) {
  wr_t w;
  w_init(&w, r->body, r->body_cap);
  page_msg(&w, title, text);
  finish(r, &w, status);
}
static void send_wifi(ru_portal_t *p, ru_portal_resp_t *r, err_t e) {
  wr_t w;
  w_init(&w, r->body, r->body_cap);
  page_wifi(&w, p, e);
  finish(r, &w, 200);
}
static void send_dz(ru_portal_t *p, ru_portal_resp_t *r, err_t e, const char *url) {
  wr_t w;
  w_init(&w, r->body, r->body_cap);
  page_domoticz(&w, p, e, url);
  finish(r, &w, 200);
}
static void send_done(ru_portal_resp_t *r) {
  wr_t w;
  w_init(&w, r->body, r->body_cap);
  page_done(&w);
  finish(r, &w, 200);
}

static void fail(ru_portal_t *p) {
  if (++p->failures >= p->cfg.max_failures && !p->locked) {
    p->locked = true;
    log_(p, "portal: locked after repeated failures");
  }
}
static void send_locked(ru_portal_resp_t *r) {
  send_msg(r, 429, "Too many attempts", "Setup is locked after too many failed attempts. Switch the setup mode off and on again on the device.");
}

/* Common checks for a form post. Returns an error or E_NONE. */
static err_t post_precheck(ru_portal_t *p, const ru_portal_req_t *rq) {
  if (!rq->form_ct || !rq->body || rq->body_len > RU_HTTP_BODY_MAX) return E_INPUT;
  if (!ru_form_check(rq->body, rq->body_len)) return E_INPUT;
  char tok[sizeof p->csrf + 8];
  ru_form_err_t fe = ru_form_get(rq->body, rq->body_len, "csrf", tok, sizeof tok);
  bool good = fe == RU_FORM_OK && ru_ct_streq(tok, p->csrf);
  ru_secure_zero(tok, sizeof tok);
  if (!good) { log_(p, "portal: post rejected (csrf)"); return E_CSRF; }
  return E_NONE;
}

static bool ssid_ok(const char *s) {
  size_t n = strlen(s);
  if (n < 1 || n > 32) return false;
  for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) return false;
  return true;
}

static void post_wifi(ru_portal_t *p, const ru_portal_req_t *rq, ru_portal_resp_t *r) {
  err_t e = post_precheck(p, rq);
  if (e != E_NONE) { fail(p); if (p->locked) send_locked(r); else if (e == E_CSRF) send_msg(r, 403, "Expired", err_text(E_CSRF)); else send_wifi(p, r, e); return; }
  char ssid[40], manual[40], pass[80];
  ru_form_err_t a = ru_form_get(rq->body, rq->body_len, "ssid_manual", manual, sizeof manual);
  ru_form_err_t b = RU_FORM_NOT_FOUND;
  if (a == RU_FORM_TOO_LONG) e = E_INPUT;
  else if (a == RU_FORM_OK && manual[0]) memcpy(ssid, manual, sizeof ssid);
  else {
    b = ru_form_get(rq->body, rq->body_len, "ssid", ssid, sizeof ssid);
    if (b != RU_FORM_OK) e = E_INPUT;
  }
  ru_form_err_t c = ru_form_get(rq->body, rq->body_len, "pass", pass, sizeof pass);
  if (c == RU_FORM_NOT_FOUND) pass[0] = 0;
  else if (c != RU_FORM_OK) e = E_INPUT;
  if (e == E_NONE && (!ssid_ok(ssid) || !ru_wifi_ssid_check(ssid) || !ru_wifi_pass_check(pass))) e = E_INPUT; /* same rules as the USB path */
  if (e == E_NONE) {
    if (!p->ops.wifi_join || !p->ops.wifi_join(p->ops.user, ssid, pass, p->cfg.join_timeout_ms))
      e = p->ops.wifi_busy && p->ops.wifi_busy(p->ops.user) ? E_BUSY : E_JOIN;
  }
  ru_secure_zero(pass, sizeof pass);
  if (p->step == RU_PORTAL_OFF) { /* stopped while the (slow) join ran */
    ru_secure_zero(ssid, sizeof ssid);
    r->status = 503;
    r->html = false;
    return;
  }
  if (e != E_NONE) {
    ru_secure_zero(ssid, sizeof ssid);
    if (e != E_BUSY) fail(p); /* busy is not the user's mistake: no lockout strike */
    if (p->locked) send_locked(r); else send_wifi(p, r, e);
    return;
  }
  memcpy(p->joined_ssid, ssid, strlen(ssid) + 1); /* ssid_ok: 1..32 bytes */
  p->step = RU_PORTAL_STEP_DOMOTICZ;
  log_(p, "portal: wifi joined");
  redirect(p, r, 303);
}

static bool json_put(wr_t *w, const char *s) { /* only " and \ need escaping: inputs were validated free of control chars */
  for (; *s; s++) {
    if ((unsigned char)*s < 0x20) return false;
    if (*s == '"' || *s == '\\') w_c(w, '\\');
    w_c(w, *s);
  }
  return w->ok;
}

static err_t map_reply(const char *reply) {
  if (!strncmp(reply, "RU+CFG OK", 9)) return E_NONE;
  static const struct { const char *code; err_t e; } m[] = {
      {"bad_json", E_BAD_TOKEN}, {"bad_version", E_VERSION}, {"bad_url", E_BAD_URL}, {"too_long", E_TOO_LONG},
      {"unreachable not_domoticz", E_NOT_DOMOTICZ}, {"unreachable", E_UNREACHABLE}, {"tls", E_TLS},
      {"token_rejected", E_REJECTED}, {"no_wifi", E_NO_WIFI}, {"store", E_STORE}};
  if (strncmp(reply, "RU+CFG ERR ", 11)) return E_STORE;
  for (size_t i = 0; i < sizeof m / sizeof *m; i++)
    if (!strncmp(reply + 11, m[i].code, strlen(m[i].code))) return m[i].e;
  return E_STORE;
}

static void post_domoticz(ru_portal_t *p, const ru_portal_req_t *rq, ru_portal_resp_t *r) {
  err_t e = post_precheck(p, rq);
  if (e != E_NONE) { fail(p); if (p->locked) send_locked(r); else if (e == E_CSRF) send_msg(r, 403, "Expired", err_text(E_CSRF)); else send_dz(p, r, e, NULL); return; }
  char urlraw[RU_CFG_URL_MAX + 8], urln[RU_CFG_URL_MAX], ack[8];
  char token[RU_CFG_TOKEN_MAX + 8];
  char json[RU_CFGLINE_MAX];
  char reply[RU_CFGLINE_REPLY_MAX];
  bool https = false;
  urlraw[0] = 0;
  if (ru_form_get(rq->body, rq->body_len, "url", urlraw, sizeof urlraw) != RU_FORM_OK) e = E_BAD_URL;
  ru_form_err_t tf = ru_form_get(rq->body, rq->body_len, "token", token, sizeof token);
  if (e == E_NONE && tf != RU_FORM_OK) e = tf == RU_FORM_TOO_LONG ? E_TOO_LONG : E_BAD_TOKEN;
  if (ru_form_get(rq->body, rq->body_len, "ack", ack, sizeof ack) != RU_FORM_OK) ack[0] = 0;
  bool acked = !strcmp(ack, "1");
  if (e == E_NONE) {
    ru_cfg_err_t ce = ru_cfg_normalise_url(urlraw, urln, sizeof urln, &https);
    if (ce == RU_CFG_TOO_LONG) e = E_TOO_LONG;
    else if (ce != RU_CFG_OK) e = E_BAD_URL;
  }
  if (e == E_NONE) {
    ru_cfg_err_t te = ru_cfg_check_token(token, NULL);
    if (te == RU_CFG_TOO_LONG) e = E_TOO_LONG;
    else if (te != RU_CFG_OK) e = E_BAD_TOKEN;
  }
  if (e == E_NONE && !https && !acked) e = E_NEEDS_ACK;
  if (e == E_NONE) {
    wr_t w;
    w_init(&w, json, sizeof json);
    w_s(&w, "{\"v\":1,\"domoticz\":{\"url\":\"");
    bool ok = json_put(&w, urln);
    w_s(&w, "\",\"token\":\"");
    ok = ok && json_put(&w, token);
    w_s(&w, https ? "\",\"insecure_lan\":false}}" : "\",\"insecure_lan\":true}}");
    if (!ok || !w.ok) e = E_TOO_LONG;
    else {
      /* the single shared path: validation, connection test, save. A reply never contains the line. */
      ru_cfgline_ops_t ops = p->ops.cfg;
      (void)ru_cfgline_handle(p->kv, &ops, json, w.n, reply, sizeof reply);
      e = map_reply(reply);
    }
  }
  ru_secure_zero(token, sizeof token);
  ru_secure_zero(json, sizeof json);
  if (e != E_NONE) {
    fail(p);
    if (p->locked) send_locked(r);
    else send_dz(p, r, e, e == E_BAD_URL ? NULL : urlraw);
    return;
  }
  p->step = RU_PORTAL_DONE;
  log_(p, "portal: configuration saved");
  redirect(p, r, 303);
}

bool ru_portal_handle(ru_portal_t *p, const ru_portal_req_t *rq, uint32_t now_ms, ru_portal_resp_t *r) {
  r->status = 500;
  r->html = false;
  r->body_len = 0;
  r->location[0] = 0;
  if (r->body_cap) r->body[0] = 0;
  if (!rq || !rq->path || !r->body || r->body_cap == 0) return false;
  ru_portal_tick(p, now_ms);
  if (p->step == RU_PORTAL_OFF) { r->status = 503; return false; }

  ru_probe_t kind = ru_captive_classify(rq->path);
  bool is_wifi = !strcmp(rq->path, "/wifi"), is_dz = !strcmp(rq->path, "/domoticz");
  if (kind != RU_PROBE_ROOT && !is_wifi && !is_dz) { /* probes and everything else: send them to the portal root */
    redirect(p, r, 302);
    return true;
  }
  if (p->locked) { send_locked(r); return true; }
  /* one client at a time */
  uint32_t me = rq->client ? rq->client : 0xFFFFFFFFu;
  if (p->owner && p->owner != me && (uint32_t)(now_ms - p->owner_seen_ms) < p->cfg.client_idle_ms) {
    send_msg(r, 503, "Busy", "Another device is setting this up right now. Try again in a few minutes.");
    return true;
  }
  p->owner = me;
  p->owner_seen_ms = now_ms;
  p->last_activity_ms = now_ms;

  if (kind == RU_PROBE_ROOT) {
    if (rq->method != RU_HTTP_GET) { send_msg(r, 405, "Not allowed", "Use the form on the first page."); return true; }
    if (p->step == RU_PORTAL_DONE) send_done(r);
    else if (p->step == RU_PORTAL_STEP_DOMOTICZ) send_dz(p, r, E_NONE, NULL);
    else send_wifi(p, r, E_NONE);
    return true;
  }
  if (p->step == RU_PORTAL_DONE) { send_done(r); return true; }
  if (is_wifi) {
    if (rq->method == RU_HTTP_GET) { p->step = RU_PORTAL_STEP_WIFI; send_wifi(p, r, E_NONE); }
    else post_wifi(p, rq, r);
    return true;
  }
  /* /domoticz */
  if (rq->method != RU_HTTP_POST) { redirect(p, r, 302); return true; }
  if (p->step != RU_PORTAL_STEP_DOMOTICZ) { redirect(p, r, 303); return true; } /* not joined yet: back to step 1 */
  post_domoticz(p, rq, r);
  if (p->step == RU_PORTAL_DONE) p->done_ms = now_ms;
  return true;
}
