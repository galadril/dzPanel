#include "ru_portal_esp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "ru_osal.h"
#include "ru_wifi_esp.h"

static const char *TAG = "ru_portal";
/* NEVER log: the AP password, the Wi-Fi password, the token, form bodies, SSIDs typed by the user. Only fixed strings. */

#define EV_DNS_DONE BIT0
#define EV_SUP_DONE BIT1

static struct {
  bool inited, active;
  ru_portal_esp_cfg_t cfg;
  ru_portal_t P;
  ru_mutex_t *mu;      /* protects P (released around the slow Wi-Fi join) */
  ru_mutex_t *snap_mu; /* protects snap */
  ru_mutex_t *ctl;     /* serialises start/stop */
  ru_portal_status_t snap;
  EventGroupHandle_t evt;
  httpd_handle_t httpd;
  esp_netif_t *ap;
  volatile bool dns_stop, quit, join_busy;
  TaskHandle_t sup;
  uint8_t ip[4];
  ru_portal_net_t nets[RU_PORTAL_MAX_NETS];
  size_t nnets;
} S;

/* ---------------- pure-layer ops ---------------- */
static bool op_random(void *u, uint8_t *b, size_t n) { (void)u; esp_fill_random(b, n); return true; }
static size_t op_scan(void *u, ru_portal_net_t *out, size_t max) {
  (void)u;
  size_t n = S.nnets < max ? S.nnets : max;
  memcpy(out, S.nets, n * sizeof *out);
  return n;
}
/* Called inside ru_portal_handle with S.mu held: release it while waiting so status/stop are not blocked for seconds. */
static bool op_join(void *u, const char *ssid, const char *pass, uint32_t timeout_ms) {
  (void)u;
  ru_mutex_unlock(S.mu);
  /* apply_wait reports the exact outcome: a wrong password ends in FAILED and the manager then reverts to the OLD network,
   * whose CONNECTED must not be read as success of the new one. Busy = another attempt (USB, Improv) is pending. */
  ru_wifi_apply_t r = ru_wifi_esp_apply_wait(ssid, pass, timeout_ms);
  S.join_busy = r == RU_WIFI_APPLY_BUSY;
  ru_mutex_lock(S.mu);
  return r == RU_WIFI_APPLY_OK;
}
static bool op_busy(void *u) { (void)u; return S.join_busy; }
static void op_log(void *u, const char *m) { (void)u; ESP_LOGI(TAG, "%s", m); }

static void snap_refresh_locked(void) { /* S.mu held */
  ru_portal_status_t t;
  ru_portal_status(&S.P, ru_now_ms(), &t);
  ru_mutex_lock(S.snap_mu);
  S.snap = t;
  ru_mutex_unlock(S.snap_mu);
  ru_secure_zero(&t, sizeof t);
}

/* ---------------- http ---------------- */
static const char *status_line(int code) {
  switch (code) {
    case 200: return "200 OK";
    case 302: return "302 Found";
    case 303: return "303 See Other";
    case 400: return "400 Bad Request";
    case 403: return "403 Forbidden";
    case 405: return "405 Method Not Allowed";
    case 408: return "408 Request Timeout";
    case 413: return "413 Content Too Large";
    case 415: return "415 Unsupported Media Type";
    case 429: return "429 Too Many Requests";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}
static void common_headers(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", RU_PORTAL_CACHE);
  httpd_resp_set_hdr(req, "Content-Security-Policy", RU_PORTAL_CSP);
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
  httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
  httpd_resp_set_hdr(req, "Connection", "close"); /* one request per connection */
}
static esp_err_t send_plain(httpd_req_t *req, int code) {
  httpd_resp_set_status(req, status_line(code));
  common_headers(req);
  return httpd_resp_send(req, NULL, 0);
}
static uint32_t peer_id(httpd_req_t *req) {
  struct sockaddr_storage ss;
  socklen_t sl = sizeof ss;
  int fd = httpd_req_to_sockfd(req);
  out[0] = 0;
  if (fd < 0 || getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) return;
  if (ss.ss_family == AF_INET) ru_portal_local_text(4, (const uint8_t *)&((struct sockaddr_in *)&ss)->sin_addr.s_addr, out, cap);
  else if (ss.ss_family == AF_INET6) ru_portal_local_text(6, ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr, out, cap);
}

/* The portal must answer only on the AP address and only under that name (ru_portal_guard, tested on the host). */
static bool guard_ok(httpd_req_t *req, esp_err_t *rc) {
  char local[24], host[48];
  size_t hl = httpd_req_get_hdr_value_len(req, "Host");
  host[0] = 0;
  if (hl > 0 && hl < sizeof host && httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) != ESP_OK) host[0] = 0;
  const char *h = ru_portal_host_arg(hl, sizeof host, host);
  req_local_ip(req, local, sizeof local);
  switch (ru_portal_guard(S.P.cfg.ap_ip, local, h, req->method == HTTP_POST)) {
    case RU_GUARD_ALLOW: return true;
    case RU_GUARD_REDIRECT:
      httpd_resp_set_status(req, status_line(302));
      common_headers(req);
      httpd_resp_set_hdr(req, "Location", S.P.url);
      *rc = httpd_resp_send(req, NULL, 0);
      return false;
    case RU_GUARD_BAD_REQUEST: *rc = send_plain(req, 400); return false;
    default: *rc = send_plain(req, 403); return false;
  }
}

