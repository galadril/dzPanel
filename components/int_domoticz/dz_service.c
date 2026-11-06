#include "dz_service.h"

#include <stdio.h>
#include <string.h>

uint32_t dz_ev_key_device(const char *idx, bool scene) {
  uint32_t v = 0;
  const char *p = idx;
  bool numeric = p && *p;
  for (; p && *p; p++) {
    if (*p < '0' || *p > '9' || v > 100000000u) { numeric = false; break; }
    v = v * 10 + (uint32_t)(*p - '0');
  }
  if (!numeric) { /* FNV-1a for non-numeric ids, folded into 27 bits */
    v = 2166136261u;
    for (p = idx; p && *p; p++) { v ^= (unsigned char)*p; v *= 16777619u; }
    v &= 0x7ffffffu;
  }
  return 16u + ((v << 1) | (scene ? 1u : 0u));
}

static void secure_zero(void *p, size_t n) { ru_secure_zero(p, n); }

static uint16_t topic_of(const dz_service_t *s) { return s->cfg.topic ? s->cfg.topic : DZ_SERVICE_TOPIC_DEFAULT; }

static void post(dz_service_t *s, uint16_t type, uint32_t key, const void *data, size_t len) {
  if (!s->cfg.bus) return;
  if (ru_bus_post_data(s->cfg.bus, topic_of(s), type, key, data, len) == RU_OK) s->events_posted++;
  else s->events_failed++;
}

static void wake(dz_service_t *s) {
  uint8_t x = 1;
  (void)ru_queue_send(s->wake, &x, 0); /* full == already woken */
}

/* ---------------- init ---------------- */

bool dz_service_init(dz_service_t *s, const dz_service_cfg_t *cfg) {
  if (!s || !cfg || !cfg->transport || !cfg->buf || !cfg->entries || !cfg->stage) return false;
  size_t need = cfg->favorites_only ? DZ_SERVICE_MIN_BUF_FAVS : DZ_SERVICE_MIN_BUF_FULL;
  if (cfg->buf_cap < need) return false; /* a full list on a real server is ~75 KB: refuse a buffer that cannot hold it */
  memset(s, 0, sizeof *s);
  s->cfg = *cfg;
  if (cfg->topic >= RU_BUS_MAX_TOPICS) return false;
  if (!dz_model_init(&s->model, cfg->entries, cfg->entries_cap, cfg->stage, cfg->stage_cap, &cfg->model_cfg)) return false;
  s->mu = ru_mutex_create();
  s->wake = ru_queue_create(1, 1);
  if (!s->mu || !s->wake) {
    if (s->mu) ru_mutex_destroy(s->mu);
    if (s->wake) ru_queue_destroy(s->wake);
    dz_model_deinit(&s->model);
    return false;
  }
  s->net_up = true;
  s->next_req_id = 1;
  s->conn_state = DZ_CS_CONNECTING;
  if (cfg->bus) ru_bus_set_coalescing(cfg->bus, topic_of(s), true);
  return true;
}

void dz_service_deinit(dz_service_t *s) {
  if (!s || !s->mu) return;
  dz_service_stop(s);
  secure_zero(&s->mb_cfg, sizeof s->mb_cfg);
  secure_zero(&s->client, sizeof s->client); /* holds the token */
  ru_mutex_destroy(s->mu);
  ru_queue_destroy(s->wake);
  dz_model_deinit(&s->model);
  s->mu = NULL;
}

dz_model_t *dz_service_model(dz_service_t *s) { return &s->model; }
dz_conn_state_t dz_service_conn_state(dz_service_t *s) { return (dz_conn_state_t)s->conn_state; }

/* ---------------- thread-safe API ---------------- */

ru_cfg_err_t dz_service_configure(dz_service_t *s, const ru_cfg_t *cfg, bool allow_control) {
  ru_cfg_err_t e = ru_cfg_validate(cfg);
  if (e != RU_CFG_OK) return e;
  ru_mutex_lock(s->mu);
  s->mb_cfg.cfg = *cfg;
  s->mb_cfg.allow_control = allow_control;
  s->mb_cfg.pending = true;
  s->mb_clear = false;
  ru_mutex_unlock(s->mu);
  wake(s);
  return RU_CFG_OK;
}

