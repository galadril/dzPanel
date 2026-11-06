/* Dev tool, not a unit test. Reads a live Domoticz through the curl transport and compares it with
 * the parser; optionally proves the MCP control path on one test dimmer (--mcp).
 *
 *   DOMOTICZ_ACCESSTOKEN=... ./dz_live_check https://host [--mcp | --tabs | --lights]
 *
 * --mcp: the control path through the Domoticz MCP endpoint, via the firmware's own code (dz_cmd: pre-flight read, initialize,
 * tools/call, verify read). Touches ONLY device idx 907 (a test dimmer): On, Off, level 30, then restores the level and state
 * it found. Prints statuses and fixed words only. Use it against a server and token you may switch things on.
 *
 * --tabs: compares the server's filter=light|temp|weather|utility membership with dz_tabs_of() and prints a diff
 * report (by Type / SubType and direction only; no idx, name or URL credentials).
 *
 * --lights: aggregate counts of the filter=light set by Type / SubType / SwitchType / dimmer / protected / favourite,
 * with the firmware capability and whether dz_tabs_of puts it on the Lights page (no idx, no names).
 *
 * The token comes from the environment and is never printed. Only counts and capability names are
 * printed: no device names, idx values, coordinates or raw bodies. At least 5 s between requests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dz_client.h"
#include "dz_cmd.h"
#include "ru_osal.h"

#define CAP 400
static dz_entry_t ent[CAP];
static dz_device_t stg[CAP];
static dz_model_t M;
static char buf[600 * 1024];
static dz_client_t C;
static dz_cmd_t K;
static int done, outcome = -1, failkind;

static void on_res(const dz_cmd_result_t *r, void *u) {
  (void)u;
  done = 1;
  outcome = (int)r->outcome;
  failkind = (int)r->fail;
}

static void gap(void) { ru_sleep_ms(5200); }

typedef struct { int n, by_cap[16], fav, prot, timeout; } hist_t;
static bool hist_cb(const dz_device_t *d, void *u) {
  hist_t *h = u;
  h->n++;
  if ((unsigned)d->cap < 16) h->by_cap[d->cap]++;
  h->fav += d->favorite;
  h->prot += d->is_protected;
  h->timeout += d->timeout;
  return true;
}

static int req(const char *q, dz_http_result_t *r) {
  dz_net_err_t e = dz_client_request(&C, q, r);
  printf("  GET %-22s -> net=%s http=%d bytes=%zu\n", strncmp(q, "param=getdevices", 16) == 0 ? "param=getdevices..." : q, dz_net_err_name(e),
         r->status, r->len);
  return e == DZ_NET_OK && r->status == 200;
}

/* ---- --tabs: server filter membership versus dz_tabs_of ---- */
typedef struct { char idx[12]; char type[24], sub[24]; unsigned tabs; bool scene; } tdev_t;
typedef struct { tdev_t *v; int n, cap; } tlist_t;
static bool tab_cb(const dz_device_t *d, void *u) {
  tlist_t *l = u;
  if (l->n >= l->cap) return false;
  tdev_t *t = &l->v[l->n++];
  snprintf(t->idx, sizeof t->idx, "%s", d->idx);
  snprintf(t->type, sizeof t->type, "%s", d->type);
  snprintf(t->sub, sizeof t->sub, "%s", d->subtype);
  t->tabs = d->tabs;
  t->scene = dz_cap_is_scene_ns(d->cap);
  return true;
}

