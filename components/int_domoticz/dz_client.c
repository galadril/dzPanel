#include "dz_client.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PH_VERSION, PH_CONFIG, PH_DEVICES_FULL, PH_SCENES, PH_PLANS, PH_DELTA };

static void wipe(void *p, size_t n) {
  volatile unsigned char *v = p;
  while (n--) *v++ = 0;
}

const char *dz_conn_state_name(dz_conn_state_t s) {
  static const char *const n[] = {"connecting", "ok", "offline", "token-rejected", "server-error", "bad-response"};
  return (unsigned)s < 6 ? n[s] : "?";
}
const char *dz_client_err_name(dz_client_err_t e) {
  static const char *const n[] = {"none", "timeout", "connect", "tls", "too-large", "http", "parse", "status"};
  return (unsigned)e < 8 ? n[e] : "?";
}

bool dz_version_parse(const char *s, dz_version_t *out) {
  memset(out, 0, sizeof *out);
  if (!s) return false;
  while (*s && (*s < '0' || *s > '9')) s++;
  char *e;
  long y = strtol(s, &e, 10);
  if (e == s || y < 2000 || y > 2999 || *e != '.') return false;
  s = e + 1;
  long mi = strtol(s, &e, 10);
  if (e == s || mi < 0 || mi > 99) return false;
  out->year = (int)y;
  out->minor = (int)mi;
  const char *b = strstr(e, "build");
  if (b) {
    b += 5;
    while (*b == ' ' || *b == ':' || *b == '=') b++;
    long bn = strtol(b, &e, 10);
    if (e != b && bn >= 0 && bn < 100000000) out->build = (int)bn;
  }
  out->valid = true;
  return true;
}

bool dz_version_at_least(const dz_version_t *v, int year, int minor) {
  return v->valid && (v->year > year || (v->year == year && v->minor >= minor));
}

bool dz_client_token_from_env(char *out, size_t cap) {
  const char *t = getenv("DOMOTICZ_ACCESSTOKEN");
  if (!t || !*t || strlen(t) >= cap) return false;
  memcpy(out, t, strlen(t) + 1);
  return true;
}

bool dz_client_init(dz_client_t *c, const dz_client_cfg_t *cfg) {
  if (!c || !cfg || !cfg->base_url || !cfg->transport || !cfg->model || !cfg->buf || cfg->buf_cap < 256) return false;
  memset(c, 0, sizeof *c);
  c->cfg = *cfg;
  size_t bl = strlen(cfg->base_url);
  while (bl && cfg->base_url[bl - 1] == '/') bl--;
  if (bl == 0 || bl >= sizeof c->base || (strncmp(cfg->base_url, "http://", 7) && strncmp(cfg->base_url, "https://", 8)))
    return false;
  memcpy(c->base, cfg->base_url, bl);
  if (cfg->token && *cfg->token) {
    if (strlen(cfg->token) >= sizeof c->token) return false;
    for (const char *p = cfg->token; *p; p++)
      if ((unsigned char)*p <= 0x20 || *p == 0x7f) return false; /* no whitespace or control chars in a bearer token */
    memcpy(c->token, cfg->token, strlen(cfg->token) + 1);
  }
  c->cfg.base_url = c->base;
  c->cfg.token = NULL; /* never keep the caller's pointer */
  if (!c->cfg.poll_interval_ms) c->cfg.poll_interval_ms = 10000;
  if (c->cfg.poll_interval_ms < 1000) c->cfg.poll_interval_ms = 1000;
  if (!c->cfg.startup_gap_ms) c->cfg.startup_gap_ms = 2000;
  if (c->cfg.startup_gap_ms < 1000) c->cfg.startup_gap_ms = 1000;
  if (!c->cfg.full_refresh_ms) c->cfg.full_refresh_ms = 600000;
  if (!c->cfg.timeout_ms) c->cfg.timeout_ms = 8000;
  if (!c->cfg.backoff_max_ms) c->cfg.backoff_max_ms = 300000;
  if (!c->cfg.scenes_every) c->cfg.scenes_every = 3;
  c->state = DZ_CS_CONNECTING;
  c->phase = PH_VERSION;
  return true;
}

dz_conn_state_t dz_client_state(const dz_client_t *c) { return c->state; }

static void set_state(dz_client_t *c, dz_conn_state_t s) {
  if (c->state == s) return;
  dz_conn_state_t old = c->state;
  c->state = s;
  if (c->cfg.on_state) c->cfg.on_state(c->cfg.user, old, s);
}

