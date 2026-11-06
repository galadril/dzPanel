#include "apps_demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ru_gesture.h"
#include "ru_nav.h"
#include "ru_theme.h"

static char *slurp(const char *path, size_t *n) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  fseek(fp, 0, SEEK_END);
  long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  char *b = malloc((size_t)len + 1);
  if (b && fread(b, 1, (size_t)len, fp) != (size_t)len) { free(b); b = NULL; }
  if (b) { b[len] = 0; *n = (size_t)len; }
  fclose(fp);
  return b;
}

static bool replace_once(char **buf, size_t *n, const char *from, const char *to) {
  char *p = strstr(*buf, from);
  if (!p) return false;
  size_t fl = strlen(from), tl = strlen(to);
  char *out = malloc(*n + tl + 1);
  memcpy(out, *buf, (size_t)(p - *buf));
  memcpy(out + (p - *buf), to, tl);
  strcpy(out + (p - *buf) + tl, p + fl);
  free(*buf);
  *buf = out;
  *n = *n - fl + tl;
  return true;
}

/* NOUNITS only: an install that reports no unit anywhere (no getconfig, and the devices carry no vunit either). */
static bool g_strip_vunit, g_strip_fav;

/* Load a fixture file, apply up to two textual Status tweaks (test data only), commit it to the model. */
static bool apply_file(appfx_t *f, const char *dir, const char *name, dz_commit_t mode, const char *from1, const char *to1,
                       const char *from2, const char *to2) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  size_t n = 0;
  char *b = slurp(path, &n);
  if (!b) return false;
  bool ok = true;
  if (from1) ok = replace_once(&b, &n, from1, to1);
  if (ok && from2) ok = replace_once(&b, &n, from2, to2);
  while (ok && g_strip_vunit && replace_once(&b, &n, "\"vunit\"", "\"xunit\"")) {}
  while (ok && g_strip_fav && replace_once(&b, &n, "\"Favorite\":1", "\"Favorite\":0")) {}
  int changed = 0;
  ok = ok && dz_model_apply(&f->model, b, n, mode, f->T, NULL, &changed) == DZ_OK;
  free(b);
  return ok;
}

bool appfx_apply_scenes(appfx_t *f, const char *json) {
  return dz_model_apply(&f->model, json, strlen(json), DZ_COMMIT_FULL_SCENES, f->T, NULL, NULL) == DZ_OK;
}