static int tabs_report(void) {
  static const struct { const char *name; unsigned bit; } F[] = {
    { "light", DZ_TAB_LIGHT }, { "temp", DZ_TAB_TEMP }, { "weather", DZ_TAB_WEATHER }, { "utility", DZ_TAB_UTILITY },
  };
  static tdev_t allv[CAP], partv[CAP];
  tlist_t all = { allv, 0, CAP }, part = { partv, 0, CAP };
  dz_http_result_t r;
  dz_result_t res;
  puts("tabs: getdevices filter=all");
  if (!req("param=getdevices&filter=all&used=true&order=Name", &r)) return 1;
  if (dz_parse_response(buf, r.len, tab_cb, &all, &res) != DZ_OK) return 1;
  int devs = 0, none = 0, multi = 0, diffs = 0;
  for (int i = 0; i < all.n; i++) {
    if (all.v[i].scene) continue;
    devs++;
    unsigned t = all.v[i].tabs;
    none += t == 0;
    multi += (t & (t - 1)) != 0;
  }
  printf("  devices=%d on-no-tab=%d on-several-tabs=%d\n", devs, none, multi);
  for (size_t f = 0; f < 4; f++) {
    char q[96];
    gap();
    snprintf(q, sizeof q, "param=getdevices&filter=%s&used=true&order=Name", F[f].name);
    if (!req(q, &r)) return 1;
    part.n = 0;
    if (dz_parse_response(buf, r.len, tab_cb, &part, &res) != DZ_OK) return 1;
    int local = 0, server = 0, bad = 0;
    for (int i = 0; i < all.n; i++) {
      if (all.v[i].scene) continue;
      bool in_local = (all.v[i].tabs & F[f].bit) != 0, in_server = false;
      for (int k = 0; k < part.n && !in_server; k++) in_server = !part.v[k].scene && !strcmp(part.v[k].idx, all.v[i].idx);
      local += in_local;
      server += in_server;
      if (in_local != in_server) {
        bad++;
        printf("    DIFF %-8s %-24s / %-24s local=%s server=%s\n", F[f].name, all.v[i].type, all.v[i].sub, in_local ? "yes" : "no", in_server ? "yes" : "no");
      }
    }
    printf("  filter=%-8s server=%d local=%d differences=%d\n", F[f].name, server, local, bad);
    diffs += bad;
  }
  printf("tabs: %s\n", diffs ? "DIFFERENCES FOUND" : "all four filters agree with dz_tabs_of");
  return diffs ? 1 : 0;
}

/* ---- --lights: what the Switches tab holds, by class ---- */
typedef struct { char type[24], sub[24]; int stv, dim, prot, fav, cap, onpage, n; } lrow_t;
typedef struct { lrow_t r[96]; int n, over; } lagg_t;
static bool light_cb(const dz_device_t *d, void *u) {
  lagg_t *g = u;
  lrow_t k;
  memset(&k, 0, sizeof k);
  snprintf(k.type, sizeof k.type, "%s", d->type);
  snprintf(k.sub, sizeof k.sub, "%s", d->subtype);
  k.stv = d->switch_type; k.dim = d->has_level; k.prot = d->is_protected; k.fav = d->favorite; k.cap = (int)d->cap;
  k.onpage = (d->tabs & DZ_TAB_LIGHT) != 0;
  for (int i = 0; i < g->n; i++) {
    lrow_t *o = &g->r[i];
    if (!strcmp(o->type, k.type) && !strcmp(o->sub, k.sub) && o->stv == k.stv && o->dim == k.dim && o->prot == k.prot &&
        o->fav == k.fav && o->cap == k.cap && o->onpage == k.onpage) { o->n++; return true; }
  }
  if (g->n >= 96) { g->over++; return true; }
  k.n = 1;
  g->r[g->n++] = k;
  return true;
}

static int lights_report(void) {
  static lagg_t g;
  dz_http_result_t r;
  dz_result_t res;
  puts("lights: getdevices filter=light");
  if (!req("param=getdevices&filter=light&used=true&order=Name", &r)) return 1;
  if (dz_parse_response(buf, r.len, light_cb, &g, &res) != DZ_OK) return 1;
  int tot = 0, on = 0;
  for (int i = 0; i < g.n; i++) {
    lrow_t *o = &g.r[i];
    tot += o->n;
    on += o->onpage ? o->n : 0;
    printf("  n=%-3d type=%-14s sub=%-18s switchtype=%-3d level=%d protected=%d fav=%d cap=%-9s lights-page=%s\n", o->n, o->type, o->sub, o->stv,
           o->dim, o->prot, o->fav, dz_cap_name((dz_cap_t)o->cap), o->onpage ? "yes" : "NO");
  }
  printf("lights: total=%d (parser devices=%d rejected=%d) on-lights-page=%d classes-over-limit=%d\n", tot, res.devices, res.rejected, on, g.over);
  return tot == on ? 0 : 1;
}

