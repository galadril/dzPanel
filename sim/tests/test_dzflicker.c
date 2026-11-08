/* Flicker budget: a data refresh must redraw only what changed. Widget setters with identical input invalidate nothing;
 * the Domoticz screens update tiles in place instead of rebuilding the screen. Pixels are counted from LVGL's
 * LV_EVENT_INVALIDATE_AREA on the display (sim_inv_*). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps_demo.h"
#include "mini_test.h"
#include "ru_icons.h"
#include "ru_widgets.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "../tests/fixtures"
#endif

static sim_t S;

static void settle(void) { sim_advance(200); sim_inv_reset(); }

/* ---- (6) identical setter calls invalidate nothing ---- */
static void test_setters_idempotent(void) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *vt = ru_value_tile_create(scr, 200, 40, "LABEL");
  lv_obj_t *st = ru_state_tile_create(scr, 120, 100, "NAME");
  lv_obj_t *g = ru_ring_gauge_create(scr, 150, "CAP");
  lv_obj_t *rim = ru_status_rim_create(scr);
  ru_value_tile_set_text(vt, "21.5C", RU_VAL_OK);
  ru_state_tile_set(st, RU_DOT_ON, "ON", RU_VAL_OK);
  ru_state_tile_set_value(st, "5");
  lv_obj_t *st0 = ru_state_tile_create(scr, 120, 100, "NONE");
  ru_state_tile_set(st0, RU_DOT_NONE, "", RU_VAL_OK);
  ru_state_tile_set_value(st0, ""); /* an empty value: still no redraw on repeats */
  ru_ring_gauge_set_display(g, "42", 0.4f, RU_VAL_OK);
  ru_status_rim_set_state(rim, RU_RIM_OK);
  settle();
  for (int i = 0; i < 3; i++) {
    ru_value_tile_set_text(vt, "21.5C", RU_VAL_OK);
    ru_state_tile_set(st, RU_DOT_ON, "ON", RU_VAL_OK);
    ru_state_tile_set_value(st, "5");
    ru_state_tile_set(st0, RU_DOT_NONE, "", RU_VAL_OK);
    ru_state_tile_set_value(st0, "");
    ru_ring_gauge_set_display(g, "42", 0.4f, RU_VAL_OK);
    ru_status_rim_set_state(rim, RU_RIM_OK);
  }
  sim_advance(100);
  CHECK(sim_inv_pixels() == 0);
  if (sim_inv_pixels()) printf("  setters: %ld px invalidated by identical calls\n", sim_inv_pixels());
  ru_value_tile_set_text(vt, "22.0C", RU_VAL_OK);
  sim_advance(100);
  CHECK(sim_inv_pixels() > 0); /* a real change still draws */
  lv_obj_clean(scr);
}

/* ---- app level: APPFX_MANY = 100 lights (idx 1..8 favourites), 30 temperature (101..130), 20 meters (131..150) ---- */

static bool tree_has_text(lv_obj_t *o, const char *needle) {
  if (lv_obj_check_type(o, &lv_label_class) && strstr(lv_label_get_text(o), needle)) return true;
  uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; i++) if (tree_has_text(lv_obj_get_child(o, (int32_t)i), needle)) return true;
  return false;
}

static ru_dzapp_t *start_many(appfx_t *f) {
  CHECK(appfx_load(f, FIXTURE_DIR, APPFX_MANY, 600000));
  ru_dzapp_t *a = ru_dzapp_create(&f->model, &f->client, true);
  CHECK(a != NULL);
  ru_dzapp_set_display_info(a, "sim", false);
  f->T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_dzapp_ui_tick(a, f->T);
  settle();
  return a;
}

/* one device re-read (partial commit), as the poller does */
static void push(appfx_t *f, const char *json_dev) {
  char doc[600];
  snprintf(doc, sizeof doc, "{\"status\":\"OK\",\"result\":[%s]}", json_dev);
  CHECK(dz_model_apply(&f->model, doc, strlen(doc), DZ_COMMIT_PARTIAL, f->T, NULL, NULL) == DZ_OK);
}