bool appfx_load(appfx_t *f, const char *dir, appfx_set_t set, uint32_t ttl_ms) {
  memset(f, 0, sizeof *f);
  f->ent = calloc(APPFX_CAP, sizeof *f->ent);
  f->stage = calloc(APPFX_CAP, sizeof *f->stage);
  f->rbuf = calloc(1, 4096);
  f->T = 100000;
  if (!f->ent || !f->stage || !f->rbuf) return false;
  dz_model_cfg_t mc = { .ttl_ms = ttl_ms, .old_update_s = UINT32_MAX };
  if (!dz_model_init(&f->model, f->ent, APPFX_CAP, f->stage, APPFX_CAP, &mc)) return false;
  f->tr = dz_transport_socket_create();
  static char base[] = "http://127.0.0.1:1";
  dz_client_cfg_t cfg = { .base_url = base, .token = "fixture-token-not-a-secret", .transport = f->tr, .model = &f->model,
                          .buf = f->rbuf, .buf_cap = 4096 };
  if (!f->tr || !dz_client_init(&f->client, &cfg)) return false;
  switch (set) {
    case APPFX_REAL:
      return apply_file(f, dir, "real_getdevices.json", DZ_COMMIT_FULL_DEVICES, NULL, NULL, NULL, NULL) &&
             apply_file(f, dir, "real_getscenes.json", DZ_COMMIT_FULL_SCENES, NULL, NULL, NULL, NULL);
    case APPFX_MOCK:
      return apply_file(f, dir, "mock_getdevices.json", DZ_COMMIT_FULL_DEVICES, NULL, NULL, NULL, NULL) &&
             apply_file(f, dir, "mock_getscenes.json", DZ_COMMIT_FULL_SCENES, NULL, NULL, NULL, NULL);
    case APPFX_MOCK_MIXED:
      /* the mock's two groups: "Away" On, "All Off" Mixed */
      return apply_file(f, dir, "mock_getdevices.json", DZ_COMMIT_FULL_DEVICES, NULL, NULL, NULL, NULL) &&
             apply_file(f, dir, "mock_getscenes.json", DZ_COMMIT_FULL_SCENES,
                        "\"Name\":\"Away\",\"Type\":\"Group\",\"Status\":\"Off\",\"Data\":\"Off\"",
                        "\"Name\":\"Away\",\"Type\":\"Group\",\"Status\":\"On\",\"Data\":\"On\"",
                        "\"Name\":\"All Off\",\"Type\":\"Group\",\"Status\":\"Off\",\"Data\":\"Off\"",
                        "\"Name\":\"All Off\",\"Type\":\"Group\",\"Status\":\"Mixed\",\"Data\":\"Mixed\"");
    case APPFX_CLIMATE_METRIC:
    case APPFX_CLIMATE_IMPERIAL:
    case APPFX_CLIMATE_NOUNITS: {
      bool imp = set == APPFX_CLIMATE_IMPERIAL;
      g_strip_vunit = set == APPFX_CLIMATE_NOUNITS;
      bool loaded = apply_file(f, dir, imp ? "mock_climate_imperial.json" : "mock_climate_metric.json", DZ_COMMIT_FULL_DEVICES, NULL, NULL, NULL, NULL);
      g_strip_vunit = false;
      if (!loaded) return false;
      if (set != APPFX_CLIMATE_NOUNITS) {
        char path[512];
        size_t n = 0;
        snprintf(path, sizeof path, "%s/%s", dir, imp ? "mock_getconfig_imperial.json" : "mock_getconfig_metric.json");
        char *c = slurp(path, &n);
        dz_units_t u;
        if (!c || !dz_units_parse(c, n, &u)) { free(c); return false; }
        free(c);
        dz_model_set_units(&f->model, &u);
      }
      return true;
    }
    case APPFX_MOCK_NOFAV: {
      g_strip_fav = true;
      bool ok = apply_file(f, dir, "mock_getdevices.json", DZ_COMMIT_FULL_DEVICES, NULL, NULL, NULL, NULL) &&
                apply_file(f, dir, "mock_getscenes.json", DZ_COMMIT_FULL_SCENES, NULL, NULL, NULL, NULL);
      g_strip_fav = false;
      return ok;
    }
    case APPFX_SWITCH_TYPES: {
      static const char doc[] =
        "{\"status\":\"OK\",\"result\":["
        "{\"idx\":\"1\",\"Name\":\"Garage Blind\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"SwitchType\":\"Blinds\",\"SwitchTypeVal\":3,\"Status\":\"Open\",\"Data\":\"Open\",\"Protected\":false,\"Favorite\":0},"
        "{\"idx\":\"2\",\"Name\":\"Roller Shutter\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"SwitchType\":\"Blinds Percentage\",\"SwitchTypeVal\":13,\"Status\":\"Set Level: 40 %\",\"Data\":\"Set Level: 40 %\",\"Level\":40,\"MaxDimLevel\":100,\"Protected\":false,\"Favorite\":0},"
        "{\"idx\":\"3\",\"Name\":\"Front Door\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"SwitchType\":\"Door Lock\",\"SwitchTypeVal\":19,\"Status\":\"Locked\",\"Data\":\"Locked\",\"Protected\":false,\"Favorite\":0},"
        "{\"idx\":\"4\",\"Name\":\"Back Door\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"SwitchType\":\"Door Contact\",\"SwitchTypeVal\":11,\"Status\":\"Open\",\"Data\":\"Open\",\"Protected\":false,\"Favorite\":0},"
        "{\"idx\":\"5\",\"Name\":\"Hall Smoke\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"SwitchType\":\"Smoke Detector\",\"SwitchTypeVal\":5,\"Status\":\"Off\",\"Data\":\"Off\",\"Protected\":false,\"Favorite\":0},"
        "{\"idx\":\"6\",\"Name\":\"Gate Button\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"SwitchType\":\"Push On Button\",\"SwitchTypeVal\":9,\"Status\":\"Off\",\"Data\":\"Off\",\"Protected\":false,\"Favorite\":0}"
        "]}";
      return dz_model_apply(&f->model, doc, sizeof doc - 1, DZ_COMMIT_FULL_DEVICES, f->T, NULL, NULL) == DZ_OK;
    }
    case APPFX_MANY: {
      static char doc[60000];
      size_t n = (size_t)snprintf(doc, sizeof doc, "{\"status\":\"OK\",\"result\":[");
      for (int i = 1; i <= 150; i++) {
        const char *type = i <= 100 ? "Light/Switch" : i <= 130 ? "Temp" : "General";
        const char *sub = i <= 100 ? "Switch" : i <= 130 ? "LaCrosse TX3" : "kWh";
        n += (size_t)snprintf(doc + n, sizeof doc - n,
                              "%s{\"idx\":\"%d\",\"Name\":\"%s %03d\",\"Type\":\"%s\",\"SubType\":\"%s\",%s\"Favorite\":%d}", i > 1 ? "," : "", i,
                              i <= 100 ? "Lamp" : i <= 130 ? "Temp" : "Meter", i, type, sub,
                              i <= 100 ? "\"Status\":\"Off\",\"Data\":\"Off\"," : i <= 130 ? "\"Temp\":20.5,\"Data\":\"20.5 C\"," : "\"Data\":\"3.5 kWh\",\"Usage\":\"120 Watt\",", i <= 8);
      }
      n += (size_t)snprintf(doc + n, sizeof doc - n, "]}");
      return dz_model_apply(&f->model, doc, n, DZ_COMMIT_FULL_DEVICES, f->T, NULL, NULL) == DZ_OK;
    }
    case APPFX_EMPTY:
    default: return true;
  }
}