/* ---- --mcp: On, Off, level 30 and a restore on idx 907 only ---- */
#define MCP_IDX "907"
static bool run_cmd(dz_cmd_kind_t kind, int level, const char *what) {
  ru_sleep_ms(1500);
  done = 0;
  outcome = -1;
  failkind = 0;
  dz_cmd_req_t q = {.kind = kind, .idx = MCP_IDX, .level = level, .confirmed = true};
  dz_cmd_err_t e = dz_cmd_submit(&K, &q, ru_now_ms(), NULL);
  if (e != DZ_CMD_ACCEPTED) { printf("  %-14s refused: %s\n", what, dz_cmd_err_name(e)); return false; }
  for (int i = 0; !done && i < 200; i++) {
    uint32_t w = dz_cmd_ms_until_due(&K, ru_now_ms());
    if (w && w != UINT32_MAX) ru_sleep_ms(w);
    dz_cmd_pump(&K, ru_now_ms());
  }
  printf("  %-14s result: %s (fail=%d) mcp session %s, %u requests\n", what, dz_outcome_name((dz_outcome_t)outcome), failkind,
         C.mcp_sid[0] ? "held" : "none", C.requests);
  return outcome == DZ_OUT_CONFIRMED;
}

static bool read907(dz_device_t *out) {
  dz_http_result_t r;
  ru_sleep_ms(1500);
  if (!req("param=getdevices&rid=" MCP_IDX, &r)) return false;
  int changed = 0;
  if (dz_model_apply(&M, buf, r.len, DZ_COMMIT_PARTIAL, ru_now_ms(), NULL, &changed) != DZ_OK) return false;
  return dz_model_get(&M, MCP_IDX, false, out, NULL, NULL, ru_now_ms());
}

static int mcp_check(void) {
  int rc = 0;
  dz_device_t d0, d1;
  puts("4. MCP control on one test dimmer (idx 907 only)");
  if (!read907(&d0)) { puts("  device 907 not readable: refusing"); return 1; }
  bool was_on = dz_status_is_on(&d0);
  int lvl0 = d0.has_level ? d0.level : -1;
  printf("  start: status '%s' level %d cap %s controllable %d protected %d\n", d0.status, lvl0, dz_cap_name(d0.cap), d0.controllable, d0.is_protected);
  if (d0.cap != DZ_CAP_DIMMER || !d0.controllable || d0.is_protected) { puts("  not a controllable dimmer: refusing"); return 1; }
  dz_cmd_init(&K, &C, &M, true, on_res, NULL);
  K.verify_delay_ms = 2500;
  rc |= !run_cmd(DZ_CMD_ON, -1, "set On");
  rc |= !run_cmd(DZ_CMD_OFF, -1, "set Off");
  rc |= !run_cmd(DZ_CMD_SET_LEVEL, 30, "level 30");
  /* restore: the level it had (when known), then Off if it was Off */
  if (lvl0 > 0) rc |= !run_cmd(DZ_CMD_SET_LEVEL, lvl0, "restore level");
  if (!was_on) rc |= !run_cmd(DZ_CMD_OFF, -1, "restore Off");
  if (!read907(&d1)) { puts("  final read failed"); return 1; }
  printf("  final: status '%s' level %d (started '%s' level %d)\n", d1.status, d1.has_level ? d1.level : -1, d0.status, lvl0);
  if (dz_status_is_on(&d1) != was_on) { puts("  WARNING: state differs from the start"); rc = 1; }
  return rc;
}