static esp_err_t h_any(httpd_req_t *req) {
  esp_err_t grc = ESP_OK;
  if (!guard_ok(req, &grc)) return grc;
  if (req->method != HTTP_GET && req->method != HTTP_POST) return send_plain(req, 405);
  if (req->content_len > RU_HTTP_BODY_MAX) return send_plain(req, 413);
  if (req->method == HTTP_GET && req->content_len != 0) return send_plain(req, 400);
  char path[RU_HTTP_PATH_MAX];
  size_t pl = 0;
  while (req->uri[pl] && req->uri[pl] != '?' && req->uri[pl] != '#') pl++;
  if (pl >= sizeof path) { memcpy(path, "/x", 3); }
  else { memcpy(path, req->uri, pl); path[pl] = 0; }
  char *body = NULL;
  size_t blen = req->content_len;
  if (req->method == HTTP_POST) {
    body = malloc(RU_HTTP_BODY_MAX + 1);
    if (!body) return send_plain(req, 503);
    uint32_t t0 = ru_now_ms();
    size_t got = 0;
    while (got < blen) {
      int r = httpd_req_recv(req, body + got, blen - got);
      if (r == HTTPD_SOCK_ERR_TIMEOUT) {
        if ((uint32_t)(ru_now_ms() - t0) > BODY_DEADLINE_MS) { ru_secure_zero(body, RU_HTTP_BODY_MAX + 1); free(body); return send_plain(req, 408); }
        continue;
      }
      if (r <= 0 || (uint32_t)(ru_now_ms() - t0) > BODY_DEADLINE_MS) { ru_secure_zero(body, RU_HTTP_BODY_MAX + 1); free(body); return send_plain(req, 408); }
      got += (size_t)r;
    }
    body[blen] = 0;
  }
  char *page = malloc(RU_PORTAL_PAGE_MAX);
  if (!page) { if (body) { ru_secure_zero(body, RU_HTTP_BODY_MAX + 1); free(body); } return send_plain(req, 503); }
  ru_portal_req_t rq = {req->method == HTTP_POST ? RU_HTTP_POST : RU_HTTP_GET, path, body, blen,
                        req->method == HTTP_POST && is_form_ct(req), peer_id(req)};
  ru_portal_resp_t rs;
  memset(&rs, 0, sizeof rs);
  rs.body = page;
  rs.body_cap = RU_PORTAL_PAGE_MAX;
  ru_mutex_lock(S.mu);
  bool ran = ru_portal_handle(&S.P, &rq, ru_now_ms(), &rs);
  snap_refresh_locked();
  ru_mutex_unlock(S.mu);
  if (body) { ru_secure_zero(body, RU_HTTP_BODY_MAX + 1); free(body); }
  if (!ran && rs.status == 500) rs.status = 503;
  httpd_resp_set_status(req, status_line(rs.status));
  common_headers(req);
  esp_err_t e;
  if (rs.status == 302 || rs.status == 303) httpd_resp_set_hdr(req, "Location", rs.location);
  if (rs.html) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    e = httpd_resp_send(req, page, (ssize_t)rs.body_len);
  } else {
    e = httpd_resp_send(req, NULL, 0);
  }
  ru_secure_zero(page, RU_PORTAL_PAGE_MAX);
  free(page);
  return e;
}

static bool http_up(void) {
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.max_uri_handlers = 2;
  c.max_open_sockets = 4;
  c.stack_size = 8192;
  c.lru_purge_enable = true;
  c.recv_wait_timeout = 5;
  c.send_wait_timeout = 5;
  c.uri_match_fn = httpd_uri_match_wildcard;
  if (httpd_start(&S.httpd, &c) != ESP_OK) { S.httpd = NULL; return false; }
  httpd_uri_t g = {.uri = "/*", .method = HTTP_GET, .handler = h_any, .user_ctx = NULL};
  httpd_uri_t p = {.uri = "/*", .method = HTTP_POST, .handler = h_any, .user_ctx = NULL};
  if (httpd_register_uri_handler(S.httpd, &g) != ESP_OK || httpd_register_uri_handler(S.httpd, &p) != ESP_OK) {
    httpd_stop(S.httpd);
    S.httpd = NULL;
    return false;
  }
  return true;
}