void appfx_unload(appfx_t *f) {
  if (f->model.mu) dz_model_deinit(&f->model);
  if (f->tr) dz_transport_destroy(f->tr);
  free(f->ent);
  free(f->stage);
  free(f->rbuf);
  memset(f, 0, sizeof *f);
}

/* ------------------------------------------------------------------ screens */

const char *const apps_demo_names[] = {
  "home", "home_connecting", "home_offline", "home_token", "home_stale", "home_empty", "home_favs_many", "home_favs_none",
  "lights", "lights_many", "lights_types", "scenes", "scenes_mixed", "temperature", "weather", "utility", "energy", "energy_counter", "settings", "settings_conn",
  "settings_bright", "settings_about",
  "home_climate", "home_climate_f", "climate_temp", "climate_temp_hum_baro", "climate_hum", "climate_wind", "climate_rain", "climate_uv",
  "climate_lux", "climate_air", "climate_value", "climate_text", "climate_unseen",
  "climate_temp_f", "climate_wind_f", "climate_nounits", "climate_stale",
  "detail_climate_wind", "detail_thermo6", "detail_setpoint_plan", "detail_setpoint_nonow", "detail_radiator", "detail_evohome",
  "detail_mode", "detail_setpoint_f", "detail_setpoint_nounits",
  "confirm_setpoint",
  "detail_onoff", "detail_dimmer", "detail_selector", "detail_blinds", "detail_thermo", "detail_lock", "detail_push", "detail_info",
  "confirm_scene", "confirm_group_mixed", "confirm_lock",
  "fb_pending", "fb_confirmed", "fb_unconfirmed", "fb_failed",
};
const int apps_demo_count = (int)(sizeof apps_demo_names / sizeof apps_demo_names[0]);

bool appfx_find(appfx_t *f, dz_cap_t cap, int state, char *idx, bool *scene) {
  size_t total = dz_model_count(&f->model);
  for (size_t i = 0; i < total; i++) {
    dz_device_t d; ru_val_t v; ru_val_state_t st; bool sc;
    if (!dz_model_at(&f->model, i, &d, &v, &st, &sc, f->T) || d.cap != cap || d.is_protected) continue;
    if (state >= 0 && (int)dz_device_state(&d) != state) continue;
    snprintf(idx, 12, "%s", d.idx);
    if (scene) *scene = sc;
    return true;
  }
  return false;
}

static void click_at(int x, int y) {
  sim_pointer(x, y, true);
  sim_advance(80);
  sim_pointer(x, y, false);
  sim_advance(80);
}

static void hold_at(int x, int y, uint32_t ms) {
  sim_pointer(x, y, true);
  sim_advance(ms);
  sim_pointer(x, y, false);
  sim_advance(100);
}

