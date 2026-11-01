/* Scripted fake transport for client/command tests: no sockets, no clock. */
#pragma once
#include <stdlib.h>
#include <string.h>

#include "dz_transport.h"

typedef struct fake fake_t;
typedef dz_net_err_t (*fake_fn)(fake_t *f, const char *url, char *buf, size_t cap, dz_http_result_t *out);
struct fake {
  dz_transport_t base;
  fake_fn fn;
  void *user;
  int calls;
  char urls[32][512];  /* ring of the last urls */
  int had_auth;        /* last call carried an Authorization header */
  int auth_in_url;     /* any url contained the secret */
  const char *secret;
};

static inline dz_net_err_t fake_get(dz_transport_t *t, const char *url, const char *const *headers, uint32_t timeout_ms,
                                    char *buf, size_t cap, dz_http_result_t *out) {
  fake_t *f = (fake_t *)t;
  (void)timeout_ms;
  snprintf(f->urls[f->calls % 32], sizeof f->urls[0], "%s", url);
  f->calls++;
  f->had_auth = headers && headers[0] && !strncmp(headers[0], "Authorization: Bearer ", 22);
  if (f->secret && strstr(url, f->secret)) f->auth_in_url = 1;
  out->status = 0;
  out->len = 0;
  buf[0] = 0;
  return f->fn(f, url, buf, cap, out);
}
static inline void fake_destroy(dz_transport_t *t) { (void)t; }
static inline void fake_init(fake_t *f, fake_fn fn, void *user) {
  memset(f, 0, sizeof *f);
  f->base.get = fake_get;
  f->base.destroy = fake_destroy;
  f->fn = fn;
  f->user = user;
}
static inline const char *fake_last_url(const fake_t *f) { return f->urls[(f->calls - 1) % 32]; }

/* copy a body into buf with the too-large rule */
static inline dz_net_err_t fake_body(const char *body, int status, char *buf, size_t cap, dz_http_result_t *out) {
  size_t n = strlen(body);
  out->status = status;
  if (n >= cap) { n = cap - 1; memcpy(buf, body, n); buf[n] = 0; out->len = n; return DZ_NET_TOO_LARGE; }
  memcpy(buf, body, n + 1);
  out->len = n;
  return DZ_NET_OK;
}
