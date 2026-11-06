/* Dev tool, not a unit test. Reads a live Domoticz through the curl transport and compares it with
 * the parser; optionally proves the MCP control path on one test dimmer (--mcp).
 *
 *   DOMOTICZ_ACCESSTOKEN=... ./dz_live_check --url https://host [--token-env NAME] [--tabs | --lights]
 *   DOMOTICZ_ACCESSTOKEN=... ./dz_live_check --url https://host --idx N --allow-write --mcp
 *
 * --url URL        required, there is no default server.
 * --token-env NAME the environment variable that holds the access token (default DOMOTICZ_ACCESSTOKEN). The token is never taken
 *                  from argv or a file and never printed.
 * --idx N          the one device the write tests may touch (digits only, at most 9).
 * --allow-write    without it no write is ever sent; --mcp and --push-test refuse to run.
 *
 * --mcp: the control path through the Domoticz MCP endpoint, via the firmware's own code (dz_cmd: pre-flight read, initialize,
 * tools/call, verify read). Touches ONLY the device given by --idx (a test dimmer): On, Off, level 30, then restores the state
 * (and the level, when it was known). Prints statuses and fixed words only (no device text or names).
 *
 * --push-test [--push-mode set|toggle|offon]: the --idx device must be a push button. Presses it twice through the firmware's own
 * command path, 3 s apart, and prints the outcome of each press as fixed words (confirmed / unconfirmed / failed / no-action), the
 * failure class, and whether the call was answered, answered "No action taken", or was uncertain. --push-mode picks the MCP call
 * that fires the push: set (set_switch_state with the type's word, the default), toggle (toggle_switch_state) or offon (the opposite
 * word, then the type's word). Needs --allow-write.
 *
 * --color-test [--hue-test]: the --idx device must be a tunable-white or colour lamp (Color Switch). Through the firmware's own command
 * path: set_color_temperature at warmth 50 (kelvin 4600) when the lamp can do it, a dimmer level change (the colour must be kept), and with
 * --hue-test also set_color_brightness with hue 120 (green). After every step the lamp is read back and printed as fixed words and numbers
 * only (on/off, level, Color mode m, t, the hue computed from RGB, whether the lamp switched on). The original on/off state, level and
 * colour mode (temperature t, or with --hue-test also the hue and a white) are restored; a colour that cannot be restored without
 * --hue-test is said so. Needs --allow-write. Prints no names, status text or token.
 *
 * --tabs: compares the server's filter=light|temp|weather|utility membership with dz_tabs_of() and prints a diff
 * report (by Type / SubType and direction only; no idx, name or URL credentials).
 *
 * --lights: aggregate counts of the filter=light set by Type / SubType / SwitchType / dimmer / protected / favourite,
 * with the firmware capability and whether dz_tabs_of puts it on the Lights page (no idx, no names).
 *
 * Only counts and capability names are printed: no device names, idx values, coordinates or raw bodies.
 * At least 5 s between requests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dz_client.h"
#include "dz_cmd.h"
#include "dz_color.h"
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

/* ---- --mcp: On, Off, level 30 and a restore on the --idx device only ---- */
static char g_idx[12];
static int g_hue;
static bool run_cmd(dz_cmd_kind_t kind, int level, const char *what) {
  ru_sleep_ms(1500);
  done = 0;
  outcome = -1;
  failkind = 0;
  dz_cmd_req_t q = {.kind = kind, .idx = g_idx, .level = level, .hue = (int16_t)g_hue, .confirmed = true};
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

static bool read_dev(dz_device_t *out) {
  dz_http_result_t r;
  char q[48];
  ru_sleep_ms(1500);
  snprintf(q, sizeof q, "param=getdevices&rid=%s", g_idx);
  if (!req(q, &r)) return false;
  int changed = 0;
  if (dz_model_apply(&M, buf, r.len, DZ_COMMIT_PARTIAL, ru_now_ms(), NULL, &changed) != DZ_OK) return false;
  return dz_model_get(&M, g_idx, false, out, NULL, NULL, ru_now_ms());
}

/* a fixed-word classification, never the device's own status text */
static void state_word(const dz_device_t *d, char *o, size_t cap) {
  if (!dz_status_is_on(d)) snprintf(o, cap, "off");
  else if (d->has_level) snprintf(o, cap, "on level %d", d->level);
  else snprintf(o, cap, "on");
}

static bool last_sent, last_noaction, last_uncertain;
static void on_res_push(const dz_cmd_result_t *r, void *u) {
  on_res(r, u);
  last_sent = r->sent;
  last_noaction = r->no_action;
  last_uncertain = r->uncertain;
}

static int push_test(dz_push_mode_t mode) {
  dz_device_t d0;
  int first = -1, second = -1;
  puts("5. push test on the --idx device (two presses, 3 s apart)");
  if (!read_dev(&d0)) { puts("  device not readable: refusing"); return 1; }
  printf("  start: cap %s controllable %d protected %d switchtype %d\n", dz_cap_name(d0.cap), d0.controllable, d0.is_protected, d0.switch_type);
  if (d0.cap != DZ_CAP_PUSH || !d0.controllable || d0.is_protected) { puts("  not a controllable push button: refusing"); return 1; }
  dz_cmd_init(&K, &C, &M, true, on_res_push, NULL);
  K.verify_delay_ms = 2500;
  K.push_mode = mode;
  for (int i = 0; i < 2; i++) {
    if (i) ru_sleep_ms(3000);
    char what[16];
    snprintf(what, sizeof what, "press %d", i + 1);
    (void)run_cmd(DZ_CMD_PUSH, -1, what);
    printf("  %s: outcome=%s fail=%d sent=%d no-action=%d uncertain=%d\n", what, dz_outcome_name((dz_outcome_t)outcome), failkind, last_sent,
           last_noaction, last_uncertain);
    if (i == 0) first = outcome; else second = outcome;
  }
  printf("push-test: first=%s second=%s\n", dz_outcome_name((dz_outcome_t)first), dz_outcome_name((dz_outcome_t)second));
  return first == DZ_OUT_CONFIRMED && second == DZ_OUT_CONFIRMED ? 0 : 1;
}

static int mcp_check(void) {
  int rc = 0;
  dz_device_t d0, d1;
  char w0[24], w1[24];
  puts("4. MCP control on one test dimmer (the --idx device only)");
  if (!read_dev(&d0)) { puts("  device not readable: refusing"); return 1; }
  bool was_on = dz_status_is_on(&d0);
  int lvl0 = d0.has_level ? d0.level : -1;
  state_word(&d0, w0, sizeof w0);
  printf("  start: %s cap %s controllable %d protected %d\n", w0, dz_cap_name(d0.cap), d0.controllable, d0.is_protected);
  if (d0.cap != DZ_CAP_DIMMER || !d0.controllable || d0.is_protected) { puts("  not a controllable dimmer: refusing"); return 1; }
  dz_cmd_init(&K, &C, &M, true, on_res, NULL);
  K.verify_delay_ms = 2500;
  rc |= !run_cmd(DZ_CMD_ON, -1, "set On");
  rc |= !run_cmd(DZ_CMD_OFF, -1, "set Off");
  rc |= !run_cmd(DZ_CMD_SET_LEVEL, 30, "level 30");
  /* restore: when it was on, the level it had (or plain On when the level was unknown); when it was off, Off */
  bool level_lost = false;
  if (was_on) {
    if (lvl0 > 0) rc |= !run_cmd(DZ_CMD_SET_LEVEL, lvl0, "restore level");
    else { rc |= !run_cmd(DZ_CMD_ON, -1, "restore On"); level_lost = true; }
  } else {
    rc |= !run_cmd(DZ_CMD_OFF, -1, "restore Off");
  }
  if (!read_dev(&d1)) { puts("  final read failed: the original state could NOT be verified"); return 1; }
  state_word(&d1, w1, sizeof w1);
  printf("  final: %s (started %s)\n", w1, w0);
  if (dz_status_is_on(&d1) != was_on) { puts("  WARNING: the original on/off state could NOT be restored"); rc = 1; }
  if (level_lost) puts("  NOTE: the original level was unknown, so the level could not be restored (on/off state only)");
  else if (was_on && lvl0 > 0 && (!d1.has_level || d1.level != lvl0)) { puts("  WARNING: the original level could NOT be restored"); rc = 1; }
  return rc;
}


/* the colour state of a lamp as fixed words and numbers */
static void colour_word(const dz_device_t *d) {
  float h = 0, sat = 0;
  bool has_hue = d->has_color && d->col_m == 3 && dz_rgb_to_hue(d->col_r, d->col_g, d->col_b, &h, &sat);
  printf("    lamp: %s level=%d mode m=%d t=%d hue=%d\n", dz_status_is_on(d) ? "on" : "off", d->has_level ? d->level : -1, d->has_color ? d->col_m : -1,
         d->has_color ? d->col_t : -1, has_hue ? (int)(h + 0.5f) : -1);
}

static int color_test(bool hue_test) {
  int rc = 0;
  dz_device_t d0, d1;
  puts("6. colour test on the --idx device (temperature, level, optionally hue)");
  if (!read_dev(&d0)) { puts("  device not readable: refusing"); return 1; }
  printf("  start: cap %s controllable %d protected %d colour-caps=%d has-colour=%d\n", dz_cap_name(d0.cap), d0.controllable, d0.is_protected,
         d0.col_caps, d0.has_color);
  colour_word(&d0);
  if (d0.cap != DZ_CAP_DIMMER || !d0.controllable || d0.is_protected || !d0.col_caps) { puts("  not a controllable colour or white lamp: refusing"); return 1; }
  bool was_on = dz_status_is_on(&d0);
  int lvl0 = d0.has_level ? d0.level : -1, m0 = d0.has_color ? d0.col_m : -1, t0 = d0.col_t;
  float hue0 = 0, sat0 = 0;
  bool hue0_ok = d0.has_color && m0 == 3 && dz_rgb_to_hue(d0.col_r, d0.col_g, d0.col_b, &hue0, &sat0);
  dz_cmd_init(&K, &C, &M, true, on_res, NULL);
  K.verify_delay_ms = 2500;
  dz_device_t d;
  if (d0.col_caps & DZ_COLCAP_CT) {
    puts("  a. temperature: warmth 50 (kelvin 4600)");
    rc |= !run_cmd(DZ_CMD_COLOR_TEMP, 50, "temp 50");
    if (read_dev(&d)) {
      colour_word(&d);
      printf("    expected t=%d; lamp %s after the command (it was %s)\n", dz_warmth_to_t(50), dz_status_is_on(&d) ? "on" : "off", was_on ? "on" : "off");
    }
  } else puts("  a. temperature: skipped (the lamp has no colour-temperature capability)");
  puts("  b. brightness change: the colour must be kept");
  int probe = lvl0 == 30 ? 55 : 30;
  rc |= !run_cmd(DZ_CMD_SET_LEVEL, probe, "level change");
  if (read_dev(&d)) {
    colour_word(&d);
    printf("    colour kept after the level change: %s\n", d.has_color && d0.has_color && d.col_m == ((d0.col_caps & DZ_COLCAP_CT) ? 2 : m0) ? "yes" : "NO");
  }
  if (hue_test) {
    if (d0.col_caps & DZ_COLCAP_RGB) {
      puts("  c. hue 120 (green) at the current brightness");
      g_hue = 120;
      rc |= !run_cmd(DZ_CMD_COLOR_HUE, probe, "hue 120");
      if (read_dev(&d)) colour_word(&d);
    } else puts("  c. hue: skipped (the lamp has no RGB capability)");
  }
  /* restore: colour mode first, then the level, then on/off */
  puts("  restore");
  if (m0 == 2 && (d0.col_caps & DZ_COLCAP_CT)) rc |= !run_cmd(DZ_CMD_COLOR_TEMP, dz_t_to_warmth(t0), "restore temp");
  else if (m0 == 3 && hue0_ok && hue_test) { g_hue = (int)(hue0 + 0.5f) % 360; rc |= !run_cmd(DZ_CMD_COLOR_HUE, lvl0 > 0 ? lvl0 : 100, "restore hue"); }
  else if (m0 == 1 && hue_test && (d0.col_caps & DZ_COLCAP_W)) rc |= !run_cmd(DZ_CMD_COLOR_WHITE, lvl0 > 0 ? lvl0 : 100, "restore white");
  else if (m0 == 3 || m0 == 1) puts("  NOTE: the original colour mode was not changed by this test (no --hue-test)");
  if (was_on) {
    if (lvl0 > 0) rc |= !run_cmd(DZ_CMD_SET_LEVEL, lvl0, "restore level");
    else rc |= !run_cmd(DZ_CMD_ON, -1, "restore On");
  } else {
    if (lvl0 > 0 && read_dev(&d) && dz_status_is_on(&d)) rc |= !run_cmd(DZ_CMD_SET_LEVEL, lvl0, "restore level");
    rc |= !run_cmd(DZ_CMD_OFF, -1, "restore Off");
  }
  if (!read_dev(&d1)) { puts("  final read failed: the original state could NOT be verified"); return 1; }
  puts("  final:");
  colour_word(&d1);
  if (dz_status_is_on(&d1) != was_on) { puts("  WARNING: the original on/off state could NOT be restored"); rc = 1; }
  if (was_on && lvl0 > 0 && (!d1.has_level || d1.level != lvl0)) { puts("  WARNING: the original level could NOT be restored"); rc = 1; }
  if (m0 == 2 && d1.has_color && d1.col_m == 2 && abs((int)d1.col_t - t0) > 3) { puts("  WARNING: the original colour temperature could NOT be restored"); rc = 1; }
  printf("color-test: %s\n", rc ? "problems" : "ok");
  return rc;
}

static bool idx_ok(const char *v) {
  size_t n = strlen(v);
  if (n == 0 || n > 9) return false;
  for (size_t i = 0; i < n; i++) if (v[i] < '0' || v[i] > '9') return false;
  return true;
}

static void usage(void) {
  puts("usage: dz_live_check --url URL [--token-env NAME] [--idx N --allow-write (--mcp | --push-test [--push-mode set|toggle|offon] | --color-test [--hue-test])] [--tabs | --lights]");
}

int main(int argc, char **argv) {
  const char *url = NULL, *tenv = "DOMOTICZ_ACCESSTOKEN";
  bool mcp = false, tabs = false, lights = false, allow_write = false, push = false, ctest = false, hue_test = false;
  dz_push_mode_t pmode = DZ_PUSH_MODE_DEFAULT;
  g_idx[0] = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--url") && i + 1 < argc) url = argv[++i];
    else if (!strcmp(a, "--token-env") && i + 1 < argc) tenv = argv[++i];
    else if (!strcmp(a, "--idx") && i + 1 < argc) {
      const char *v = argv[++i];
      if (!idx_ok(v)) { puts("--idx must be 1 to 9 digits"); return 2; }
      snprintf(g_idx, sizeof g_idx, "%s", v);
    }
    else if (!strcmp(a, "--allow-write")) allow_write = true;
    else if (!strcmp(a, "--mcp")) mcp = true;
    else if (!strcmp(a, "--push-test")) push = true;
    else if (!strcmp(a, "--color-test")) ctest = true;
    else if (!strcmp(a, "--hue-test")) hue_test = true;
    else if (!strcmp(a, "--push-mode") && i + 1 < argc) {
      const char *v = argv[++i];
      if (!strcmp(v, "set")) pmode = DZ_PUSH_SET_STATE;
      else if (!strcmp(v, "toggle")) pmode = DZ_PUSH_TOGGLE;
      else if (!strcmp(v, "offon")) pmode = DZ_PUSH_OFF_THEN_ON;
      else { puts("--push-mode: set, toggle or offon"); return 2; }
    }
    else if (!strcmp(a, "--tabs")) tabs = true;
    else if (!strcmp(a, "--lights")) lights = true;
    else { usage(); return 2; }
  }
  if (!url) { puts("--url is required"); usage(); return 2; }
  if (mcp && !allow_write) { puts("--mcp writes to the device: refusing without --allow-write"); return 2; }
  if (mcp && !g_idx[0]) { puts("--mcp needs --idx N"); return 2; }
  if (push && !allow_write) { puts("--push-test writes to the device: refusing without --allow-write"); return 2; }
  if (push && !g_idx[0]) { puts("--push-test needs --idx N"); return 2; }
  if (ctest && !allow_write) { puts("--color-test writes to the device: refusing without --allow-write"); return 2; }
  if (ctest && !g_idx[0]) { puts("--color-test needs --idx N"); return 2; }
  if (hue_test && !ctest) { puts("--hue-test only goes with --color-test"); return 2; }
  char token[DZ_TOKEN_MAX];
  const char *tv = getenv(tenv);
  if (!tv || !*tv || strlen(tv) >= sizeof token) { puts("token environment variable not set"); return 2; }
  memcpy(token, tv, strlen(tv) + 1);
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

  if (push) {
    rc = push_test(pmode) || rc;
    printf("requests made: %u\n", C.requests);
    dz_model_deinit(&M);
    dz_transport_destroy(tr);
    return rc;
  }

  if (ctest) {
    rc = color_test(hue_test) || rc;
    printf("requests made: %u\n", C.requests);
    dz_model_deinit(&M);
    dz_transport_destroy(tr);
    return rc;
  }

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