static appfx_set_t set_for(const char *n) {
  if (!strcmp(n, "home_climate_f") || !strcmp(n, "climate_temp_f") || !strcmp(n, "climate_wind_f") || !strcmp(n, "detail_setpoint_f")) return APPFX_CLIMATE_IMPERIAL;
  if (!strcmp(n, "climate_nounits") || !strcmp(n, "detail_setpoint_nounits")) return APPFX_CLIMATE_NOUNITS;
  if (!strcmp(n, "home_climate") || !strcmp(n, "temperature") || !strcmp(n, "weather") || !strncmp(n, "climate_", 8) || !strcmp(n, "detail_climate_wind") || !strcmp(n, "detail_thermo6") || !strncmp(n, "detail_setpoint", 15) ||
      !strcmp(n, "detail_radiator") || !strcmp(n, "detail_evohome") || !strcmp(n, "detail_mode") || !strcmp(n, "confirm_setpoint"))
    return APPFX_CLIMATE_METRIC;
  if (!strcmp(n, "home_favs_none")) return APPFX_MOCK_NOFAV;
  if (!strcmp(n, "home_favs_many")) return APPFX_MOCK;
  if (!strcmp(n, "lights_many")) return APPFX_MANY;
  if (!strcmp(n, "lights_types")) return APPFX_SWITCH_TYPES;
  if (!strcmp(n, "scenes_mixed") || !strncmp(n, "confirm_group", 13) || !strncmp(n, "fb_", 3)) return APPFX_MOCK_MIXED;
  if (!strncmp(n, "detail_", 7) && strcmp(n, "detail_info") != 0) return APPFX_MOCK;
  if (!strcmp(n, "confirm_scene") || !strcmp(n, "confirm_lock")) return APPFX_MOCK;
  if (!strcmp(n, "home_empty") || !strcmp(n, "home_connecting")) return APPFX_EMPTY;
  return APPFX_REAL;
}

