#include "dz_transport_esp.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"

static const char *TAG = "dz_http";

typedef struct {
  dz_transport_t base;
  volatile bool insecure_lan;
} esp_tr_t;

void dz_transport_esp_set_insecure_lan(dz_transport_t *t, bool allow) {
  if (t) ((esp_tr_t *)t)->insecure_lan = allow;
}

static bool header_ok(const char *h) {
  if (!h || !strchr(h, ':')) return false;
  for (const char *p = h; *p; p++)
    if (*p == '\r' || *p == '\n') return false; /* header injection */
  return true;
}

/* Maps a failure of esp_http_client_open()/fetch_headers() to a transport error kind. */
static dz_net_err_t map_open_error(esp_http_client_handle_t c, esp_err_t err) {
  int tls_code = 0, tls_flags = 0;
  esp_err_t tls_err = esp_http_client_get_and_clear_last_tls_error(c, &tls_code, &tls_flags);
  if (err == ESP_ERR_HTTP_EAGAIN || tls_err == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT || tls_err == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT ||
      esp_http_client_get_errno(c) == 110 /* ETIMEDOUT */)
    return DZ_NET_TIMEOUT;
  if (tls_flags != 0 || tls_err == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED || tls_err == ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED ||
      tls_err == ESP_ERR_MBEDTLS_SSL_SETUP_FAILED || tls_err == ESP_ERR_MBEDTLS_SSL_CONF_OWN_CERT_FAILED ||
      tls_err == ESP_ERR_MBEDTLS_SSL_SET_HOSTNAME_FAILED || tls_err == ESP_ERR_MBEDTLS_SSL_READ_FAILED ||
      tls_err == ESP_ERR_MBEDTLS_SSL_WRITE_FAILED)
    return DZ_NET_TLS;
  (void)tls_code;
  return DZ_NET_CONNECT;
}

static dz_net_err_t esp_get(dz_transport_t *t, const char *url, const char *const *headers, uint32_t timeout_ms, char *buf,
                            size_t cap, dz_http_result_t *out) {
  esp_tr_t *self = (esp_tr_t *)t;
  out->status = 0;
  out->len = 0;
  if (!cap) return DZ_NET_CONNECT;
  buf[0] = 0;
  if (!url) return DZ_NET_CONNECT;
  bool https = !strncmp(url, "https://", 8);
  if (!https && !(self->insecure_lan && !strncmp(url, "http://", 7))) {
    ESP_LOGW(TAG, "plain http refused (insecure_lan not set)"); /* the URL is deliberately not logged */
    return DZ_NET_CONNECT;
  }
  for (size_t i = 0; url[i]; i++)
    if ((unsigned char)url[i] <= 0x20 || (unsigned char)url[i] >= 0x7f) return DZ_NET_CONNECT; /* request-line injection */
  for (size_t i = 0; headers && headers[i]; i++)
    if (!header_ok(headers[i])) return DZ_NET_CONNECT;

  esp_http_client_config_t cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.url = url;
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = (int)(timeout_ms ? timeout_ms : 8000);
  cfg.disable_auto_redirect = true; /* never follow: the token must not leave the configured host */
  cfg.keep_alive_enable = false;
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 2048; /* request line plus a JWT of up to ~800 bytes */
  if (https) cfg.crt_bundle_attach = esp_crt_bundle_attach; /* verification stays on: no skip_cert_common_name_check */
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return DZ_NET_CONNECT;

  dz_net_err_t result = DZ_NET_OK;
  int64_t deadline = esp_timer_get_time() + (int64_t)cfg.timeout_ms * 1000;
  esp_http_client_set_header(c, "Connection", "close");
  esp_http_client_set_header(c, "Accept", "application/json");
  for (size_t i = 0; headers && headers[i]; i++) {
    /* "Name: value": copy the name out, pass the value pointer through (esp_http_client copies it) */
    const char *colon = strchr(headers[i], ':');
    char name[40];
    size_t nl = (size_t)(colon - headers[i]);
    if (nl == 0 || nl >= sizeof name) { result = DZ_NET_CONNECT; goto done; }
    memcpy(name, headers[i], nl);
    name[nl] = 0;
    const char *v = colon + 1;
    while (*v == ' ') v++;
    esp_http_client_set_header(c, name, v);
  }
  esp_err_t e = esp_http_client_open(c, 0);
  if (e != ESP_OK) { result = map_open_error(c, e); goto done; }
  int64_t cl = esp_http_client_fetch_headers(c);
  if (cl < 0) { result = map_open_error(c, (esp_err_t)(-cl)); if (result == DZ_NET_CONNECT && cl == -ESP_ERR_HTTP_EAGAIN) result = DZ_NET_TIMEOUT; goto done; }
  out->status = esp_http_client_get_status_code(c);

  size_t len = 0;
  for (;;) {
    if (esp_timer_get_time() > deadline) { result = DZ_NET_TIMEOUT; break; }
    if (len + 1 >= cap) { /* buffer full: is there more? */
      char probe;
      int r = esp_http_client_read(c, &probe, 1);
      if (r > 0) result = DZ_NET_TOO_LARGE;
      else if (r < 0 && r != -ESP_ERR_HTTP_EAGAIN) result = DZ_NET_CONNECT;
      break;
    }
    int r = esp_http_client_read(c, buf + len, (int)(cap - 1 - len));
    if (r > 0) { len += (size_t)r; continue; }
    if (r == 0) {
      if (esp_http_client_is_complete_data_received(c)) break;
      if (esp_http_client_get_errno(c) == 110) { result = DZ_NET_TIMEOUT; break; }
      result = DZ_NET_CONNECT; /* connection closed before the body was complete */
      break;
    }
    if (r == -ESP_ERR_HTTP_EAGAIN) { result = DZ_NET_TIMEOUT; break; }
    result = DZ_NET_CONNECT;
    break;
  }
  buf[len] = 0;
  out->len = len;
  if (result == DZ_NET_TIMEOUT || result == DZ_NET_CONNECT) { /* contract: no body on a transport error */
    buf[0] = 0;
    out->len = 0;
  }
done:
  esp_http_client_cleanup(c);
  return result;
}

static void esp_destroy(dz_transport_t *t) { free(t); }

dz_transport_t *dz_transport_esp_create(bool insecure_lan) {
  esp_tr_t *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->base.get = esp_get;
  t->base.destroy = esp_destroy;
  t->insecure_lan = insecure_lan;
  esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN); /* header values must never reach a debug log */
  esp_log_level_set("esp-tls", ESP_LOG_WARN);
  esp_log_level_set("esp-tls-mbedtls", ESP_LOG_WARN);
  return &t->base;
}

/* dz_transport.c (the host socket and curl transports) cannot be compiled for the device: it needs poll.h, fork and
 * friends. These two tiny functions live there on the host, so the device build gets weak copies here (same behaviour);
 * if dz_transport.c is ever split, drop these. */
__attribute__((weak)) const char *dz_net_err_name(dz_net_err_t e) {
  switch (e) {
    case DZ_NET_OK: return "ok";
    case DZ_NET_TIMEOUT: return "timeout";
    case DZ_NET_CONNECT: return "connect";
    case DZ_NET_TLS: return "tls";
    case DZ_NET_TOO_LARGE: return "too-large";
  }
  return "?";
}
__attribute__((weak)) void dz_transport_destroy(dz_transport_t *t) {
  if (t && t->destroy) t->destroy(t);
}