void dz_service_clear_config(dz_service_t *s) {
  ru_mutex_lock(s->mu);
  secure_zero(&s->mb_cfg, sizeof s->mb_cfg);
  s->mb_clear = true;
  ru_mutex_unlock(s->mu);
  wake(s);
}

void dz_service_set_net_up(dz_service_t *s, bool up) {
  ru_mutex_lock(s->mu);
  if (up && !s->net_up) s->mb_retry = true;
  s->net_up = up;
  ru_mutex_unlock(s->mu);
  wake(s);
}

void dz_service_set_poll_interval(dz_service_t *s, uint32_t ms) {
  ru_mutex_lock(s->mu);
  s->mb_poll_ms = ms;
  s->mb_poll_set = true;
  ru_mutex_unlock(s->mu);
  wake(s);
}

void dz_service_retry(dz_service_t *s) {
  ru_mutex_lock(s->mu);
  s->mb_retry = true;
  ru_mutex_unlock(s->mu);
  wake(s);
}

uint32_t dz_service_submit(dz_service_t *s, const dz_cmd_req_t *req) {
  if (!req || !req->idx || strlen(req->idx) >= sizeof s->sq[0].idx) return 0;
  uint32_t id = 0;
  ru_mutex_lock(s->mu);
  if (s->sq_n < DZ_SERVICE_SUBMIT_QUEUE) {
    id = s->next_req_id++;
    if (!s->next_req_id) s->next_req_id = 1;
    size_t i = s->sq_n++;
    s->sq[i].id = id;
    s->sq[i].req = *req;
    snprintf(s->sq[i].idx, sizeof s->sq[i].idx, "%s", req->idx);
    s->sq[i].req.idx = NULL; /* re-pointed at the task side */
  }
  ru_mutex_unlock(s->mu);
  if (id) wake(s);
  return id;
}

/* ---------------- task side ---------------- */

static void post_cmd(dz_service_t *s, uint32_t req_id, dz_outcome_t o, dz_fail_t f, dz_cmd_kind_t k, dz_cmd_err_t rej, int level) {
  dz_ev_cmd_t ev;
  memset(&ev, 0, sizeof ev);
  ev.req_id = req_id;
  ev.outcome = (uint8_t)o;
  ev.fail = (uint8_t)f;
  ev.kind = (uint8_t)k;
  ev.reject = (uint8_t)rej;
  ev.level = (int16_t)level;
  post(s, DZ_EV_CMD_RESULT, 0, &ev, sizeof ev);
}

static void post_conn(dz_service_t *s, dz_conn_state_t from, dz_conn_state_t to) {
  dz_ev_conn_t ev;
  memset(&ev, 0, sizeof ev);
  ev.state = (uint8_t)to;
  ev.prev = (uint8_t)from;
  ev.err = (uint8_t)s->client.last_err;
  ev.http_status = (int16_t)s->client.last_http_status;
  post(s, DZ_EV_CONN_STATE, DZ_EV_KEY_CONN, &ev, sizeof ev);
}

static void on_conn(void *user, dz_conn_state_t from, dz_conn_state_t to) {
  dz_service_t *s = user;
  s->conn_state = (uint8_t)to;
  post_conn(s, from, to);
  if (to == DZ_CS_TOKEN_REJECTED) {
    dz_ev_auth_t a = {(int16_t)s->client.last_http_status};
    post(s, DZ_EV_AUTH_REJECTED, DZ_EV_KEY_AUTH, &a, sizeof a);
  }
}

static void on_cmd(const dz_cmd_result_t *r, void *user) {
  dz_service_t *s = user;
  uint32_t req_id = 0;
  for (size_t i = 0; i < DZ_CMD_QUEUE; i++)
    if (s->idmap[i].used && s->idmap[i].cmd_id == r->id) { req_id = s->idmap[i].svc_id; s->idmap[i].used = false; break; }
  post_cmd(s, req_id, r->outcome, r->fail, r->kind, DZ_CMD_ACCEPTED, r->level_sent);
  if (r->fail == DZ_FAIL_UNAUTHORIZED) {
    dz_ev_auth_t a = {401};
    post(s, DZ_EV_AUTH_REJECTED, DZ_EV_KEY_AUTH, &a, sizeof a);
  }
}