ru_dzapp_t *apps_demo_show(appfx_t *f, const char *dir, const char *n) {
  bool known = false;
  for (int i = 0; i < apps_demo_count; i++) if (!strcmp(n, apps_demo_names[i])) known = true;
  if (!known) return NULL;
  bool stale = !strcmp(n, "home_stale") || !strcmp(n, "climate_stale");
  if (!appfx_load(f, dir, set_for(n), stale ? 300000u : 600000u)) return NULL;
  ru_dzapp_t *a = ru_dzapp_create(&f->model, &f->client, true);
  if (a) ru_dzapp_set_display_info(a, "sim", !sim_panel_square());
  if (!a) return NULL;
  /* "stale": the model was last confirmed 7 minutes before the UI looks at it */
  f->T += stale ? 420000u : 1000u;
  ru_dzapp_set_conn(a, !strcmp(n, "home_connecting") ? DZ_CS_CONNECTING : !strcmp(n, "home_offline") ? DZ_CS_OFFLINE
                       : !strcmp(n, "home_token") ? DZ_CS_TOKEN_REJECTED : DZ_CS_OK);
  ru_dzapp_ui_tick(a, f->T);
  int c = ru_theme_geom()->diameter / 2;
  char idx[12];
  bool sc = false;
  ru_section_t sec = RU_SEC_HOME;
  if (!strncmp(n, "lights", 6)) sec = RU_SEC_LIGHTS;
  else if (!strncmp(n, "scenes", 6) || !strncmp(n, "fb_", 3)) sec = RU_SEC_SCENES;
  else if (!strcmp(n, "temperature")) sec = RU_SEC_TEMP;
  else if (!strcmp(n, "weather")) sec = RU_SEC_WEATHER;
  else if (!strcmp(n, "utility")) sec = RU_SEC_UTILITY;
  else if (!strncmp(n, "energy", 6)) sec = RU_SEC_ENERGY;
  else if (!strncmp(n, "settings", 8)) sec = RU_SEC_SETTINGS;
  ru_dzapp_show_section(a, sec);
  /* a later page of a long list: the swipe a finger would make */
  if (!strcmp(n, "home_favs_many")) ru_dzapp_swipe(a, RU_DIR_UP, c, c);
  if (!strcmp(n, "lights_many")) for (int k = 0; k < 4; k++) ru_dzapp_swipe(a, RU_DIR_UP, c, c);

  if (!strcmp(n, "energy_counter")) {
    /* the energy device that has no power reading: its hero is the day total with the unit Domoticz reports */
    int k = 0;
    size_t total = dz_model_count(&f->model);
    for (size_t i = 0; i < total; i++) {
      dz_device_t d; ru_val_t v; ru_val_state_t st; bool s2;
      if (!dz_model_at(&f->model, i, &d, &v, &st, &s2, f->T) || s2 || d.cap != DZ_CAP_ENERGY) continue;
      if (!(d.has_usage || d.has_usage_deliv || d.has_today)) continue;
      if (!d.has_usage && !d.has_usage_deliv && d.has_today) { ru_dzapp_select(a, RU_SEC_ENERGY, k); break; }
      k++;
    }
  } else if (!strncmp(n, "climate_", 8)) {
    /* the readings page of one sensor: the detail view of the Temperature / Weather pages */
    static const struct { const char *name, *idx; } pick[] = {
      {"climate_temp", "100"}, {"climate_temp_hum_baro", "103"}, {"climate_hum", "101"}, {"climate_wind", "104"},
      {"climate_rain", "105"}, {"climate_uv", "106"}, {"climate_lux", "107"}, {"climate_air", "108"}, {"climate_value", "112"},
      {"climate_text", "113"}, {"climate_unseen", "116"},
      {"climate_temp_f", "103"}, {"climate_wind_f", "104"}, {"climate_nounits", "104"}, {"climate_stale", "100"}};
    for (size_t k = 0; k < sizeof pick / sizeof pick[0]; k++)
      if (!strcmp(n, pick[k].name)) ru_dzapp_open(a, pick[k].idx, false);
  } else if (!strncmp(n, "detail_", 7) && (strstr(n, "thermo6") || strstr(n, "setpoint") || strstr(n, "radiator") || strstr(n, "evohome") ||
                                           strstr(n, "mode") || strstr(n, "climate_wind"))) {
    static const struct { const char *name, *idx; } det[] = {
      {"detail_climate_wind", "104"}, {"detail_thermo6", "117"}, {"detail_setpoint_plan", "118"}, {"detail_setpoint_nonow", "121"},
      {"detail_radiator", "123"}, {"detail_evohome", "124"}, {"detail_mode", "125"}, {"detail_setpoint_f", "118"},
      {"detail_setpoint_nounits", "118"}};
    for (size_t k = 0; k < sizeof det / sizeof det[0]; k++) if (!strcmp(n, det[k].name)) ru_dzapp_open(a, det[k].idx, false);
  } else if (!strcmp(n, "confirm_setpoint")) {
    ru_dzapp_open(a, "118", false);
    sim_render();
    click_at(c + ru_u(14), c + ru_u(24)); /* the + button: a setpoint always asks for a hold */
  } else if (!strcmp(n, "settings_conn")) ru_dzapp_open_sub(a, RU_SUB_CONNECTION);
  else if (!strcmp(n, "settings_bright")) ru_dzapp_open_sub(a, RU_SUB_BRIGHTNESS);
  else if (!strcmp(n, "settings_about")) ru_dzapp_open_sub(a, RU_SUB_ABOUT);
  else if (!strcmp(n, "detail_onoff") && appfx_find(f, DZ_CAP_ONOFF, -1, idx, &sc)) ru_dzapp_open(a, idx, false);
  else if (!strcmp(n, "detail_dimmer") && appfx_find(f, DZ_CAP_DIMMER, -1, idx, &sc)) ru_dzapp_open(a, idx, false);
  else if (!strcmp(n, "detail_selector") && appfx_find(f, DZ_CAP_SELECTOR, -1, idx, &sc)) ru_dzapp_open(a, idx, false);
  else if (!strcmp(n, "detail_blinds") && appfx_find(f, DZ_CAP_BLINDS, -1, idx, &sc)) ru_dzapp_open(a, idx, false);
  else if (!strcmp(n, "detail_thermo") && appfx_find(f, DZ_CAP_THERMOSTAT, -1, idx, &sc)) ru_dzapp_open(a, idx, false);
  else if (!strcmp(n, "detail_lock") && appfx_find(f, DZ_CAP_LOCK, -1, idx, &sc)) ru_dzapp_open(a, idx, false);
  else if (!strcmp(n, "detail_push") && appfx_find(f, DZ_CAP_PUSH, -1, idx, &sc)) ru_dzapp_open(a, idx, false);
  else if (!strcmp(n, "detail_info") && appfx_find(f, DZ_CAP_TEMP, -1, idx, &sc)) ru_dzapp_open(a, idx, false);
  else if (!strcmp(n, "confirm_scene") && appfx_find(f, DZ_CAP_SCENE, -1, idx, &sc)) ru_dzapp_open(a, idx, true);
  else if (!strcmp(n, "confirm_group_mixed") && appfx_find(f, DZ_CAP_GROUP, DZ_STATE_MIXED, idx, &sc)) ru_dzapp_open(a, idx, true);
  else if (!strcmp(n, "confirm_lock") && appfx_find(f, DZ_CAP_LOCK, -1, idx, &sc)) {
    ru_dzapp_open(a, idx, false);
    sim_render();
    click_at(c, c + ru_u(14)); /* the LOCK / UNLOCK button: a lock always asks for a hold */
  } else if (!strncmp(n, "fb_", 3)) {
    dz_cmd_result_t r;
    memset(&r, 0, sizeof r);
    if (!strcmp(n, "fb_pending")) {
      if (appfx_find(f, DZ_CAP_GROUP, DZ_STATE_ON, idx, &sc)) ru_dzapp_open(a, idx, true);
      hold_at(c, c + ru_u(13), 1600); /* a full hold: the request is queued, nothing has answered yet */
    } else {
      appfx_find(f, DZ_CAP_GROUP, DZ_STATE_MIXED, idx, &sc);
      snprintf(r.idx, sizeof r.idx, "%s", idx);
      if (!strcmp(n, "fb_confirmed")) r.outcome = DZ_OUT_CONFIRMED;
      else if (!strcmp(n, "fb_unconfirmed")) { r.outcome = DZ_OUT_UNCONFIRMED; r.have_after = true; strcpy(r.after.status, "Mixed"); r.after.cap = DZ_CAP_GROUP; }
      else { r.outcome = DZ_OUT_FAILED; r.fail = DZ_FAIL_TRANSPORT; }
      ru_dzapp_inject_result(a, &r);
    }
  }
  ru_dzapp_ui_tick(a, f->T);
  return a;
}

