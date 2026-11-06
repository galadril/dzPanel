/* Flicker budget: a data refresh must redraw only what changed. Widget setters with identical input invalidate nothing;
 * the Domoticz screens update tiles in place instead of rebuilding the screen. Pixels are counted from LVGL's
 * LV_EVENT_INVALIDATE_AREA on the display (sim_inv_*). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps_demo.h"
#include "mini_test.h"
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
  ru_ring_gauge_set_display(g, "42", 0.4f, RU_VAL_OK);
  ru_status_rim_set_state(rim, RU_RIM_OK);
  settle();
  for (int i = 0; i < 3; i++) {
    ru_value_tile_set_text(vt, "21.5C", RU_VAL_OK);
    ru_state_tile_set(st, RU_DOT_ON, "ON", RU_VAL_OK);
    ru_state_tile_set_value(st, "5");
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
  CHECK(ru_dzapp_section(a) == RU_SEC_HOME);
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
  /* a new favourite appears: the page shows it */
  push(&f, lamp(60, "Fresh", "Off", 1));
  tick(&f, a, 2100);
  size_t rows = ru_dzapp_home_rows(a);
  CHECK(rows == 6); /* 9 favourites: 6 on the first page */
  ru_dzapp_destroy(a);
  appfx_unload(&f);
}

/* (4) a build copies at most the entries it shows, whatever the model size; the climate pages with 30 sensors too */
static void test_copy_budget(void) {
  appfx_t f;
  ru_dzapp_t *a = start_many(&f);
  static const ru_section_t secs[] = { RU_SEC_HOME, RU_SEC_LIGHTS, RU_SEC_TEMP, RU_SEC_UTILITY };
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

/* a FULL read of the given lamps (all favourites), as the poller does */
static void full3(appfx_t *f, int a, int b, int c) {
  char doc[1200];
  snprintf(doc, sizeof doc, "{\"status\":\"OK\",\"result\":[%s", lamp(a, "L", "Off", 1));
  size_t n = strlen(doc);
  n += (size_t)snprintf(doc + n, sizeof doc - n, ",%s", lamp(b, "L", "Off", 1));
  n += (size_t)snprintf(doc + n, sizeof doc - n, ",%s]}", lamp(c, "L", "Off", 1));
  CHECK(dz_model_apply(&f->model, doc, n, DZ_COMMIT_FULL_DEVICES, f->T, NULL, NULL) == DZ_OK);
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

int main(void) {
  CHECK(sim_init(&S, 480));
  test_setters_idempotent();
  test_offscreen_change();
  test_visible_change();
  test_structure_change();
  test_copy_budget();
  test_rate_limit();
  test_order_only();
  sim_shutdown(&S);
  MT_DONE();
}