int main(int argc, char **argv) {
  const char *url = argc > 1 ? argv[1] : "https://domoticz.hnogames.nl";
  bool mcp = argc > 2 && !strcmp(argv[2], "--mcp");
  bool tabs = argc > 2 && !strcmp(argv[2], "--tabs");
  bool lights = argc > 2 && !strcmp(argv[2], "--lights");
  char token[DZ_TOKEN_MAX];
  if (!dz_client_token_from_env(token, sizeof token)) { puts("DOMOTICZ_ACCESSTOKEN not set"); return 2; }
  dz_transport_t *tr = dz_transport_curl_create();
  dz_model_cfg_t mc = {.ttl_ms = 600000, .old_update_s = UINT32_MAX};
  dz_model_init(&M, ent, CAP, stg, CAP, &mc);
  dz_client_cfg_t cfg = {.base_url = url, .token = token, .transport = tr, .model = &M, .buf = buf, .buf_cap = sizeof buf,
                         .timeout_ms = 20000, .poll_interval_ms = 5200, .startup_gap_ms = 5200};
  bool okc = dz_client_init(&C, &cfg);
  memset(token, 0, sizeof token);
  if (!okc) { puts("bad config"); return 2; }
  int rc = 0;
  dz_http_result_t r;

  puts("1. getversion");
  if (!req("param=getversion", &r)) return 1;
  dz_result_t res;
  dz_err_t pe = dz_parse_response(buf, r.len, NULL, NULL, &res);
  dz_version_t v;
  char vt[48] = "";
  const char *vp = strstr(buf, "\"version\"");
  if (vp && (vp = strchr(vp + 9, '"'))) { size_t k = strcspn(vp + 1, "\""); if (k < sizeof vt) { memcpy(vt, vp + 1, k); vt[k] = 0; } }
  dz_version_parse(vt, &v);
  printf("  parse=%d version=%d.%d build=%d valid=%d\n", pe, v.year, v.minor, v.build, v.valid);
  if (pe != DZ_OK || !v.valid) rc = 1;

  if (mcp) {
    rc = mcp_check() || rc;
    printf("requests made: %u\n", C.requests);
    dz_model_deinit(&M);
    dz_transport_destroy(tr);
    return rc;
  }

  if (lights) {
    gap();
    rc = lights_report() || rc;
    dz_model_deinit(&M);
    dz_transport_destroy(tr);
    return rc;
  }

  if (tabs) {
    gap();
    rc = tabs_report() || rc;
    dz_model_deinit(&M);
    dz_transport_destroy(tr);
    return rc;
  }

  gap();
  puts("2. getdevices (full, all used)");
  if (!req("param=getdevices&filter=all&used=true&order=Name", &r)) return 1;
  static hist_t h;
  memset(&h, 0, sizeof h);
  pe = dz_parse_response(buf, r.len, hist_cb, &h, &res);
  printf("  parse=%d devices=%d rejected=%d favourites=%d protected=%d timeout=%d\n", pe, res.devices, res.rejected, h.fav, h.prot, h.timeout);
  for (int i = 0; i < 16; i++) if (h.by_cap[i]) printf("    cap %-10s %d\n", dz_cap_name((dz_cap_t)i), h.by_cap[i]);
  int changed = 0;
  pe = dz_model_apply(&M, buf, r.len, DZ_COMMIT_FULL_DEVICES, ru_now_ms(), &res, &changed);
  printf("  model count=%zu changed=%d overflow=%u (parse devices=%d)\n", dz_model_count(&M), changed, dz_model_overflow(&M), res.devices);
  /* groups come back under scene idx; the client keeps them out of the device namespace but this raw apply keeps them */
  if (pe != DZ_OK || (int)dz_model_count(&M) + (int)dz_model_overflow(&M) != res.devices) rc = 1;
  size_t stale = 0, unknown = 0;
  for (size_t i = 0; i < dz_model_count(&M); i++) {
    ru_val_state_t st;
    dz_model_at(&M, i, NULL, NULL, &st, NULL, ru_now_ms());
    stale += st == RU_VAL_STALE;
    unknown += st == RU_VAL_UNKNOWN;
  }
  printf("  freshness: stale=%zu unknown=%zu\n", stale, unknown);
  char actt[24] = "";
  const char *ap = strstr(buf, "\"ActTime\"");
  if (ap && (ap = strchr(ap + 9, ':'))) { ap++; while (*ap == ' ' || *ap == '\t') ap++; size_t k = strspn(ap, "0123456789"); if (k < sizeof actt) { memcpy(actt, ap, k); actt[k] = 0; } }
  printf("  ActTime present: %s\n", actt[0] ? "yes" : "NO");

  if (actt[0]) {
    gap();
    puts("3. delta with ActTime cursor");
    char q[160];
    snprintf(q, sizeof q, "param=getdevices&filter=all&used=true&order=Name&lastupdate=%lld", atoll(actt) - 2);
    if (!req(q, &r)) return 1;
    memset(&h, 0, sizeof h);
    pe = dz_parse_response(buf, r.len, hist_cb, &h, &res);
    printf("  parse=%d delta devices=%d (full had %zu)\n", pe, res.devices, dz_model_count(&M));
    if (pe != DZ_OK) rc = 1;
  }

  printf("requests made: %u, state=%s\n", C.requests, dz_conn_state_name(dz_client_state(&C)));
  dz_model_deinit(&M);
  dz_transport_destroy(tr);
  return rc;
}