static void fail_inflight(dz_service_t *s) {
  for (size_t i = 0; i < DZ_CMD_QUEUE; i++)
    if (s->idmap[i].used) {
      post_cmd(s, s->idmap[i].svc_id, DZ_OUT_FAILED, DZ_FAIL_TRANSPORT, DZ_CMD_ON, DZ_CMD_ACCEPTED, -1);
      s->idmap[i].used = false;
    }
}

static void apply_mailbox(dz_service_t *s, uint32_t now) {
  ru_cfg_t cfg;
  bool have_cfg, clear, retry, poll_set;
  bool allow;
  uint32_t poll;
  ru_mutex_lock(s->mu);
  have_cfg = s->mb_cfg.pending;
  clear = s->mb_clear;
  retry = s->mb_retry;
  poll_set = s->mb_poll_set;
  poll = s->mb_poll_ms;
  allow = s->mb_cfg.allow_control;
  if (have_cfg) { cfg = s->mb_cfg.cfg; secure_zero(&s->mb_cfg, sizeof s->mb_cfg); }
  s->mb_clear = s->mb_retry = s->mb_poll_set = false;
  ru_mutex_unlock(s->mu);

  if (poll_set) s->cfg.poll_interval_ms = poll;
  if (clear) {
    fail_inflight(s);
    secure_zero(&s->client, sizeof s->client);
    s->client_ready = false;
    s->conn_state = DZ_CS_CONNECTING;
  }
  if (have_cfg) {
    fail_inflight(s);
    if (s->cfg.set_insecure_lan) s->cfg.set_insecure_lan(s->cfg.transport, cfg.insecure_lan);
    dz_client_cfg_t cc;
    memset(&cc, 0, sizeof cc);
    cc.base_url = cfg.url;
    cc.token = cfg.token;
    cc.transport = s->cfg.transport;
    cc.model = &s->model;
    cc.buf = s->cfg.buf;
    cc.buf_cap = s->cfg.buf_cap;
    cc.favorites_only = s->cfg.favorites_only;
    cc.poll_interval_ms = s->cfg.poll_interval_ms;
    cc.timeout_ms = s->cfg.timeout_ms;
    cc.on_state = on_conn;
    cc.user = s;
    s->client_ready = dz_client_init(&s->client, &cc); /* copies url and token into the client */
    if (s->client_ready) {
      s->allow_control = allow;
      dz_cmd_init(&s->cmd, &s->client, &s->model, allow, on_cmd, s);
      s->conn_state = DZ_CS_CONNECTING;
      post_conn(s, DZ_CS_CONNECTING, DZ_CS_CONNECTING);
    } else s->conn_state = DZ_CS_BAD_RESPONSE;
    secure_zero(&cfg, sizeof cfg);
  } else if (poll_set && s->client_ready) dz_client_set_poll_interval(&s->client, s->cfg.poll_interval_ms);
  if (retry && s->client_ready) {
    dz_client_retry_now(&s->client, now);
    s->conn_state = (uint8_t)s->client.state; /* the state callback already posted the change */
  }
}

static void drain_submits(dz_service_t *s, uint32_t now) {
  for (;;) {
    uint32_t id;
    dz_cmd_req_t req;
    char idx[12];
    bool up;
    ru_mutex_lock(s->mu);
    if (!s->sq_n) { ru_mutex_unlock(s->mu); return; }
    id = s->sq[0].id;
    req = s->sq[0].req;
    memcpy(idx, s->sq[0].idx, sizeof idx);
    s->sq_n--;
    memmove(&s->sq[0], &s->sq[1], s->sq_n * sizeof s->sq[0]);
    up = s->net_up;
    ru_mutex_unlock(s->mu);
    req.idx = idx;
    if (!s->client_ready) { post_cmd(s, id, DZ_OUT_FAILED, DZ_FAIL_NONE, req.kind, DZ_CMDE_CONTROL_DISABLED, -1); continue; }
    if (!up) { /* never run a stale command later when Wi-Fi comes back */
      post_cmd(s, id, DZ_OUT_FAILED, DZ_FAIL_TRANSPORT, req.kind, DZ_CMD_ACCEPTED, -1);
      continue;
    }
    uint32_t cid = 0;
    dz_cmd_err_t e = dz_cmd_submit(&s->cmd, &req, now, &cid);
    if (e != DZ_CMD_ACCEPTED) { post_cmd(s, id, DZ_OUT_FAILED, DZ_FAIL_NONE, req.kind, e, -1); continue; }
    size_t i = 0;
    while (i < DZ_CMD_QUEUE && s->idmap[i].used) i++;
    if (i == DZ_CMD_QUEUE) { /* cannot happen (same capacity as dz_cmd); fail visibly rather than lose track */
      post_cmd(s, id, DZ_OUT_FAILED, DZ_FAIL_NONE, req.kind, DZ_CMDE_QUEUE_FULL, -1);
      continue;
    }
    s->idmap[i].used = true;
    s->idmap[i].svc_id = id;
    s->idmap[i].cmd_id = cid;
  }
}

