/* Weather tile widget (ru_wx_tile_*): object budget, no overlap in the worst case, idempotent setter, stale look. Runs at 466 round,
 * 480 round and 480 square. */
#include <stdio.h>
#include <string.h>

#include "mini_test.h"
#include "ru_icons.h"
#include "ru_widgets.h"
#include "sim_core.h"

static int count_desc(lv_obj_t *o) {
  int n = 0;
  uint32_t c = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < c; i++) n += 1 + count_desc(lv_obj_get_child(o, (int32_t)i));
  return n;
}

static bool hit(lv_obj_t *a, lv_obj_t *b) {
  lv_area_t x, y;
  lv_obj_get_coords(a, &x);
  lv_obj_get_coords(b, &y);
  return x.x1 <= y.x2 && y.x1 <= x.x2 && x.y1 <= y.y2 && y.y1 <= x.y2;
}

static bool inside(lv_obj_t *p, lv_obj_t *c) {
  lv_area_t a, b;
  lv_obj_get_coords(p, &a);
  lv_obj_get_coords(c, &b);
  return b.x1 >= a.x1 && b.x2 <= a.x2 && b.y1 >= a.y1 && b.y2 <= a.y2;
}

static ru_wxtile_data_t data(const char *name, const char *icon, const char *val, const char *unit, const char *second, ru_val_state_t st) {
  const ru_theme_t *t = ru_theme();
  ru_wxtile_data_t d = { name, icon, t->info, val, unit, second, t->text_dim, st };
  return d;
}

static void run(int size, bool square) {
  sim_t S;
  CHECK(sim_init_ex(&S, size, square));
  lv_obj_t *scr = lv_screen_active();
  int w = ru_u(25), h = ru_u(21);
  printf("  %d %s: tile %d x %d\n", size, square ? "square" : "round", w, h);
  lv_obj_t *t = ru_wx_tile_create(scr, w, h);
  lv_obj_set_pos(t, 40, 40);
  ru_wxtile_data_t d = data("Living room", RU_ICON_THERMOMETER, "21.5", "\xC2\xB0" "C", "45%", RU_VAL_OK);
  ru_wx_tile_set(t, &d);
  lv_obj_update_layout(scr);
  CHECK(count_desc(t) == 6 && count_desc(t) < 8); /* name, icon, value, unit, second, dot */

  /* worst cases: the longest name against the widest value rows; no two boxes of the tile touch and all stay inside it */
  const char *names[] = { "ABCDEFGHIJKLMNOPQRSTUVWXYZabc", "Mmmmmmmmmmmmmmmmmmmmmmmmmmmmm" };
  struct { const char *icon, *v, *u, *s; } rows[] = {
    { RU_ICON_THERMOMETER, "-12.5", "\xC2\xB0" "F", "100%" },
    { RU_ICON_GAUGE, "1013", " hPa", "FORECAST" },
    { RU_ICON_WIND, "100.5", " km/h", "NNW  G 112.4" },
    { RU_ICON_CLOUD_RAIN, "1234.5", "", "RATE 12.5" },
    { "", "-12.5", "\xC2\xB0" "F", "" },
  };
  for (size_t n = 0; n < 2; n++) {
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
      d = data(names[n], rows[r].icon, rows[r].v, rows[r].u, rows[r].s, RU_VAL_OK);
      ru_wx_tile_set(t, &d);
      lv_obj_update_layout(scr);
      lv_obj_t *name = ru_wx_tile_part(t, 0), *icon = ru_wx_tile_part(t, 1), *val = ru_wx_tile_part(t, 2), *unit = ru_wx_tile_part(t, 3), *sec = ru_wx_tile_part(t, 4);
      bool ok = !hit(name, val) && !hit(name, unit) && !hit(name, sec) && !hit(val, sec) && !hit(unit, sec) && !hit(val, unit);
      if (rows[r].icon[0]) ok = ok && !hit(name, icon) && !hit(icon, val) && !hit(icon, unit) && !hit(icon, sec);
      for (int p = 0; p < 5; p++) ok = ok && inside(t, ru_wx_tile_part(t, p));
      if (!ok) printf("    overlap or overflow: name %zu row %zu\n", n, r);
      CHECK(ok);
      /* the value row is centred: the group's left and right gaps differ by at most 2 px */
      lv_area_t a, b, tt;
      lv_obj_get_coords(t, &tt);
      lv_obj_get_coords(rows[r].icon[0] ? icon : val, &a);
      lv_obj_get_coords(unit, &b);
      int right_end = b.x2 >= b.x1 ? b.x2 : (lv_obj_get_coords(val, &b), b.x2);
      int lg = a.x1 - tt.x1, rg = tt.x2 - right_end;
      if (!(lg - rg <= 2 && rg - lg <= 2)) printf("    not centred: %d vs %d (row %zu)\n", lg, rg, r);
      CHECK(lg - rg <= 2 && rg - lg <= 2);
    }
  }

  /* the same data again: nothing is invalidated */
  d = data("Living room", RU_ICON_THERMOMETER, "21.5", "\xC2\xB0" "C", "45%", RU_VAL_OK);
  ru_wx_tile_set(t, &d);
  sim_advance(100);
  sim_inv_reset();
  for (int i = 0; i < 3; i++) ru_wx_tile_set(t, &d);
  sim_advance(100);
  CHECK(sim_inv_pixels() == 0);
  if (sim_inv_pixels()) printf("    repeat: %ld px\n", sim_inv_pixels());
  /* a new value redraws a little, not the screen */
  d.value = "21.6";
  ru_wx_tile_set(t, &d);
  sim_advance(100);
  CHECK(sim_inv_pixels() > 0 && sim_inv_pixels() < (long)size * size / 20);
  /* an icon colour change alone touches only the icon */
  sim_inv_reset();
  d.icon_color = ru_theme()->warn;
  ru_wx_tile_set(t, &d);
  sim_advance(100);
  CHECK(sim_inv_pixels() > 0 && sim_inv_pixels() < 40 * 40);

  /* stale: value and icon grey, warn dot shown, border on; back to OK clears all three */
  lv_obj_t *icon = ru_wx_tile_part(t, 1), *val = ru_wx_tile_part(t, 2), *dot = ru_wx_tile_part(t, 5);
  d.icon_color = ru_theme()->error;
  d.state = RU_VAL_OK;
  ru_wx_tile_set(t, &d);
  CHECK(lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN), ru_theme()->error) && lv_color_eq(lv_obj_get_style_text_color(val, LV_PART_MAIN), ru_theme()->text));
  CHECK(lv_obj_has_flag(dot, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_state(t, LV_STATE_USER_1));
  d.state = RU_VAL_STALE;
  ru_wx_tile_set(t, &d);
  CHECK(lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN), ru_theme()->stale) && lv_color_eq(lv_obj_get_style_text_color(val, LV_PART_MAIN), ru_theme()->stale));
  CHECK(!lv_obj_has_flag(dot, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_state(t, LV_STATE_USER_1));
  d.state = RU_VAL_UNKNOWN;
  d.value = "--";
  ru_wx_tile_set(t, &d);
  CHECK(!lv_obj_has_flag(dot, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_state(t, LV_STATE_USER_1) &&
        lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN), ru_theme()->stale));
  d.state = RU_VAL_OK;
  d.value = "21.5";
  ru_wx_tile_set(t, &d);
  CHECK(lv_obj_has_flag(dot, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_state(t, LV_STATE_USER_1));
  lv_obj_clean(scr);
  sim_shutdown(&S);
}

int main(void) {
  run(466, false);
  run(480, false);
  run(480, true);
  MT_DONE();
}