void dz_client_mcp_reset(dz_client_t *c) {
  wipe(c->mcp_sid, sizeof c->mcp_sid);
  c->mcp_last_ms = 0;
}

void dz_client_set_token(dz_client_t *c, const char *token) {
  dz_client_mcp_reset(c); /* a session belongs to the token it was opened with */
  if (token && strlen(token) < sizeof c->token) {
    wipe(c->token, sizeof c->token);
    memcpy(c->token, token, strlen(token) + 1);
  }
  c->halted = false;
  c->failures = 0;
  c->next_due_ms = 0;
  c->phase = PH_VERSION;
  set_state(c, DZ_CS_CONNECTING);
  c->ever_ok = false;
}

void dz_client_retry_now(dz_client_t *c, uint32_t now_ms) {
  dz_client_mcp_reset(c); /* new network, new server maybe: open a fresh session on the next command */
  c->halted = false;
  c->failures = 0;
  c->next_due_ms = now_ms;
  if (c->state != DZ_CS_OK) set_state(c, DZ_CS_CONNECTING);
}

void dz_client_set_poll_interval(dz_client_t *c, uint32_t ms) {
  c->cfg.poll_interval_ms = ms < 1000 ? 1000 : ms;
}

uint32_t dz_client_ms_until_due(const dz_client_t *c, uint32_t now_ms) {
  if (c->halted) return UINT32_MAX;
  int32_t d = (int32_t)(c->next_due_ms - now_ms);
  return d > 0 ? (uint32_t)d : 0;
}

void dz_client_note_auth_failure(dz_client_t *c) {
  dz_client_mcp_reset(c);
  c->halted = true;
  c->last_err = DZ_CERR_HTTP;
  set_state(c, DZ_CS_TOKEN_REJECTED);
}

dz_net_err_t dz_client_request(dz_client_t *c, const char *query, dz_http_result_t *r) {
  char url[DZ_URL_MAX + 512];
  char hdr[sizeof "Authorization: Bearer " + DZ_TOKEN_MAX];
  const char *hdrs[2] = {NULL, NULL};
  dz_http_result_t local;
  if (!r) r = &local;
  int n = snprintf(url, sizeof url, "%s/json.htm?type=command&%s", c->base, query);
  if (n < 0 || (size_t)n >= sizeof url) { r->status = 0; r->len = 0; c->cfg.buf[0] = 0; return DZ_NET_CONNECT; }
  if (c->token[0]) {
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s", c->token);
    hdrs[0] = hdr;
  }
  c->requests++;
  dz_net_err_t e = c->cfg.transport->get(c->cfg.transport, url, hdrs[0] ? hdrs : NULL, c->cfg.timeout_ms, c->cfg.buf,
                                         c->cfg.buf_cap, r);
  wipe(hdr, sizeof hdr);
  c->cfg.buf[c->cfg.buf_cap - 1] = 0;
  return e;
}

/* ---------------------------------------------------------------- MCP */

uint32_t dz_client_mcp_next_id(dz_client_t *c) { return ++c->mcp_rpc_id; }

static bool sid_ok(const char *s) {
  if (!*s) return false;
  for (; *s; s++)
    if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '-' || *s == '_' || *s == '.'))
      return false;
  return true;
}

/* One POST to <base>/mcp. With capture, the Mcp-Session-Id of the answer lands in c->mcp_sid. */
static dz_net_err_t mcp_post(dz_client_t *c, const char *body, bool with_sid, bool capture, dz_http_result_t *r) {
  char url[DZ_URL_MAX + 8];
  char auth[sizeof "Authorization: Bearer " + DZ_TOKEN_MAX];
  char sidh[sizeof "Mcp-Session-Id: " + sizeof c->mcp_sid];
  const char *hdrs[4];
  size_t nh = 0;
  r->status = 0;
  r->len = 0;
  if (!c->cfg.transport->post) { c->cfg.buf[0] = 0; return DZ_NET_CONNECT; }
  int n = snprintf(url, sizeof url, "%s/mcp", c->base);
  if (n < 0 || (size_t)n >= sizeof url) { c->cfg.buf[0] = 0; return DZ_NET_CONNECT; }
  if (c->token[0]) {
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", c->token);
    hdrs[nh++] = auth;
  }
  hdrs[nh++] = "Accept: application/json, text/event-stream"; /* and NO MCP-Protocol-Version: an older value is HTTP 400 */
  if (with_sid) {
    snprintf(sidh, sizeof sidh, "Mcp-Session-Id: %s", c->mcp_sid);
    hdrs[nh++] = sidh;
  }
  hdrs[nh] = NULL;
  dz_post_t p = {body, strlen(body), "application/json", capture ? "Mcp-Session-Id" : NULL, capture ? c->mcp_sid : NULL,
                 capture ? sizeof c->mcp_sid : 0};
  c->requests++;
  dz_net_err_t e = c->cfg.transport->post(c->cfg.transport, url, hdrs, &p, c->cfg.timeout_ms, c->cfg.buf, c->cfg.buf_cap, r);
  wipe(auth, sizeof auth);
  wipe(sidh, sizeof sidh);
  c->cfg.buf[c->cfg.buf_cap - 1] = 0;
  return e;
}