static void post_changes(dz_service_t *s) {
  uint32_t gen = dz_model_generation(&s->model);
  if (gen == s->last_posted_gen) return;
  /* only this task writes the model, so reading the entries without the lock is safe here */
  for (size_t i = 0; i < s->model.n; i++) {
    const dz_entry_t *e = &s->model.e[i];
    if (e->changed_gen <= s->last_posted_gen) continue;
    dz_ev_device_t ev;
    memset(&ev, 0, sizeof ev);
    snprintf(ev.idx, sizeof ev.idx, "%s", e->dev.idx);
    ev.is_scene = e->is_scene ? 1 : 0;
    post(s, DZ_EV_DEVICE_CHANGED, dz_ev_key_device(e->dev.idx, e->is_scene), &ev, sizeof ev);
  }
  dz_ev_model_t m = {gen, (uint32_t)s->model.n};
  post(s, DZ_EV_MODEL_CHANGED, DZ_EV_KEY_MODEL, &m, sizeof m);
  s->last_posted_gen = gen;
}

uint32_t dz_service_run_once(dz_service_t *s, uint32_t now) {
  s->now_ms = now;
  apply_mailbox(s, now);
  drain_submits(s, now);
  if (s->client_ready) {
    bool up;
    ru_mutex_lock(s->mu);
    up = s->net_up;
    ru_mutex_unlock(s->mu);
    bool did = dz_cmd_pump(&s->cmd, now); /* commands first: a tap should not wait for a poll */
    if (!did && up) did = dz_client_step(&s->client, now);
    if (did) s->requests_seen++;
    post_changes(s);
  }
  uint32_t sleep = DZ_SERVICE_MAX_SLEEP_MS;
  bool busy;
  bool up;
  ru_mutex_lock(s->mu);
  busy = s->sq_n || s->mb_cfg.pending || s->mb_clear || s->mb_retry || s->mb_poll_set;
  up = s->net_up;
  ru_mutex_unlock(s->mu);
  if (busy) return 0;
  if (!s->client_ready) return sleep;
  uint32_t d = up ? dz_client_ms_until_due(&s->client, now) : UINT32_MAX;
  if (d < sleep) sleep = d;
  d = dz_cmd_ms_until_due(&s->cmd, now);
  if (d < sleep) sleep = d;
  return sleep;
}

void dz_service_task(void *arg) {
  dz_service_t *s = arg;
  for (;;) {
    bool stop;
    ru_mutex_lock(s->mu);
    stop = s->stop;
    ru_mutex_unlock(s->mu);
    if (stop) break;
    uint32_t ms = dz_service_run_once(s, ru_now_ms());
    if (ms) {
      uint8_t x;
      (void)ru_queue_recv(s->wake, &x, ms);
    }
  }
}

bool dz_service_start(dz_service_t *s) {
  if (s->task) return true;
  s->stop = false;
  s->task = ru_task_create("dz_service", dz_service_task, s);
  return s->task != NULL;
}

void dz_service_stop(dz_service_t *s) {
  if (!s->task) return;
  ru_mutex_lock(s->mu);
  s->stop = true;
  ru_mutex_unlock(s->mu);
  wake(s);
  ru_task_join(s->task);
  s->task = NULL;
}

