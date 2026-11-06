/* Scripted fake transport for client/command tests: no sockets, no clock. */
#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

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
  /* POST (MCP): */
  int posts;                 /* number of POSTs */
  char body[512];            /* last POST body */
  char bodies[16][512];      /* ring of the last bodies */
  char hdrs[8][160];         /* the request headers of the last call, in order ("Name: value"), NULL-terminated by empty */
  int post_secret_leak;      /* the secret appeared in a URL, a body or a non-Authorization header */
  const char *sid_script;    /* session id the fake answers to the capture header ("" -> header absent) */
  int init_calls;            /* POSTs that asked for a header capture */
  int sid_in_req;            /* last POST carried an Mcp-Session-Id header */
  char sid_seen[80];         /* its value */
  int proto_hdr;             /* a POST carried an MCP-Protocol-Version header (must stay 0) */
};

/* JSON-RPC echo: a scripted reply that starts with {"id":N, is rewritten to carry the id of the request being answered
 * (fake_force_id != 0 overrides it: a stale or foreign reply). The client requires the ids to match. */
static unsigned fake_cur_id, fake_force_id;

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
/* body of a POST: the fake's fn sees `url` and f->body; status and body go out through buf/out as for GET */
static inline dz_net_err_t fake_post(dz_transport_t *t, const char *url, const char *const *headers, const dz_post_t *p,
                                     uint32_t timeout_ms, char *buf, size_t cap, dz_http_result_t *out) {
  fake_t *f = (fake_t *)t;
  (void)timeout_ms;
  snprintf(f->urls[f->calls % 32], sizeof f->urls[0], "%s", url);
  f->calls++;
  snprintf(f->body, sizeof f->body, "%.*s", (int)p->len, p->body);
  { const char *ip = strstr(f->body, "\"id\":"); fake_cur_id = ip ? (unsigned)strtoul(ip + 5, NULL, 10) : 0; }
  snprintf(f->bodies[f->posts % 16], sizeof f->bodies[0], "%s", f->body);
  f->posts++;
  f->had_auth = 0;
  f->sid_in_req = 0;
  f->sid_seen[0] = 0;
  int hn = 0;
  for (size_t i = 0; headers && headers[i]; i++) {
    if (!strncmp(headers[i], "Authorization: Bearer ", 22)) f->had_auth = 1;
    else if (f->secret && strstr(headers[i], f->secret)) f->post_secret_leak = 1;
    if (!strncmp(headers[i], "Mcp-Session-Id: ", 16)) { f->sid_in_req = 1; snprintf(f->sid_seen, sizeof f->sid_seen, "%s", headers[i] + 16); }
    if (!strncasecmp(headers[i], "MCP-Protocol-Version", 20)) f->proto_hdr = 1;
    if (hn < 7) snprintf(f->hdrs[hn++], sizeof f->hdrs[0], "%s", headers[i]);
  }
  f->hdrs[hn][0] = 0;
  if (f->secret && (strstr(url, f->secret) || strstr(f->body, f->secret))) { f->auth_in_url = 1; f->post_secret_leak = 1; }
  if (p->hdr_out && p->hdr_cap) p->hdr_out[0] = 0;
  if (p->capture_header) {
    f->init_calls++;
    if (p->hdr_out && p->hdr_cap && f->sid_script) snprintf(p->hdr_out, p->hdr_cap, "%s", f->sid_script);
  }
  out->status = 0;
  out->len = 0;
  buf[0] = 0;
  return f->fn(f, url, buf, cap, out);
}
static inline void fake_destroy(dz_transport_t *t) { (void)t; }
static inline void fake_init(fake_t *f, fake_fn fn, void *user) {
  memset(f, 0, sizeof *f);
  f->base.get = fake_get;
  f->base.post = fake_post;
  f->base.destroy = fake_destroy;
  f->fn = fn;
  f->user = user;
}
static inline const char *fake_last_url(const fake_t *f) { return f->urls[(f->calls - 1) % 32]; }

/* copy a body into buf with the too-large rule */
static inline dz_net_err_t fake_body(const char *body, int status, char *buf, size_t cap, dz_http_result_t *out) {
  char idb[1100];
  if (!strncmp(body, "{\"id\":", 6) && strlen(body) < sizeof idb - 16) {
    const char *rest = body + 6 + strspn(body + 6, "0123456789");
    snprintf(idb, sizeof idb, "{\"id\":%u%s", fake_force_id ? fake_force_id : fake_cur_id, rest);
    body = idb;
  }
  size_t n = strlen(body);
  out->status = status;
  if (n >= cap) { n = cap - 1; memcpy(buf, body, n); buf[n] = 0; out->len = n; return DZ_NET_TOO_LARGE; }
  memcpy(buf, body, n + 1);
  out->len = n;
  return DZ_NET_OK;
}