/* ---------------- DNS responder ---------------- */
static void dns_task(void *arg) {
  (void)arg;
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  uint8_t *q = malloc(512), *o = malloc(512);
  if (s >= 0 && q && o) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    memcpy(&a.sin_addr.s_addr, S.ip, 4); /* the AP address only: never the home-network interface */
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (bind(s, (struct sockaddr *)&a, sizeof a) == 0) {
      while (!S.dns_stop) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, q, 512, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue; /* timeout: re-check the stop flag */
        size_t m = ru_dns_answer(q, (size_t)n, S.ip, o, 512);
        if (m) sendto(s, o, m, 0, (struct sockaddr *)&from, fl);
      }
    } else {
      ESP_LOGW(TAG, "dns bind failed");
    }
  }
  if (s >= 0) close(s);
  free(q);
  free(o);
  xEventGroupSetBits(S.evt, EV_DNS_DONE);
  vTaskDelete(NULL);
}

/* ---------------- AP ---------------- */
static bool ap_up(const ru_portal_status_t *st) {
  S.ap = esp_netif_create_default_wifi_ap();
  if (!S.ap) return false;
  esp_netif_ip_info_t ip;
  memset(&ip, 0, sizeof ip);
  if (esp_netif_str_to_ip4(S.P.cfg.ap_ip, &ip.ip) != ESP_OK) return false;
  IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
  ip.gw = ip.ip;
  memcpy(S.ip, &ip.ip.addr, 4); /* network order bytes */
  esp_netif_dhcps_stop(S.ap);
  esp_netif_set_ip_info(S.ap, &ip);
  esp_netif_dns_info_t dns;
  memset(&dns, 0, sizeof dns);
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4 = ip.ip;
  esp_netif_set_dns_info(S.ap, ESP_NETIF_DNS_MAIN, &dns); /* phones ask us, and we answer everything with ourselves */
  uint8_t offer = 1;
  esp_netif_dhcps_option(S.ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof offer);
  if (esp_netif_dhcps_start(S.ap) != ESP_OK) return false;
  wifi_config_t ac;
  memset(&ac, 0, sizeof ac);
  size_t sl = strnlen(st->ssid, sizeof ac.ap.ssid);
  memcpy(ac.ap.ssid, st->ssid, sl);
  ac.ap.ssid_len = (uint8_t)sl;
  size_t pl = strnlen(st->password, sizeof st->password); /* <= 32 chars: fits the 64-byte AP field */
  memcpy(ac.ap.password, st->password, pl);
  ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ac.ap.max_connection = 2; /* the pure layer admits one client session; a second slot only eases a phone's reconnect */
  ac.ap.channel = 1;        /* follows the STA channel while STA is connected (hardware limit, see docs/progress/portal.md) */
  ac.ap.pmf_cfg.required = false;
  esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &ac);
  ru_secure_zero(&ac, sizeof ac);
  return e == ESP_OK;
}

static void net_down(void) {
  if (S.httpd) { httpd_stop(S.httpd); S.httpd = NULL; }
  S.dns_stop = true;
  xEventGroupWaitBits(S.evt, EV_DNS_DONE, pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));
  if (S.ap) {
    esp_wifi_set_mode(WIFI_MODE_STA); /* back to station only: the AP interface and its DHCP server go away */
    esp_netif_destroy_default_wifi(S.ap);
    S.ap = NULL;
  }
}

static void sup_task(void *arg) {
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    ru_mutex_lock(S.mu);
    bool run = ru_portal_tick(&S.P, ru_now_ms());
    snap_refresh_locked();
    ru_mutex_unlock(S.mu);
    if (!run || S.quit) break;
  }
  net_down();
  ru_mutex_lock(S.mu);
  ru_portal_stop(&S.P, RU_PORTAL_STOP_USER); /* no-op when it already stopped with a reason */
  ru_portal_stop_t why = S.P.last_stop;
  snap_refresh_locked();
  ru_mutex_unlock(S.mu);
  S.active = false;
  S.sup = NULL;
  if (S.cfg.on_stopped) S.cfg.on_stopped(S.cfg.user, why);
  xEventGroupSetBits(S.evt, EV_SUP_DONE);
  vTaskDelete(NULL);
}

