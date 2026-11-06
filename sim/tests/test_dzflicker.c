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

int main(void) {
  CHECK(sim_init(&S, 480));
  test_setters_idempotent();
  sim_shutdown(&S);
  MT_DONE();
}