/* ---------------- connection test ---------------- */

ru_cfg_test_t dz_service_probe(dz_transport_t *t, void (*set_insecure)(dz_transport_t *, bool), const ru_cfg_t *cfg,
                               uint32_t timeout_ms, char *info, size_t info_cap) {
  char url[RU_CFG_URL_MAX + 64];
  char hdr[sizeof "Authorization: Bearer " + RU_CFG_TOKEN_MAX];
  char buf[1024];
  const char *hdrs[2] = {NULL, NULL};
  dz_http_result_t r = {0, 0};
  ru_cfg_test_t out;
  if (info && info_cap) info[0] = 0;
  if (!t || !cfg || ru_cfg_validate(cfg) != RU_CFG_OK) return RU_CFG_TEST_UNREACHABLE;
  if (set_insecure) set_insecure(t, cfg->insecure_lan);
  snprintf(url, sizeof url, "%s/json.htm?type=command&param=getversion", cfg->url);
  snprintf(hdr, sizeof hdr, "Authorization: Bearer %s", cfg->token);
  hdrs[0] = hdr;
  dz_net_err_t ne = t->get(t, url, hdrs, timeout_ms, buf, sizeof buf, &r);
  secure_zero(hdr, sizeof hdr);
  if (ne == DZ_NET_TIMEOUT || ne == DZ_NET_CONNECT) return RU_CFG_TEST_UNREACHABLE;
  if (ne == DZ_NET_TLS) return RU_CFG_TEST_TLS;
  if (r.status == 401 || r.status == 403) return RU_CFG_TEST_TOKEN_REJECTED;
  if (ne != DZ_NET_OK || r.status != 200) return RU_CFG_TEST_BAD_RESPONSE;
  dz_result_t res;
  if (dz_parse_response(buf, r.len, NULL, NULL, &res) != DZ_OK) return RU_CFG_TEST_BAD_RESPONSE;
  out = RU_CFG_TEST_OK;
  dz_version_t v;
  char ver[16] = "";
  if (dz_version_parse(res.version, &v) && v.valid) snprintf(ver, sizeof ver, "%d.%d", v.year, v.minor);
  /* Control goes through the Domoticz MCP endpoint: say whether it answers an initialize for this token. The session is left
   * to idle out on the server (the notification is skipped); its id is wiped here. */
  bool mcp = false;
  if (t->post) {
    char body[256], sid[65], auth[sizeof "Authorization: Bearer " + RU_CFG_TOKEN_MAX];
    const char *ph[3] = {auth, "Accept: application/json, text/event-stream", NULL};
    size_t bl = dz_mcp_init_body(body, sizeof body, 1);
    snprintf(url, sizeof url, "%s/mcp", cfg->url);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", cfg->token);
    dz_post_t p = {body, bl, "application/json", "Mcp-Session-Id", sid, sizeof sid};
    sid[0] = 0;
    dz_http_result_t pr = {0, 0};
    dz_net_err_t pe = bl ? t->post(t, url, ph, &p, timeout_ms, buf, sizeof buf, &pr) : DZ_NET_CONNECT;
    dz_mcp_reply_t mr;
    mcp = pe == DZ_NET_OK && pr.status == 200 && sid[0] && dz_mcp_parse_reply(buf, pr.len, &mr) == DZ_OK && mr.has_result && !mr.has_error;
    secure_zero(auth, sizeof auth);
    secure_zero(sid, sizeof sid);
  }
  if (info && info_cap) snprintf(info, info_cap, "%s%scontrol MCP %s", ver, ver[0] ? " " : "", mcp ? "ok" : "unavailable");
  return out;
}

ru_cfg_test_t dz_service_cfgline_test(void *user, const ru_cfg_t *cfg, uint32_t timeout_ms, char *info, size_t info_cap) {
  dz_probe_ctx_t *c = user;
  if (info && info_cap) info[0] = 0;
  if (!c) return RU_CFG_TEST_UNREACHABLE;
  if (c->wifi_up && !c->wifi_up(c->user)) return RU_CFG_TEST_NO_WIFI;
  return dz_service_probe(c->transport, c->set_insecure, cfg, timeout_ms, info, info_cap);
}