static const char *lamp(int idx, const char *name, const char *status, int fav) {
  static char b[300];
  snprintf(b, sizeof b, "{\"idx\":\"%d\",\"Name\":\"%s\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"Status\":\"%s\",\"Data\":\"%s\",\"Favorite\":%d}",
           idx, name, status, status, fav);
  return b;
}

static const char *temp_dev(int idx, double t) {
  static char b[300];
  snprintf(b, sizeof b, "{\"idx\":\"%d\",\"Name\":\"Temp %03d\",\"Type\":\"Temp\",\"SubType\":\"LaCrosse TX3\",\"Temp\":%.1f,\"Data\":\"%.1f C\",\"Favorite\":0}", idx, idx, t, t);
  return b;
}

static void tick(appfx_t *f, ru_dzapp_t *a, uint32_t dt) {
  f->T += dt;
  ru_dzapp_ui_tick(a, f->T);
  sim_advance(100);
}

/* (1) a device that is not on the screen changes: same screen object, nothing invalidated, no copies of the model */
static void test_offscreen_change(void) {
  appfx_t f;
  ru_dzapp_t *a = start_many(&f);
  lv_obj_t *scr = lv_screen_active();
  push(&f, lamp(50, "Lamp 050", "On", 0));
  push(&f, "{\"idx\":\"140\",\"Name\":\"Meter 140\",\"Type\":\"General\",\"SubType\":\"kWh\",\"Data\":\"9.9 kWh\",\"Usage\":\"500 Watt\",\"Favorite\":0}");
  sim_inv_reset();
  tick(&f, a, 300);
  printf("  offscreen change: %ld px invalidated\n", sim_inv_pixels());
  CHECK(lv_screen_active() == scr);
  CHECK(sim_inv_pixels() == 0);
  /* (4) nothing changed: a tick takes no copy from the model at all */
  uint32_t c0 = dz_model_copies(&f.model);
  for (int i = 0; i < 4; i++) tick(&f, a, 250);
  CHECK(dz_model_copies(&f.model) == c0);
  CHECK(lv_screen_active() == scr);
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* (2) a value on the screen changes: the same screen, a small area redrawn, the new text shown */
static void test_visible_change(void) {
  appfx_t f;
  ru_dzapp_t *a = start_many(&f);
  const long px = (long)S.size * S.size;
  /* Favourites: lamp 2 goes on */
  lv_obj_t *scr = lv_screen_active();
  CHECK(ru_dzapp_section(a) == RU_SEC_FAVS);
  push(&f, lamp(2, "Lamp 002", "On", 1));
  sim_inv_reset();
  tick(&f, a, 300);
  printf("  favourites value change: %ld px (%.1f%%), %ld areas\n", sim_inv_pixels(), 100.0 * (double)sim_inv_pixels() / (double)px, sim_inv_count());
  CHECK(lv_screen_active() == scr);
  CHECK(sim_inv_pixels() > 0 && sim_inv_pixels() < px / 20);
  ru_dzapp_row_info_t ri;
  CHECK(ru_dzapp_row_info(a, 1, &ri) && !strcmp(ri.idx, "2") && !strcmp(ri.sub, "ON"));
  CHECK(tree_has_text(lv_screen_active(), "ON"));
  /* Lights */
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  tick(&f, a, 300);
  settle();
  scr = lv_screen_active();
  push(&f, lamp(3, "Lamp 003", "On", 0));
  sim_inv_reset();
  tick(&f, a, 300);
  printf("  lights value change: %ld px (%.1f%%)\n", sim_inv_pixels(), 100.0 * (double)sim_inv_pixels() / (double)px);
  CHECK(lv_screen_active() == scr);
  CHECK(sim_inv_pixels() > 0 && sim_inv_pixels() < px / 20);
  CHECK(ru_dzapp_row_info(a, 2, &ri) && !strcmp(ri.idx, "3") && !strcmp(ri.sub, "ON"));
  /* Temperature */
  ru_dzapp_show_section(a, RU_SEC_TEMP);
  tick(&f, a, 300);
  settle();
  scr = lv_screen_active();
  CHECK(!tree_has_text(scr, "23.7"));
  push(&f, temp_dev(103, 23.7));
  sim_inv_reset();
  tick(&f, a, 300);
  printf("  temperature value change: %ld px (%.1f%%)\n", sim_inv_pixels(), 100.0 * (double)sim_inv_pixels() / (double)px);
  CHECK(lv_screen_active() == scr);
  CHECK(sim_inv_pixels() > 0 && sim_inv_pixels() < px / 20);
  CHECK(tree_has_text(lv_screen_active(), "23.7"));
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* a FULL read of the given lamps (all favourites), as the poller does */
static void full3(appfx_t *f, int a, int b, int c) {
  char doc[1200];
  snprintf(doc, sizeof doc, "{\"status\":\"OK\",\"result\":[%s", lamp(a, "L", "Off", 1));
  size_t n = strlen(doc);
  n += (size_t)snprintf(doc + n, sizeof doc - n, ",%s", lamp(b, "L", "Off", 1));
  n += (size_t)snprintf(doc + n, sizeof doc - n, ",%s]}", lamp(c, "L", "Off", 1));
  CHECK(dz_model_apply(&f->model, doc, n, DZ_COMMIT_FULL_DEVICES, f->T, NULL, NULL) == DZ_OK);
}

/* (3) a structure change may rebuild, and shows the new structure */
static void test_structure_change(void) {
  appfx_t f;
  ru_dzapp_t *a = start_many(&f);
  CHECK(!tree_has_text(lv_screen_active(), "RENAMED"));
  push(&f, lamp(2, "Renamed", "Off", 1));
  tick(&f, a, 2100);
  CHECK(tree_has_text(lv_screen_active(), "RENAMED"));
  ru_dzapp_set_conn(a, DZ_CS_OFFLINE);
  tick(&f, a, 10); /* a connection change is not rate limited */
  CHECK(tree_has_text(lv_screen_active(), "OFFLINE"));
  ru_dzapp_set_conn(a, DZ_CS_OK);
  tick(&f, a, 10);
  CHECK(!tree_has_text(lv_screen_active(), "OFFLINE"));
  /* a full read with new favourites first: the SHOWN page gets new content */
  full3(&f, 61, 62, 63);
  tick(&f, a, 2100);
  ru_dzapp_row_info_t ri;
  CHECK(ru_dzapp_home_rows(a) == 3);
  CHECK(ru_dzapp_row_info(a, 0, &ri) && !strcmp(ri.idx, "61"));
  push(&f, lamp(64, "Fresh", "Off", 1)); /* a delta arrival joins the tail of the shown page */
  tick(&f, a, 2100);
  CHECK(ru_dzapp_home_rows(a) == 4);
  CHECK(tree_has_text(lv_screen_active(), "FRESH"));
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* (4) a build copies at most the entries it shows, whatever the model size; the climate pages with 30 sensors too */
static void test_copy_budget(void) {
  appfx_t f;
  ru_dzapp_t *a = start_many(&f);
  static const ru_section_t secs[] = { RU_SEC_FAVS, RU_SEC_LIGHTS, RU_SEC_TEMP, RU_SEC_UTILITY };
  for (size_t i = 0; i < sizeof secs / sizeof secs[0]; i++) {
    uint32_t c0 = dz_model_copies(&f.model);
    ru_dzapp_show_section(a, secs[i]);
    uint32_t used = dz_model_copies(&f.model) - c0;
    printf("  build of %s: %u model copies (model has %zu)\n", ru_section_name(secs[i]), (unsigned)used, dz_model_count(&f.model));
    CHECK(used <= 3 * 6 + 4); /* shown tiles (create + signature), never the 150 entries */
  }
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* (7) background rebuilds: at most one per 2 s; the data of the screen stays right meanwhile via the next rebuild */
static void test_rate_limit(void) {
  appfx_t f;
  ru_dzapp_t *a = start_many(&f);
  lv_obj_t *s0 = lv_screen_active();
  push(&f, lamp(2, "Renamed A", "Off", 1));
  tick(&f, a, 300);
  CHECK(lv_screen_active() == s0); /* the rename waits */
  tick(&f, a, 300);
  CHECK(lv_screen_active() == s0);
  tick(&f, a, 1600);
  CHECK(lv_screen_active() != s0);
  CHECK(tree_has_text(lv_screen_active(), "RENAMED A"));
  lv_obj_t *s1 = lv_screen_active();
  push(&f, lamp(2, "Renamed B", "Off", 1));
  tick(&f, a, 250);
  CHECK(lv_screen_active() == s1);
  tick(&f, a, 2000);
  CHECK(lv_screen_active() != s1 && tree_has_text(lv_screen_active(), "RENAMED B"));
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* a reorder on the server (no data change) reaches the page */
static void test_order_only(void) {
  appfx_t f;
  ru_dzapp_t *a = start_many(&f);
  full3(&f, 1, 2, 3);
  tick(&f, a, 2100);
  ru_dzapp_row_info_t ri;
  CHECK(ru_dzapp_row_info(a, 0, &ri) && !strcmp(ri.idx, "1"));
  tick(&f, a, 2100); /* idle for longer than the rate limit */
  full3(&f, 2, 1, 3);
  tick(&f, a, 10); /* within the same stale epoch: only the generation can announce it */
  CHECK(ru_dzapp_row_info(a, 0, &ri) && !strcmp(ri.idx, "2"));
  CHECK(ru_dzapp_row_info(a, 1, &ri) && !strcmp(ri.idx, "1"));
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* a temperature device with extra JSON members appended (fields the old detail hash did not cover) */
static void push_temp_extra(appfx_t *f, int idx, double t, const char *extra) {
  char b[400];
  snprintf(b, sizeof b, "{\"idx\":\"%d\",\"Name\":\"Temp %03d\",\"Type\":\"Temp\",\"SubType\":\"LaCrosse TX3\",\"Temp\":%.1f,\"Data\":\"%.1f C\",\"Favorite\":0%s}", idx, idx, t, t, extra);
  push(f, b);
}

/* an open detail page follows every change of its device at once (no 2 s limit, whole-entry signature) */
static void test_detail_live(void) {
  appfx_t f;
  ru_dzapp_t *a = start_many(&f);
  ru_dzapp_show_section(a, RU_SEC_TEMP);
  tick(&f, a, 300);
  CHECK(ru_dzapp_tap_row(a, 2)); /* idx 103 */
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && !strcmp(ru_dzapp_current_idx(a), "103"));
  tick(&f, a, 10);
  static const char *extras[] = { ",\"ForecastStr\":\"Sunny\"", ",\"HumidityStatus\":\"Wet\"", ",\"Protected\":true", ",\"MaxDimLevel\":32" };
  static const char *names[] = { "forecast", "hum_status", "is_protected", "max_level" };
  for (size_t i = 0; i < 4; i++) {
    lv_obj_t *s0 = lv_screen_active();
    push_temp_extra(&f, 103, 21.5, extras[i]);
    tick(&f, a, 10); /* far inside the 2 s window of the last build */
    if (lv_screen_active() == s0) printf("  detail: %s change not shown\n", names[i]);
    CHECK(lv_screen_active() != s0);
  }
  /* a plain value change is shown on the next tick as well */
  lv_obj_t *s1 = lv_screen_active();
  CHECK(!tree_has_text(s1, "27.3"));
  push_temp_extra(&f, 103, 27.3, ",\"MaxDimLevel\":32,\"Protected\":true,\"HumidityStatus\":\"Wet\",\"ForecastStr\":\"Sunny\"");
  tick(&f, a, 10);
  CHECK(lv_screen_active() != s1 && tree_has_text(lv_screen_active(), "27.3"));
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* the pager dots on a non-grid screen follow a newly appearing section */
static void test_dots_nongrid(void) {
  appfx_t f;
  ru_dzapp_t *a = start_many(&f);
  /* a model with lights only; open a detail page */
  char doc[400];
  size_t n = (size_t)snprintf(doc, sizeof doc, "{\"status\":\"OK\",\"result\":[%s", lamp(1, "L", "Off", 1));
  snprintf(doc + n, sizeof doc - n, ",%s]}", lamp(5, "L", "Off", 0));
  CHECK(dz_model_apply(&f.model, doc, strlen(doc), DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  tick(&f, a, 2100);
  CHECK(!ru_dzapp_section_available(a, RU_SEC_TEMP));
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  tick(&f, a, 300);
  ru_dzapp_open_sub(a, RU_SUB_ABOUT);
  tick(&f, a, 10);
  lv_obj_t *s0 = lv_screen_active();
  push(&f, temp_dev(5, 20.0)); /* same count: lamp 5 becomes a sensor */
  tick(&f, a, 10);
  CHECK(ru_dzapp_section_available(a, RU_SEC_TEMP));
  CHECK(lv_screen_active() != s0);
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* time alone: a state going stale is shown after the 1.5 s stale epoch, in place */
static void test_stale_flip(void) {
  appfx_t f;
  CHECK(appfx_load(&f, FIXTURE_DIR, APPFX_MANY, 3000));
  ru_dzapp_t *a = ru_dzapp_create(&f.model, &f.client, true);
  CHECK(a != NULL);
  ru_dzapp_set_display_info(a, "sim", false);
  f.T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_dzapp_ui_tick(a, f.T);
  settle();
  lv_obj_t *scr = lv_screen_active();
  tick(&f, a, 1000);
  CHECK(sim_inv_pixels() == 0);
  tick(&f, a, 1600); /* past the TTL: the favourites are stale */
  tick(&f, a, 1600);
  CHECK(lv_screen_active() == scr);
  CHECK(sim_inv_pixels() > 0); /* the stale look was drawn without a rebuild */
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* getconfig arrives: the temperature texts get their unit */
static void test_units_change(void) {
  appfx_t f;
  ru_dzapp_t *a = start_many(&f);
  ru_dzapp_show_section(a, RU_SEC_TEMP);
  tick(&f, a, 300);
  CHECK(!tree_has_text(lv_screen_active(), "F"));
  dz_units_t u;
  memset(&u, 0, sizeof u);
  u.have_temp = true;
  strcpy(u.temp, "F");
  dz_model_set_units(&f.model, &u);
  tick(&f, a, 2100);
  CHECK(tree_has_text(lv_screen_active(), "F"));
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* a device added on another page changes the page count, not the shown page */
static void test_pages_count(void) {
  appfx_t f;
  ru_dzapp_t *a = start_many(&f);
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  tick(&f, a, 300);
  int p0 = ru_dzapp_pages(a);
  for (int i = 0; i < 3; i++) push(&f, lamp(200 + i, "Extra", "Off", 0));
  tick(&f, a, 2100);
  CHECK(ru_dzapp_pages(a) == p0 + 1);
  CHECK(ru_dzapp_page(a) == 0);
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* the Utility rows follow a live value */
static void test_utility_rows(void) {
  appfx_t f;
  ru_dzapp_t *a = start_many(&f);
  ru_dzapp_show_section(a, RU_SEC_UTILITY);
  tick(&f, a, 300);
  settle();
  lv_obj_t *scr = lv_screen_active();
  CHECK(ru_dzapp_home_rows(a) > 0);
  CHECK(!tree_has_text(scr, "500"));
  ru_dzapp_row_info_t ri;
  CHECK(ru_dzapp_row_info(a, 0, &ri));
  char js[300];
  snprintf(js, sizeof js, "{\"idx\":\"%s\",\"Name\":\"Meter %s\",\"Type\":\"General\",\"SubType\":\"kWh\",\"Data\":\"9.9 kWh\",\"Usage\":\"500 Watt\",\"Favorite\":0}", ri.idx, ri.idx);
  push(&f, js);
  sim_inv_reset();
  tick(&f, a, 300);
  CHECK(lv_screen_active() == scr);
  CHECK(sim_inv_pixels() > 0);
  CHECK(ru_dzapp_row_info(a, 0, &ri) && !strcmp(ri.sub, "500 W"));
  CHECK(tree_has_text(lv_screen_active(), "500"));
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* ---- weather tiles at app level (APPFX_CLIMATE_METRIC: a temperature sensor, rain gauge and UV sensor with units) ---- */
static lv_obj_t *wx_tile_of(lv_obj_t *o, const char *name) {
  uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
    if (n >= 6 && lv_obj_check_type(c, &lv_label_class) && i == 0 && !strcmp(lv_label_get_text(c), name)) return o;
    lv_obj_t *r = wx_tile_of(c, name);
    if (r) return r;
  }
  return NULL;
}

static int count_desc(lv_obj_t *o) {
  int n = 0;
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) n += 1 + count_desc(lv_obj_get_child(o, (int32_t)i));
  return n;
}

static lv_color_t rgb(uint32_t v) { return lv_color_hex(v); }

static void push_json(appfx_t *f, const char *fmt, ...) {
  char b[700];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  push(f, b);
}

static void test_wx_tiles(void) {
  appfx_t f;
  CHECK(appfx_load(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 600000));
  ru_dzapp_t *a = ru_dzapp_create(&f.model, &f.client, true);
  CHECK(a != NULL);
  ru_dzapp_set_display_info(a, "sim", false);
  f.T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_dzapp_ui_tick(a, f.T);
  const long px = (long)S.size * S.size;
  ru_dzapp_show_section(a, RU_SEC_TEMP);
  settle();
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *tile = wx_tile_of(scr, "TEMP SENSOR");
  CHECK(tile != NULL);
  if (!tile) { ru_dzapp_destroy(a); appfx_unload(&f); return; }
  CHECK(count_desc(tile) == 6); /* name, icon, value, unit, small line, dot */
  lv_obj_t *icon = ru_wx_tile_part(tile, 1), *val = ru_wx_tile_part(tile, 2);
  CHECK(!strcmp(lv_label_get_text(val), "21.5") && !strcmp(lv_label_get_text(icon), RU_ICON_THERMOMETER));
  CHECK(lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN), rgb(0x3DDC84)) && lv_color_eq(lv_obj_get_style_text_color(val, LV_PART_MAIN), rgb(0xE6E9EE)));
  /* a value change: the same screen and the same tile, a small area, the icon follows the band, the value stays white */
  const char *tdev = "{\"idx\":\"100\",\"Name\":\"Temp Sensor\",\"Type\":\"Temp\",\"SubType\":\"LaCrosse TX3\",\"Temp\":%.1f,\"Data\":\"%.1f C\",\"Favorite\":1,\"trend\":2}";
  push_json(&f, tdev, 36.0, 36.0);
  sim_inv_reset();
  tick(&f, a, 300);
  printf("  wx value change: %ld px (%.1f%%)\n", sim_inv_pixels(), 100.0 * (double)sim_inv_pixels() / (double)px);
  CHECK(lv_screen_active() == scr && wx_tile_of(scr, "TEMP SENSOR") == tile);
  CHECK(sim_inv_pixels() > 0 && sim_inv_pixels() < px / 20);
  CHECK(!strcmp(lv_label_get_text(val), "36.0"));
  CHECK(lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN), rgb(0xFF5A5F)) && lv_color_eq(lv_obj_get_style_text_color(val, LV_PART_MAIN), rgb(0xE6E9EE)));
  /* nothing changed: nothing redrawn */
  settle();
  for (int i = 0; i < 4; i++) tick(&f, a, 250);
  CHECK(sim_inv_pixels() == 0 && lv_screen_active() == scr);
  /* Weather: rain gauge (rate 0.4 drizzle, 0 cloud, 12 pouring), UV 1/4/9/12 */
  ru_dzapp_show_section(a, RU_SEC_WEATHER);
  tick(&f, a, 300);
  settle();
  lv_obj_t *rain = wx_tile_of(lv_screen_active(), "RAIN GAUGE"), *uv = wx_tile_of(lv_screen_active(), "UV SENSOR");
  CHECK(rain && uv);
  if (rain && uv) {
    const char *rdev = "{\"idx\":\"105\",\"Name\":\"Rain Gauge\",\"Type\":\"Rain\",\"SubType\":\"RainByRate\",\"Rain\":\"12.5\",\"RainRate\":\"%s\",\"Data\":\"0;12.5\",\"Favorite\":1}";
    const char *rate[] = { "0.4", "0", "12" };
    const char *ric[] = { RU_ICON_CLOUD_DRIZZLE, RU_ICON_CLOUD, RU_ICON_CLOUD_RAIN };
    uint32_t rcol[] = { 0x4DB3FF, 0x8A93A0, 0x8C7CFF };
    for (int i = 0; i < 3; i++) {
      push_json(&f, rdev, rate[i]);
      tick(&f, a, 300);
      lv_obj_t *ri = ru_wx_tile_part(rain, 1);
      if (strcmp(lv_label_get_text(ri), ric[i])) printf("  rain %s: wrong icon\n", rate[i]);
      CHECK(!strcmp(lv_label_get_text(ri), ric[i]) && lv_color_eq(lv_obj_get_style_text_color(ri, LV_PART_MAIN), rgb(rcol[i])));
    }
    const char *udev = "{\"idx\":\"106\",\"Name\":\"UV Sensor\",\"Type\":\"UV\",\"SubType\":\"UV3\",\"UVI\":\"%d\",\"Temp\":20.1,\"Data\":\"%d UVI\",\"Favorite\":1}";
    int uvi[] = { 1, 4, 9, 12 };
    const char *uic[] = { RU_ICON_SUN_DIM, RU_ICON_SUN, RU_ICON_SUN, RU_ICON_SUN };
    uint32_t ucol[] = { 0x3DDC84, 0xFFC247, 0xFF5A5F, 0x8C7CFF };
    for (int i = 0; i < 4; i++) {
      push_json(&f, udev, uvi[i], uvi[i]);
      tick(&f, a, 300);
      lv_obj_t *ui = ru_wx_tile_part(uv, 1), *uw = ru_wx_tile_part(uv, 4);
      CHECK(!strcmp(lv_label_get_text(ui), uic[i]) && lv_color_eq(lv_obj_get_style_text_color(ui, LV_PART_MAIN), rgb(ucol[i])));
      CHECK(lv_color_eq(lv_obj_get_style_text_color(uw, LV_PART_MAIN), rgb(ucol[i]))); /* the UV word carries the band colour too */
    }
    CHECK(lv_screen_active() == lv_obj_get_screen(uv)); /* all of it in place, no rebuild of the screen */
    /* stale: no new data past the TTL; the same screen, grey icon and value, warn dot, border, no band colour */
    lv_obj_t *scr2 = lv_screen_active();
    tick(&f, a, 700000);
    tick(&f, a, 1000);
    CHECK(lv_screen_active() == scr2 || wx_tile_of(lv_screen_active(), "UV SENSOR"));
    lv_obj_t *t2 = wx_tile_of(lv_screen_active(), "UV SENSOR");
    CHECK(t2 != NULL);
    if (t2) {
      CHECK(lv_color_eq(lv_obj_get_style_text_color(ru_wx_tile_part(t2, 1), LV_PART_MAIN), ru_theme()->stale));
      CHECK(lv_color_eq(lv_obj_get_style_text_color(ru_wx_tile_part(t2, 2), LV_PART_MAIN), ru_theme()->stale));
      CHECK(!lv_obj_has_flag(ru_wx_tile_part(t2, 5), LV_OBJ_FLAG_HIDDEN) && lv_obj_has_state(t2, LV_STATE_USER_1));
    }
  }
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

static lv_obj_t *find_label(lv_obj_t *o, const char *needle) {
  if (lv_obj_check_type(o, &lv_label_class) && strstr(lv_label_get_text(o), needle)) return o;
  uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t *r = find_label(lv_obj_get_child(o, (int32_t)i), needle);
    if (r) return r;
  }
  return NULL;
}

/* the Clock page: a minute change redraws only the time label; ticks in between touch and copy nothing */
static void test_clock_minute(void) {
  appfx_t f;
  CHECK(appfx_load(&f, FIXTURE_DIR, APPFX_REAL, 600000));
  ru_dzapp_t *a = ru_dzapp_create(&f.model, &f.client, true);
  CHECK(a != NULL);
  ru_dzapp_set_display_info(a, "sim", false);
  f.T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_dzapp_ui_tick(a, f.T);
  ru_dzapp_show_section(a, RU_SEC_CLOCK);
  settle();
  lv_obj_t *scr = lv_screen_active();
  char hm[8], dt[12];
  CHECK(ru_dzapp_clock_text(a, hm, dt) && !strcmp(hm, "13:07"));
  lv_obj_t *tl = find_label(scr, "13:");
  CHECK(tl != NULL);
  lv_area_t ar;
  lv_obj_get_coords(tl, &ar);
  long label_px = (long)(ar.x2 - ar.x1 + 1) * (ar.y2 - ar.y1 + 1);
  /* ticks inside the same minute: nothing drawn, nothing copied */
  tick(&f, a, 1600); /* the first look after a build evaluates everything once */
  uint32_t c0 = dz_model_copies(&f.model);
  sim_inv_reset();
  for (int i = 0; i < 6; i++) tick(&f, a, 1600);
  CHECK(ru_dzapp_clock_text(a, hm, dt) && !strcmp(hm, "13:07"));
  CHECK(sim_inv_pixels() == 0);
  CHECK(dz_model_copies(&f.model) == c0);
  CHECK(lv_screen_active() == scr);
  /* the minute turns */
  tick(&f, a, 70000);
  sim_inv_reset();
  tick(&f, a, 1600);
  tick(&f, a, 1600);
  CHECK(ru_dzapp_clock_text(a, hm, dt) && !strcmp(hm, "13:09"));
  /* the label is redrawn, once per change and not outside its own box (a few px for glyph overhang) */
  printf("  clock minute change: %ld px, label %ld px, %ld areas\n", sim_inv_pixels(), label_px, sim_inv_count());
  CHECK(lv_screen_active() == scr && find_label(scr, "09") == tl);
  CHECK(sim_inv_pixels() > 0 && sim_inv_pixels() <= label_px + label_px / 4);
  CHECK(dz_model_copies(&f.model) == c0);
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

int main(void) {
  CHECK(sim_init(&S, 480));
  test_clock_minute();
  test_setters_idempotent();
  test_offscreen_change();
  test_visible_change();
  test_structure_change();
  test_copy_budget();
  test_rate_limit();
  test_order_only();
  test_detail_live();
  test_dots_nongrid();
  test_stale_flip();
  test_units_change();
  test_pages_count();
  test_utility_rows();
  test_wx_tiles();
  sim_shutdown(&S);
  MT_DONE();
}