static void res_clear(dz_mcp_result_t *res) {
  memset(res, 0, sizeof *res);
  res->parse = DZ_ERR_SYNTAX;
}

static bool is_session_error(const dz_mcp_reply_t *r) {
  return r->has_error && (r->code == -32001 || (r->code == -32600 && strstr(r->message, "ession")));
}

/* Runs one request and fills res from it. */
static bool mcp_exchange(dz_client_t *c, const char *body, bool with_sid, bool capture, dz_mcp_stage_t stage, dz_mcp_result_t *res) {
  dz_http_result_t hr;
  res->stage = stage;
  res->net = mcp_post(c, body, with_sid, capture, &hr);
  res->http = hr.status;
  res->parse = DZ_ERR_SYNTAX;
  memset(&res->reply, 0, sizeof res->reply);
  if (res->net != DZ_NET_OK || hr.status != 200) {
    if (hr.status == 401) dz_client_mcp_reset(c);
    return false;
  }
  res->parse = dz_mcp_parse_reply(c->cfg.buf, hr.len, &res->reply);
  return res->parse == DZ_OK;
}

bool dz_client_mcp_open(dz_client_t *c, uint32_t now_ms, bool force, dz_mcp_result_t *res) {
  dz_mcp_result_t local;
  if (!res) res = &local;
  res_clear(res);
  if (!force && c->mcp_sid[0] && (int32_t)(now_ms - c->mcp_last_ms) <= 55 * 60 * 1000) return true;
  dz_client_mcp_reset(c);
  snprintf(c->mcp_body, sizeof c->mcp_body,
           "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\","
           "\"capabilities\":{},\"clientInfo\":{\"name\":\"dzPanel\",\"version\":\"1\"}}}",
           (unsigned)dz_client_mcp_next_id(c));
  bool ok = mcp_exchange(c, c->mcp_body, false, true, DZ_MCP_STAGE_INIT, res);
  if (!ok || !res->reply.has_result || res->reply.has_error || !sid_ok(c->mcp_sid)) {
    if (ok) res->parse = DZ_ERR_STATUS; /* a reply, but no session came with it */
    dz_client_mcp_reset(c);
    return false;
  }
  c->mcp_last_ms = now_ms;
  /* The server ignores this message, so a failure here is not fatal; its HTML 202 body is never parsed. */
  dz_mcp_result_t ignore;
  res_clear(&ignore);
  snprintf(c->mcp_body, sizeof c->mcp_body, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
  (void)mcp_exchange(c, c->mcp_body, true, false, DZ_MCP_STAGE_INIT, &ignore);
  return true;
}

bool dz_client_mcp_call(dz_client_t *c, uint32_t now_ms, const char *body, dz_mcp_result_t *res) {
  dz_mcp_result_t local;
  if (!res) res = &local;
  res_clear(res);
  if (!body || strlen(body) >= sizeof c->mcp_call) return false;
  if (body != c->mcp_call) memmove(c->mcp_call, body, strlen(body) + 1); /* a caller that encoded in place passes c->mcp_call */
  body = c->mcp_call;
  if (!dz_client_mcp_open(c, now_ms, false, res)) return false;
  for (int attempt = 0;; attempt++) {
    bool ok = mcp_exchange(c, body, true, false, DZ_MCP_STAGE_CALL, res);
    res->reinit = attempt > 0;
    if (!ok) return false; /* transport error, timeout, non-200, non-JSON: never retried */
    if (!is_session_error(&res->reply)) {
      c->mcp_last_ms = now_ms;
      return true;
    }
    if (attempt > 0) { res->session_lost = true; dz_client_mcp_reset(c); return true; }
    /* 'Missing session' / 'Session not found' arrive before the tool runs: one re-initialize and one retry is safe */
    if (!dz_client_mcp_open(c, now_ms, true, res)) { res->reinit = true; return false; }
    res->reinit = true; /* res was cleared by the open; the loop sends the call again */
  }
}

/* ---- top-level field scan (ActTime, app_version, Sunrise...) without a full parse ---- */
static bool top_field(const char *j, size_t n, const char *key, char *out, size_t cap) {
  size_t i = 0, kl = strlen(key);
  int depth = 0;
  bool expect_key = false;
  while (i < n && (j[i] == ' ' || j[i] == '\t' || j[i] == '\r' || j[i] == '\n')) i++;
  if (i >= n || j[i] != '{') return false;
  for (; i < n; i++) {
    char ch = j[i];
    if (ch == '"') {
      size_t st = ++i;
      while (i < n && j[i] != '"') i += (j[i] == '\\' && i + 1 < n) ? 2 : 1;
      if (i >= n) return false;
      size_t en = i;
      if (depth == 1 && expect_key) {
        expect_key = false;
        if (en - st == kl && !memcmp(j + st, key, kl)) {
          size_t p = en + 1;
          while (p < n && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n')) p++;
          if (p >= n || j[p] != ':') return false;
          p++;
          while (p < n && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n')) p++;
          size_t o = 0;
          if (p < n && j[p] == '"') {
            for (p++; p < n && j[p] != '"' && o + 1 < cap; p++) {
              if (j[p] == '\\') { if (p + 1 < n) p++; else break; }
              out[o++] = j[p];
            }
          } else {
            while (p < n && o + 1 < cap && j[p] != ',' && j[p] != '}' && j[p] != ' ' && j[p] != '\n' && j[p] != '\r' && j[p] != '\t')
              out[o++] = j[p++];
          }
          out[o] = 0;
          return o > 0;
        }
      }
      continue;
    }
    if (ch == '{' || ch == '[') { depth++; if (ch == '{' && depth == 1) expect_key = true; }
    else if (ch == '}' || ch == ']') { depth--; if (depth <= 0) return false; }
    else if (ch == ',' && depth == 1) expect_key = true;
  }
  return false;
}

/* getconfig -> units. Reads exactly five keys through top_field and nothing else: the document also holds the user's
 * coordinates and other private settings, which are neither copied, kept nor logged. */
static bool printable_token(const char *t, size_t max) {
  size_t n = strlen(t);
  if (n == 0 || n > max) return false;
  for (size_t i = 0; i < n; i++) if ((unsigned char)t[i] < 0x21 || (unsigned char)t[i] > 0x7e) return false;
  return true;
}

static bool num_field(const char *j, size_t n, const char *key, float *out) {
  char t[24], *e;
  if (!top_field(j, n, key, t, sizeof t)) return false;
  float f = strtof(t, &e);
  if (e == t || *e || !isfinite(f) || f <= 0.0f || f > 1000.0f) return false;
  *out = f;
  return true;
}

bool dz_units_parse(const char *json, size_t len, dz_units_t *out) {
  char t[16];
  dz_result_t res;
  memset(out, 0, sizeof *out);
  if (!json || dz_parse_response(json, len, NULL, NULL, &res) != DZ_OK) return false;
  if (top_field(json, len, "TempSign", t, sizeof t) && (!strcmp(t, "C") || !strcmp(t, "F"))) {
    out->have_temp = true;
    memcpy(out->temp, t, 2);
  }
  if (top_field(json, len, "WindSign", t, sizeof t) && printable_token(t, sizeof out->wind - 1)) {
    out->have_wind = true;
    memcpy(out->wind, t, strlen(t) + 1);
  }
  if (top_field(json, len, "CurrencySign", t, sizeof t) && strlen(t) < sizeof out->currency) {
    out->have_currency = true;
    memcpy(out->currency, t, strlen(t) + 1);
  }
  out->have_temp_scale = num_field(json, len, "TempScale", &out->temp_scale);
  out->have_wind_scale = num_field(json, len, "WindScale", &out->wind_scale);
  return out->have_temp || out->have_wind || out->have_currency || out->have_temp_scale || out->have_wind_scale;
}

static bool filter_cb(const dz_device_t *d, void *user) {
  dz_client_t *c = user;
  if (c->cfg.favorites_only && !d->favorite) return true;
  /* getdevices also lists groups, under the scene idx. The scene namespace is fed by getscenes only:
   * two differently shaped rows for one idx would flip-flop the model and the change counter. */
  if (c->phase != PH_SCENES && dz_cap_is_scene_ns(d->cap)) return true;
  return dz_model_stage_cb(d, c->cfg.model);
}

static bool plan_cb(const dz_device_t *d, void *user) {
  dz_client_t *c = user;
  if (c->n_plans < DZ_MAX_PLANS) {
    snprintf(c->plans[c->n_plans].idx, sizeof c->plans[0].idx, "%s", d->idx);
    snprintf(c->plans[c->n_plans].name, sizeof c->plans[0].name, "%s", d->name);
    c->n_plans++;
  }
  return true;
}

static void fail(dz_client_t *c, dz_conn_state_t s, dz_client_err_t e, uint32_t now) {
  c->last_err = e;
  c->failures++;
  uint32_t base = c->cfg.poll_interval_ms, d = base;
  for (unsigned i = 1; i < c->failures && d < c->cfg.backoff_max_ms; i++) d = d > UINT32_MAX / 2 ? UINT32_MAX : d * 2;
  if (d > c->cfg.backoff_max_ms) d = c->cfg.backoff_max_ms;
  c->next_due_ms = now + d;
  if (s == DZ_CS_OFFLINE) {
    /* a couple of transport blips on a healthy link do not flip the state */
    if (c->state == DZ_CS_OK && c->failures < 3) return;
    if (c->state == DZ_CS_CONNECTING && !c->ever_ok && c->failures < 3) return;
  }
  set_state(c, s);
}

static const char *phase_query(dz_client_t *c, char *q, size_t cap) {
  const char *fav = c->cfg.favorites_only ? "&favorite=1" : "";
  switch (c->phase) {
    case PH_VERSION: return "param=getversion";
    case PH_CONFIG: return "param=getconfig";
    case PH_DEVICES_FULL: snprintf(q, cap, "param=getdevices&filter=all&used=true&order=Name%s", fav); return q;
    case PH_SCENES: return "param=getscenes";
    case PH_PLANS: return "param=getplans";
    default: {
      if (!c->cursor) { snprintf(q, cap, "param=getdevices&filter=all&used=true&order=Name%s", fav); return q; }
      int64_t cur = c->cursor > 2 ? c->cursor - 2 : c->cursor; /* overlap: upserts are idempotent */
      snprintf(q, cap, "param=getdevices&filter=all&used=true&order=Name%s&lastupdate=%lld", fav, (long long)cur);
      return q;
    }
  }
}

static void next_phase(dz_client_t *c, uint32_t now) {
  uint32_t gap = c->cfg.poll_interval_ms;
  switch (c->phase) {
    case PH_VERSION: c->phase = PH_CONFIG; gap = c->cfg.startup_gap_ms; break;
    case PH_CONFIG: c->phase = PH_DEVICES_FULL; if (!c->cursor) gap = c->cfg.startup_gap_ms; break; /* a refresh keeps the poll rhythm */
    case PH_DEVICES_FULL: c->phase = PH_SCENES; c->last_full_ms = now; c->delta_count = 0; gap = c->cfg.startup_gap_ms; break;
    case PH_SCENES: {
      bool initial = c->delta_count == 0;
      if (initial) { c->phase = PH_PLANS; gap = c->cfg.startup_gap_ms; }
      else c->phase = PH_DELTA;
      break;
    }
    case PH_PLANS: c->phase = PH_DELTA; break;
    default:
      c->delta_count++;
      if ((uint32_t)(now - c->last_full_ms) >= c->cfg.full_refresh_ms) c->phase = PH_CONFIG; /* units are re-read on every full refresh */
      else if (c->delta_count % c->cfg.scenes_every == 0) c->phase = PH_SCENES;
      break;
  }
  c->next_due_ms = now + gap;
}

bool dz_client_step(dz_client_t *c, uint32_t now) {
  if (c->halted || (int32_t)(now - c->next_due_ms) < 0) return false;
  char q[256];
  const char *query = phase_query(c, q, sizeof q);
  dz_http_result_t r;
  dz_net_err_t ne = dz_client_request(c, query, &r);
  c->last_http_status = r.status;
  if (ne == DZ_NET_TIMEOUT) { fail(c, DZ_CS_OFFLINE, DZ_CERR_TIMEOUT, now); return true; }
  if (ne == DZ_NET_CONNECT) { fail(c, DZ_CS_OFFLINE, DZ_CERR_CONNECT, now); return true; }
  if (ne == DZ_NET_TLS) { fail(c, DZ_CS_OFFLINE, DZ_CERR_TLS, now); return true; }
  if (r.status == 401 || r.status == 403) {
    c->last_err = DZ_CERR_HTTP;
    c->halted = true; /* stop polling: a retry storm helps nobody */
    set_state(c, DZ_CS_TOKEN_REJECTED);
    return true;
  }
  if (c->phase == PH_CONFIG && (r.status != 200 || ne == DZ_NET_TOO_LARGE)) {
    /* getconfig is a nicety: a server that refuses it (old, restricted user) must not stop the device list. Units
     * simply stay unknown and the screens show bare numbers. */
    c->failures = 0;
    next_phase(c, now);
    return true;
  }
  if (r.status >= 500) { fail(c, DZ_CS_SERVER_ERROR, DZ_CERR_HTTP, now); return true; }
  if (r.status != 200) { fail(c, DZ_CS_BAD_RESPONSE, DZ_CERR_HTTP, now); return true; }
  if (ne == DZ_NET_TOO_LARGE) { fail(c, DZ_CS_BAD_RESPONSE, DZ_CERR_TOO_LARGE, now); return true; }

  const char *body = c->cfg.buf;
  dz_result_t res;
  dz_err_t pe;
  int changed;
  switch (c->phase) {
    case PH_DEVICES_FULL:
    case PH_DELTA: {
      /* no ActTime from the server: never guess a cursor, keep doing full reads */
      dz_commit_t mode = (c->phase == PH_DEVICES_FULL || !c->cursor) ? DZ_COMMIT_FULL_DEVICES : DZ_COMMIT_DELTA;
      dz_model_begin(c->cfg.model);
      pe = dz_parse_response(body, r.len, filter_cb, c, &res);
      if (pe == DZ_OK) changed = dz_model_commit(c->cfg.model, mode, res.server_time, now);
      else dz_model_abort(c->cfg.model); /* a half-parsed response never replaces good data */
      (void)changed;
      if (pe == DZ_OK) {
        char t[24];
        if (top_field(body, r.len, "ActTime", t, sizeof t)) {
          char *e;
          long long v = strtoll(t, &e, 10);
          if (!*e && v > 0) c->cursor = v;
        }
        if (top_field(body, r.len, "Sunrise", t, sizeof t)) snprintf(c->sunrise, sizeof c->sunrise, "%s", t);
        if (top_field(body, r.len, "Sunset", t, sizeof t)) snprintf(c->sunset, sizeof c->sunset, "%s", t);
      }
      break;
    }
    case PH_SCENES:
      dz_model_begin(c->cfg.model);
      pe = dz_parse_response(body, r.len, filter_cb, c, &res);
      if (pe == DZ_OK) dz_model_commit(c->cfg.model, DZ_COMMIT_FULL_SCENES, res.server_time, now);
      else dz_model_abort(c->cfg.model);
      break;
    case PH_PLANS:
      c->n_plans = 0;
      pe = dz_parse_response(body, r.len, plan_cb, c, &res);
      if (pe != DZ_OK) c->n_plans = 0;
      break;
    case PH_CONFIG: {
      dz_units_t u;
      if (dz_units_parse(body, r.len, &u)) dz_model_set_units(c->cfg.model, &u);
      pe = DZ_OK; /* never fatal */
      break;
    }
    default: { /* PH_VERSION */
      pe = dz_parse_response(body, r.len, NULL, NULL, &res);
      if (pe == DZ_OK) {
        /* dz_result_t.version is only 16 bytes and cuts "2026.4 (build 18568)": read the full text ourselves.
         * Lenient: an unparsable version is not an error. */
        char vt[48];
        const char *vs = top_field(body, r.len, "version", vt, sizeof vt) ? vt : res.version;
        if (!dz_version_parse(vs, &c->version)) c->version.valid = false;
      }
      break;
    }
  }
  if (pe == DZ_ERR_SYNTAX) { fail(c, DZ_CS_BAD_RESPONSE, DZ_CERR_PARSE, now); return true; }
  if (pe == DZ_ERR_STATUS) { fail(c, DZ_CS_SERVER_ERROR, DZ_CERR_STATUS, now); return true; }

  c->failures = 0;
  c->last_err = DZ_CERR_NONE;
  c->ever_ok = true;
  set_state(c, DZ_CS_OK);
  next_phase(c, now);
  return true;
}