/* ------------------------------------------------------------------ gestures like the device shell */

static struct { ru_gesture_t g; ru_dzapp_t *app; } G;

static void route(const ru_gesture_event_t *e) {
  if (!G.app) return;
  if (e->zone != RU_ZONE_RIM) {
    if (e->type == RU_G_SWIPE) ru_dzapp_swipe(G.app, e->dir, (int)e->x0, (int)e->y0);
    return;
  }
  ru_nav_input_t in;
  if (!ru_nav_input_from_gesture(e, &in)) return;
  if (in.type == RU_NAV_IN_BACK) ru_dzapp_back(G.app);
  else if (in.type == RU_NAV_IN_GO_HOME || in.type == RU_NAV_IN_HOME_ACTION) ru_dzapp_home(G.app);
}

static void touch_hook(int x, int y, bool down, uint32_t t_ms, void *ud) {
  (void)ud;
  ru_touch_t t = { (int16_t)x, (int16_t)y, t_ms, down, 1, 0 };
  ru_gesture_event_t ev[RU_GESTURE_MAX_EVENTS];
  int n = ru_gesture_feed(&G.g, &t, ev, RU_GESTURE_MAX_EVENTS);
  for (int i = 0; i < n; i++) route(&ev[i]);
  n = ru_gesture_tick(&G.g, t_ms, ev, RU_GESTURE_MAX_EVENTS);
  for (int i = 0; i < n; i++) route(&ev[i]);
}

void appfx_gestures_attach(ru_dzapp_t *a, int diameter, bool square) {
  ru_gesture_cfg_t cfg;
  if (square) ru_gesture_cfg_square(&cfg, diameter);
  else ru_gesture_cfg_default(&cfg, diameter);
  memset(&G, 0, sizeof G);
  ru_gesture_init(&G.g, diameter, &cfg);
  G.app = a;
  sim_set_touch_hook(touch_hook, NULL);
}

void appfx_gestures_detach(void) {
  sim_set_touch_hook(NULL, NULL);
  G.app = NULL;
}

void appfx_drag(int x0, int y0, int x1, int y1, int steps) {
  sim_pointer(x0, y0, true);
  sim_advance(40);
  for (int k = 1; k <= steps; k++) {
    sim_pointer(x0 + (x1 - x0) * k / steps, y0 + (y1 - y0) * k / steps, true);
    sim_advance(16);
  }
  sim_pointer(x1, y1, false);
  sim_advance(100);
}

int apps_demo_shot(sim_t *s, const char *dir, const char *name, const char *path) {
  appfx_t f;
  ru_dzapp_t *a = apps_demo_show(&f, dir, name);
  if (!a) { appfx_unload(&f); fprintf(stderr, "cannot build apps screen '%s'\n", name); return 2; }
  sim_render();
  int rc = sim_save_png(s, path, true) ? 0 : 1;
  if (!rc) printf("wrote %s\n", path);
  ru_dzapp_destroy(a);
  appfx_unload(&f);
  return rc;
}