/* ---------------- public API ---------------- */
bool ru_portal_esp_init(const ru_portal_esp_cfg_t *cfg) {
  if (S.inited || !cfg || !cfg->kv) return false;
  memset(&S, 0, sizeof S);
  S.cfg = *cfg;
  S.mu = ru_mutex_create();
  S.snap_mu = ru_mutex_create();
  S.ctl = ru_mutex_create();
  S.evt = xEventGroupCreate();
  if (!S.mu || !S.snap_mu || !S.ctl || !S.evt) return false;
  ru_portal_ops_t ops;
  memset(&ops, 0, sizeof ops);
  ops.random = op_random;
  ops.scan = op_scan;
  ops.wifi_join = op_join;
  ops.wifi_busy = op_busy;
  ops.cfg = cfg->test;
  ops.log = op_log;
  ru_portal_init(&S.P, &cfg->cfg, &ops, cfg->kv);
  S.inited = true;
  return true;
}

bool ru_portal_esp_start(ru_portal_src_t src) {
  if (!S.inited) return false;
  ru_mutex_lock(S.ctl);
  if (S.active) { ru_mutex_unlock(S.ctl); return false; }
  /* scan first, while STA owns the radio: the AP's channel then never hops under a connected phone */
  ru_improv_net_t raw[RU_PORTAL_MAX_NETS];
  size_t n = ru_wifi_esp_scan(raw, RU_PORTAL_MAX_NETS);
  S.nnets = 0;
  for (size_t i = 0; i < n && i < RU_PORTAL_MAX_NETS; i++) {
    snprintf(S.nets[i].ssid, sizeof S.nets[i].ssid, "%s", raw[i].ssid);
    S.nets[i].rssi = (int8_t)raw[i].rssi;
    S.nets[i].secured = raw[i].secured;
    S.nnets++;
  }
  ru_mutex_lock(S.mu);
  bool ok = ru_portal_start(&S.P, src, ru_now_ms());
  ru_portal_status_t st;
  if (ok) snap_refresh_locked();
  ru_portal_status(&S.P, ru_now_ms(), &st);
  ru_mutex_unlock(S.mu);
  if (!ok) { ru_mutex_unlock(S.ctl); return false; }
  S.active = true;
  S.quit = false;
  S.dns_stop = false;
  xEventGroupClearBits(S.evt, EV_SUP_DONE);
  xEventGroupSetBits(S.evt, EV_DNS_DONE); /* "no DNS task running" until one is created */
  bool up = ap_up(&st);
  ru_secure_zero(&st, sizeof st);
  if (up) up = http_up();
  if (up) {
    xEventGroupClearBits(S.evt, EV_DNS_DONE);
    up = xTaskCreate(dns_task, "ru_dns", 3072, NULL, 4, NULL) == pdPASS;
    if (!up) xEventGroupSetBits(S.evt, EV_DNS_DONE);
  }
  if (up) up = xTaskCreate(sup_task, "ru_portal", 4096, NULL, 4, &S.sup) == pdPASS;
  if (!up) {
    ESP_LOGW(TAG, "start failed, rolling back");
    net_down();
    ru_mutex_lock(S.mu);
    ru_portal_stop(&S.P, RU_PORTAL_STOP_USER);
    snap_refresh_locked();
    ru_mutex_unlock(S.mu);
    S.active = false;
  }
  ru_mutex_unlock(S.ctl);
  return up;
}

void ru_portal_esp_stop(void) {
  if (!S.inited) return;
  ru_mutex_lock(S.ctl);
  if (S.active && S.sup) {
    S.quit = true;
    ru_mutex_lock(S.mu);
    ru_portal_stop(&S.P, RU_PORTAL_STOP_USER);
    ru_mutex_unlock(S.mu);
    xTaskNotifyGive(S.sup);
    xEventGroupWaitBits(S.evt, EV_SUP_DONE, pdFALSE, pdTRUE, pdMS_TO_TICKS(8000));
  }
  ru_mutex_unlock(S.ctl);
}

bool ru_portal_esp_running(void) { return S.inited && S.active; }

bool ru_portal_esp_status(ru_portal_status_t *out) {
  if (!S.inited || !out) return false;
  ru_mutex_lock(S.snap_mu);
  *out = S.snap;
  ru_mutex_unlock(S.snap_mu);
  return out->running;
}
bool ru_portal_esp_qr_wifi(char *out, size_t cap) {
  ru_portal_status_t st;
  bool ok = ru_portal_esp_status(&st) && ru_qr_wifi_payload(st.ssid, st.password, out, cap);
  ru_secure_zero(&st, sizeof st);
  return ok;
}
bool ru_portal_esp_qr_url(char *out, size_t cap) {
  ru_portal_status_t st;
  bool ok = ru_portal_esp_status(&st) && strlen(st.url) + 1 <= cap;
  if (ok) memcpy(out, st.url, strlen(st.url) + 1);
  ru_secure_zero(&st, sizeof st);
  return ok;
}
