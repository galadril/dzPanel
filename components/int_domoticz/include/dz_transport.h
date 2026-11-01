/* HTTP transport interface for the Domoticz client. One blocking GET with a hard timeout and a
 * bounded response buffer. Implementations must never log, print or persist header values: the
 * Authorization header carries the access token. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
  DZ_NET_OK = 0,
  DZ_NET_TIMEOUT,    /* no (complete) answer within timeout_ms */
  DZ_NET_CONNECT,    /* DNS, refused, reset, malformed HTTP, no route */
  DZ_NET_TLS,        /* TLS failure, or https requested from a transport that cannot do it */
  DZ_NET_TOO_LARGE   /* body did not fit the buffer; status is still valid, body is truncated */
} dz_net_err_t;

typedef struct {
  int status;   /* HTTP status, 0 if none was received */
  size_t len;   /* body bytes stored in buf (buf is always NUL terminated) */
} dz_http_result_t;

typedef struct dz_transport dz_transport_t;
struct dz_transport {
  /* headers: NULL terminated array of "Name: value" strings (may be NULL).
   * buf/cap: cap >= 1. Always NUL terminates, even on error. */
  dz_net_err_t (*get)(dz_transport_t *t, const char *url, const char *const *headers, uint32_t timeout_ms, char *buf,
                      size_t cap, dz_http_result_t *out);
  void (*destroy)(dz_transport_t *t);
};

const char *dz_net_err_name(dz_net_err_t e);

/* Plain-socket HTTP/1.1 (http:// only, Connection: close, Content-Length and chunked). For the mock. */
dz_transport_t *dz_transport_socket_create(void);

/* Dev-only live transport: runs the `curl` CLI. Headers and URL go to curl through a config on
 * stdin, so the token never appears on a command line or in /proc. Host only. */
dz_transport_t *dz_transport_curl_create(void);

void dz_transport_destroy(dz_transport_t *t);
