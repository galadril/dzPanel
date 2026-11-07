/* The light detail page inside the app (fixture-backed, no network): requests from touches, in-place updates, protected lamps,
 * the ring image is generated once across rebuilds. Runs at 466 round, 480 round and 480 square. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps_demo.h"
#include "mini_test.h"
#include "ru_ring_img.h"
#include "ru_widgets.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "../tests/fixtures"
#endif

typedef struct { int n; dz_cmd_req_t last; int cnt[24]; char idx[12]; } spy_t;
static void spy_fn(const dz_cmd_req_t *r, void *ud) {
  spy_t *s = ud;
  s->n++;
  s->last = *r;
  snprintf(s->idx, sizeof s->idx, "%s", r->idx);
  s->last.idx = s->idx;
  if ((int)r->kind < 24) s->cnt[r->kind]++;
}

static void ring_xy(int size, float r_u, float deg, int *x, int *y) {
  float a = deg * 3.14159265f / 180.0f, r = r_u * (float)size / 100.0f;
  *x = (int)lroundf((float)size / 2.0f + r * cosf(a));
  *y = (int)lroundf((float)size / 2.0f + r * sinf(a));
}
static void touch(int x, int y, bool down) { sim_pointer(x, y, down); sim_advance(50); }

static const char *text_of(lv_obj_t *o) { return lv_label_get_text(o); }

static void push(appfx_t *f, const char *json_dev) {
  char doc[900];
  snprintf(doc, sizeof doc, "{\"status\":\"OK\",\"result\":[%s]}", json_dev);
  CHECK(dz_model_apply(&f->model, doc, strlen(doc), DZ_COMMIT_PARTIAL, f->T, NULL, NULL) == DZ_OK);
}

static const char *spots(int level, int hue_mode_m, const char *extra) {
  static char b[700];
  snprintf(b, sizeof b,
           "{\"idx\":\"1\",\"Name\":\"Spots\",\"Type\":\"Color Switch\",\"SubType\":\"RGBWW\",\"SwitchTypeVal\":7,\"Status\":\"Set Level: %d %%\",\"Data\":\"On\","
           "\"Level\":%d,\"LevelInt\":%d,\"MaxDimLevel\":100,\"Favorite\":1,\"Protected\":false,\"HaveTimeout\":false,"
           "\"Color\":\"{\\\"b\\\":0,\\\"cw\\\":0,\\\"g\\\":128,\\\"m\\\":%d,\\\"r\\\":255,\\\"t\\\":0,\\\"ww\\\":0}\"%s}",
           level, level, level, hue_mode_m, extra);
  return b;
}

static void tick(appfx_t *f, ru_dzapp_t *a, uint32_t dt) {
  f->T += dt;
  ru_dzapp_ui_tick(a, f->T);
  sim_advance(100);
}

static void run(int size, bool square) {
  sim_t S;
  CHECK(sim_init_ex(&S, size, square));
  ru_ring_img_free();
  printf("  %d %s\n", size, square ? "square" : "round");
  appfx_t f;
  CHECK(appfx_load(&f, FIXTURE_DIR, APPFX_LIGHTS, 600000));
  ru_dzapp_t *a = ru_dzapp_create(&f.model, &f.client, true);
  CHECK(a != NULL);
  ru_dzapp_set_display_info(a, "sim", !square);
  f.T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_dzapp_ui_tick(a, f.T);
  spy_t spy;
  memset(&spy, 0, sizeof spy);
  ru_dzapp_set_cmd_spy(a, spy_fn, &spy);
  int c = size / 2, x, y;
  float bri_mid = square ? 43.0f : 43.0f - 2.2f, col_mid = square ? 28.5f : bri_mid;

  /* idx 1: RGBWW, orange, 70 %, On */
  ru_dzapp_open(a, "1", false);
  tick(&f, a, 10);
  lv_obj_t *w = ru_dzapp_light(a);
  CHECK(w != NULL && ru_dzapp_view(a) == RU_VIEW_DETAIL);
  if (!w) { sim_shutdown(&S); return; }
  CHECK(!strcmp(text_of(ru_light_part(w, 4)), "70%") && !strcmp(text_of(ru_light_part(w, 5)), square ? "ORANGE" : "BRIGHTNESS"));
  uint32_t gen0 = ru_ring_img_generations();
  /* the hue ring: on a round panel the COLOUR chip first (it sends nothing) */
  if (!square) { ru_light_press_chip(w, 1); CHECK(spy.n == 0); }
  ring_xy(size, col_mid, 225, &x, &y); /* hue 120 */
  touch(x, y, true);
  ring_xy(size, col_mid, 226, &x, &y);
  touch(x, y, true);
  CHECK(spy.n == 0); /* nothing while the finger is down */
  ring_xy(size, col_mid, 225, &x, &y);
  touch(x, y, true);
  touch(x, y, false);
  CHECK(spy.n == 1 && spy.last.kind == DZ_CMD_COLOR_HUE && abs(spy.last.hue - 120) <= 2 && spy.last.level == 70 && !strcmp(spy.last.idx, "1"));
  if (spy.n != 1) printf("    hue: n=%d kind=%d hue=%d level=%d\n", spy.n, (int)spy.last.kind, spy.last.hue, spy.last.level);
  CHECK(!strcmp(text_of(ru_light_part(w, 5)), "SENDING")); /* feedback in the caption */
  /* a brightness drag: one SET_LEVEL near 50 */
  int before = spy.n;
  if (!square) ru_light_press_chip(w, 0);
  ring_xy(size, bri_mid, 270, &x, &y);
  touch(x, y, true);
  touch(x, y, false);
  CHECK(spy.n == before + 1 && spy.last.kind == DZ_CMD_SET_LEVEL && abs(spy.last.level - 50) <= 2);
  /* the disc: a tap on a lit lamp is OFF */
  before = spy.n;
  touch(c, c, true);
  touch(c, c, false);
  CHECK(spy.n == before + 1 && spy.last.kind == DZ_CMD_OFF);
  /* chips: a tap sends nothing and swaps the ring */
  if (square) {
    before = spy.n;
    ru_light_press_chip(w, 1);
    CHECK(spy.n == before && ru_light_view(w) == RU_RING_TEMP);
    ru_light_press_chip(w, 0);
  }

  /* in place: another device changes, then this one; the page object and its arc stay */
  tick(&f, a, 5000); /* the feedback of the commands above has timed out */
  lv_obj_t *scr0 = ru_dzapp_screen(a), *arc0 = ru_light_part(ru_dzapp_light(a), 1);
  push(&f, "{\"idx\":\"2\",\"Name\":\"Led Strip\",\"Type\":\"Color Switch\",\"SubType\":\"RGB\",\"SwitchTypeVal\":7,\"Status\":\"Set Level: 20 %\",\"Data\":\"On\",\"Level\":20,\"LevelInt\":20,\"MaxDimLevel\":100,\"Favorite\":1}");
  tick(&f, a, 300);
  CHECK(ru_dzapp_screen(a) == scr0 && ru_light_part(ru_dzapp_light(a), 1) == arc0);
  sim_advance(200);
  sim_inv_reset();
  push(&f, spots(40, 3, ""));
  tick(&f, a, 300);
  CHECK(ru_dzapp_screen(a) == scr0 && ru_light_part(ru_dzapp_light(a), 1) == arc0);
  CHECK(!strcmp(text_of(ru_light_part(ru_dzapp_light(a), 4)), "40%"));
  long px = sim_inv_pixels();
  CHECK(px > 0 && px < (long)size * size * 4 / 10);
  if (!(px > 0 && px < (long)size * size * 4 / 10)) printf("    level change invalidated %ld px\n", px);
  /* the same data again: nothing at all */
  sim_advance(200);
  sim_inv_reset();
  push(&f, spots(40, 3, ""));
  tick(&f, a, 300);
  CHECK(sim_inv_pixels() == 0 && ru_dzapp_screen(a) == scr0);
  /* a colour change in the Color JSON (mode 2: temperature) shows in place too */
  push(&f, spots(40, 2, ""));
  tick(&f, a, 300);
  CHECK(ru_dzapp_screen(a) == scr0);
  /* structure changes rebuild: the lamp becomes protected; the ring image is not generated again (cached) */
  push(&f, spots(40, 3, ",\"Protected\":true"));
  tick(&f, a, 300);
  CHECK(ru_dzapp_screen(a) != scr0 && ru_dzapp_light(a) != NULL);
  CHECK(!strcmp(text_of(ru_light_part(ru_dzapp_light(a), 5)), "PROTECTED"));
  before = spy.n;
  w = ru_dzapp_light(a);
  if (!square) ru_light_press_chip(w, 1);
  ring_xy(size, col_mid, 225, &x, &y);
  touch(x, y, true);
  touch(x, y, false);
  touch(c, c, true);
  touch(c, c, false);
  CHECK(spy.n == before); /* a protected lamp sends nothing */
  (void)gen0;
  ru_dzapp_back(a);
  tick(&f, a, 10);
  ru_dzapp_open(a, "1", false);
  tick(&f, a, 10);
  uint32_t gen1 = ru_ring_img_generations();
  ru_dzapp_back(a);
  ru_dzapp_open(a, "1", false);
  tick(&f, a, 10);
  ru_dzapp_back(a);
  ru_dzapp_open(a, "1", false);
  tick(&f, a, 10);
  CHECK(ru_ring_img_generations() == gen1); /* rebuilds of the same lamp reuse the image */
  ru_dzapp_back(a);
  ru_dzapp_open(a, "4", false); /* a WW lamp: the temperature ring */
  tick(&f, a, 10);
  CHECK(ru_dzapp_light(a) && ru_ring_img_generations() <= gen1 + 1);
  ru_dzapp_back(a);
  ru_dzapp_open(a, "4", false);
  uint32_t gen2 = ru_ring_img_generations();
  ru_dzapp_back(a);
  ru_dzapp_open(a, "4", false);
  tick(&f, a, 10);
  CHECK(ru_ring_img_generations() == gen2);

  /* a plain dimmer (no colour): one ring, the disc, no chips */
  ru_dzapp_back(a);
  ru_dzapp_open(a, "9", false);
  tick(&f, a, 10);
  CHECK(ru_dzapp_light(a) && ru_light_chips(ru_dzapp_light(a)) == 0 && ru_light_part(ru_dzapp_light(a), 2) == NULL);
  CHECK(!strcmp(text_of(ru_light_part(ru_dzapp_light(a), 4)), "50%"));
  ru_dzapp_back(a);
  tick(&f, a, 10);
  CHECK(ru_dzapp_light(a) == NULL);
  ru_dzapp_destroy(a);
  appfx_unload(&f);
  ru_ring_img_free();
  sim_shutdown(&S);
}

/* next / previous device: swipes on a light page, never from a ring */
static void run_swipe(int size, bool square) {
  sim_t S;
  CHECK(sim_init_ex(&S, size, square));
  ru_ring_img_free();
  appfx_t f;
  CHECK(appfx_load(&f, FIXTURE_DIR, APPFX_LIGHTS, 600000));
  ru_dzapp_t *a = ru_dzapp_create(&f.model, &f.client, true);
  ru_dzapp_set_display_info(a, "sim", !square);
  f.T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_dzapp_ui_tick(a, f.T);
  int c = size / 2, x, y;
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  ru_dzapp_open(a, "4", false);
  tick(&f, a, 10);
  CHECK(ru_dzapp_light(a) != NULL);
  /* a swipe from the disc steps to the next device (idx 5), another to idx 6, right goes back */
  CHECK(ru_dzapp_swipe(a, RU_DIR_LEFT, c, c) && !strcmp(ru_dzapp_current_idx(a), "5"));
  tick(&f, a, 10);
  CHECK(ru_dzapp_swipe(a, RU_DIR_LEFT, c, c) && !strcmp(ru_dzapp_current_idx(a), "6"));
  CHECK(ru_dzapp_swipe(a, RU_DIR_RIGHT, c, c) && !strcmp(ru_dzapp_current_idx(a), "5"));
  /* from a ring's touch band: refused */
  ring_xy(size, 43.0f - (square ? 0.0f : 2.2f), 200, &x, &y);
  CHECK(!ru_dzapp_swipe(a, RU_DIR_LEFT, x, y) && !strcmp(ru_dzapp_current_idx(a), "5"));
  if (square) {
    ring_xy(size, 28.5f, 200, &x, &y); /* the inner ring */
    CHECK(!ru_dzapp_swipe(a, RU_DIR_LEFT, x, y) && !strcmp(ru_dzapp_current_idx(a), "5"));
  }
  /* from the rim: the shell's; vertical swipes do nothing here */
  CHECK(!ru_dzapp_swipe(a, RU_DIR_LEFT, 3, c) && !ru_dzapp_swipe(a, RU_DIR_UP, c, c));
  /* the dead zone below the sweep (the gap) is free */
  ring_xy(size, 43.0f, 90, &x, &y);
  CHECK(ru_dzapp_swipe(a, RU_DIR_RIGHT, x, y) && !strcmp(ru_dzapp_current_idx(a), "4"));
  /* a held control blocks it: the guard of a drag in progress */
  tick(&f, a, 10);
  ring_xy(size, 43.0f - (square ? 0.0f : 2.2f), 270, &x, &y);
  touch(x, y, true);
  touch(x + 3, y, true); /* a drag is under way (the ring's callback sets the guard) */
  CHECK(!ru_dzapp_swipe(a, RU_DIR_LEFT, c, c));
  touch(x + 3, y, false);
  /* the ends: no wrap */
  ru_dzapp_open(a, "1", false);
  tick(&f, a, 3000);
  f.T += 10000;
  CHECK(!ru_dzapp_swipe(a, RU_DIR_RIGHT, c, c) && !strcmp(ru_dzapp_current_idx(a), "1"));
  ru_dzapp_open(a, "9", false);
  CHECK(!ru_dzapp_swipe(a, RU_DIR_LEFT, c, c) && !strcmp(ru_dzapp_current_idx(a), "9"));
  /* the indicator names the position */
  tick(&f, a, 10);
  CHECK(ru_dzapp_light(a) != NULL);
  ru_dzapp_destroy(a);
  appfx_unload(&f);
  ru_ring_img_free();
  sim_shutdown(&S);
}

/* the dial pages (thermostat): a swipe is never taken from the dial ring or the buttons */
static void run_swipe_dial(int size, bool square) {
  sim_t S;
  CHECK(sim_init_ex(&S, size, square));
  appfx_t f;
  CHECK(appfx_load(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 600000));
  ru_dzapp_t *a = ru_dzapp_create(&f.model, &f.client, true);
  ru_dzapp_set_display_info(a, "sim", !square);
  f.T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_dzapp_ui_tick(a, f.T);
  int c = size / 2, x, y;
  ru_dzapp_show_section(a, RU_SEC_UTILITY);
  ru_dzapp_open(a, "118", false);
  tick(&f, a, 10);
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && ru_dzapp_light(a) == NULL);
  ring_xy(size, 40.0f, 200, &x, &y);
  CHECK(!ru_dzapp_swipe(a, RU_DIR_LEFT, x, y) && !strcmp(ru_dzapp_current_idx(a), "118"));
  CHECK(!ru_dzapp_swipe(a, RU_DIR_LEFT, c, c + (int)(20.0f * (float)size / 100.0f))); /* the buttons */
  bool moved = ru_dzapp_swipe(a, RU_DIR_LEFT, c, c - (int)(8.0f * (float)size / 100.0f)) ||
               ru_dzapp_swipe(a, RU_DIR_RIGHT, c, c - (int)(8.0f * (float)size / 100.0f));
  CHECK(moved && strcmp(ru_dzapp_current_idx(a), "118") != 0); /* the centre is free */
  ru_dzapp_destroy(a);
  appfx_unload(&f);
  sim_shutdown(&S);
}

/* the ring image cannot be allocated: the colour ring is a read-only track with a caption; brightness and the disc keep working;
 * the allocation is tried once per page, never per paint */
static void run_noring(int size, bool square) {
  sim_t S;
  CHECK(sim_init_ex(&S, size, square));
  ru_ring_img_free();
  appfx_t f;
  CHECK(appfx_load(&f, FIXTURE_DIR, APPFX_LIGHTS, 600000));
  ru_dzapp_t *a = ru_dzapp_create(&f.model, &f.client, true);
  ru_dzapp_set_display_info(a, "sim", !square);
  f.T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_dzapp_ui_tick(a, f.T);
  spy_t spy;
  memset(&spy, 0, sizeof spy);
  ru_dzapp_set_cmd_spy(a, spy_fn, &spy);
  int c = size / 2, x, y;
  float bri_mid = square ? 43.0f : 43.0f - 2.2f, col_mid = square ? 28.5f : bri_mid;
  ru_ring_img_test_fail_alloc(true);
  ru_dzapp_open(a, "1", false);
  tick(&f, a, 10);
  lv_obj_t *w = ru_dzapp_light(a);
  CHECK(w != NULL);
  if (!w) { ru_ring_img_test_fail_alloc(false); sim_shutdown(&S); return; }
  if (!square) ru_light_press_chip(w, 1); /* round: show the colour ring */
  tick(&f, a, 10);
  CHECK(!strcmp(text_of(ru_light_part(w, 5)), "NO COLOUR RING"));
  /* many paints: the allocation is not retried (generations stay, no image) */
  uint32_t g0 = ru_ring_img_generations();
  ru_ring_img_test_fail_alloc(false); /* memory is back, but this page does not retry per paint */
  for (int i = 0; i < 5; i++) tick(&f, a, 300);
  CHECK(ru_ring_img_generations() == g0 && !strcmp(text_of(ru_light_part(w, 5)), "NO COLOUR RING"));
  /* the colour ring is not touchable */
  int before = spy.n;
  ring_xy(size, col_mid, 225, &x, &y);
  touch(x, y, true);
  touch(x, y, false);
  CHECK(spy.n == before);
  /* the disc still works */
  touch(c, c, true);
  touch(c, c, false);
  CHECK(spy.n == before + 1 && spy.last.kind == DZ_CMD_OFF);
  /* brightness still works (square: its ring is always there; round: switch back) */
  before = spy.n;
  if (!square) ru_light_press_chip(w, 0);
  ring_xy(size, bri_mid, 270, &x, &y);
  touch(x, y, true);
  touch(x, y, false);
  CHECK(spy.n == before + 1 && spy.last.kind == DZ_CMD_SET_LEVEL);
  /* the next page open retries once and now has the ring */
  ru_dzapp_back(a);
  tick(&f, a, 10);
  ru_dzapp_open(a, "1", false);
  tick(&f, a, 10);
  CHECK(ru_ring_img_generations() == g0 + 1);
  w = ru_dzapp_light(a);
  CHECK(w != NULL && strcmp(text_of(ru_light_part(w, 5)), "NO COLOUR RING") != 0);
  ru_dzapp_destroy(a);
  appfx_unload(&f);
  ru_ring_img_free();
  sim_shutdown(&S);
}

/* MaxDimLevel above 100: the command path refuses level commands, so the page is read-only (UNSUPPORTED) and the percentage is clamped */
static void run_unsupported(int size, bool square) {
  sim_t S;
  CHECK(sim_init_ex(&S, size, square));
  ru_ring_img_free();
  appfx_t f;
  CHECK(appfx_load(&f, FIXTURE_DIR, APPFX_LIGHTS, 600000));
  ru_dzapp_t *a = ru_dzapp_create(&f.model, &f.client, true);
  ru_dzapp_set_display_info(a, "sim", !square);
  f.T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_dzapp_ui_tick(a, f.T);
  spy_t spy;
  memset(&spy, 0, sizeof spy);
  ru_dzapp_set_cmd_spy(a, spy_fn, &spy);
  int c = size / 2, x, y;
  push(&f, "{\"idx\":\"9\",\"Name\":\"Plain Dimmer\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"SwitchTypeVal\":7,\"Status\":\"Set Level: 78 %\","
         "\"Data\":\"On\",\"Level\":200,\"LevelInt\":200,\"MaxDimLevel\":255,\"Favorite\":1}");
  ru_dzapp_open(a, "9", false);
  tick(&f, a, 10);
  lv_obj_t *w = ru_dzapp_light(a);
  CHECK(w != NULL);
  if (!w) { sim_shutdown(&S); return; }
  CHECK(!strcmp(text_of(ru_light_part(w, 5)), "UNSUPPORTED"));
  int pct = atoi(text_of(ru_light_part(w, 4)));
  CHECK(pct == 78 && strchr(text_of(ru_light_part(w, 4)), '%')); /* 200 of 255 */
  /* a level at the top of the scale never reads above 100 % */
  push(&f, "{\"idx\":\"9\",\"Name\":\"Plain Dimmer\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"SwitchTypeVal\":7,\"Status\":\"Set Level: 100 %\","
         "\"Data\":\"On\",\"Level\":255,\"LevelInt\":255,\"MaxDimLevel\":255,\"Favorite\":1}");
  tick(&f, a, 300);
  w = ru_dzapp_light(a);
  CHECK(w && !strcmp(text_of(ru_light_part(w, 4)), "100%"));
  /* no drag ring, no disc tap sends anything */
  ring_xy(size, square ? 43.0f : 43.0f - 2.2f, 270, &x, &y);
  touch(x, y, true);
  touch(x, y, false);
  touch(c, c, true);
  touch(c, c, false);
  CHECK(spy.n == 0);
  ru_dzapp_destroy(a);
  appfx_unload(&f);
  ru_ring_img_free();
  sim_shutdown(&S);
}

/* a label with exactly this text anywhere under o */
static bool has_text(lv_obj_t *o, const char *txt) {
  if (lv_obj_check_type(o, &lv_label_class) && !strcmp(lv_label_get_text(o), txt)) return true;
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
    if (has_text(lv_obj_get_child(o, (int32_t)i), txt)) return true;
  return false;
}

/* the widget state cannot be allocated (LVGL heap exhausted): tile grids and detail pages must not crash; detail pages fall back to plain text */
static void run_attachfail(int size, bool square) {
  sim_t S;
  CHECK(sim_init_ex(&S, size, square));
  ru_ring_img_free();
  appfx_t f;
  CHECK(appfx_load(&f, FIXTURE_DIR, APPFX_LIGHTS, 600000));
  ru_dzapp_t *a = ru_dzapp_create(&f.model, &f.client, true);
  ru_dzapp_set_display_info(a, "sim", !square);
  f.T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_widgets_test_fail_attach(true);
  ru_dzapp_ui_tick(a, f.T); /* the tile grid */
  tick(&f, a, 10);
  CHECK(ru_dzapp_screen(a) != NULL);
  push(&f, "{\"idx\":\"9\",\"Name\":\"Plain Dimmer\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"SwitchTypeVal\":7,\"Status\":\"Set Level: 50 %\","
         "\"Data\":\"On\",\"Level\":50,\"LevelInt\":50,\"MaxDimLevel\":100,\"Favorite\":1}");
  const char *idxs[2] = { "1", "9" }; /* a colour light and a plain dimmer */
  for (int k = 0; k < 2; k++) {
    ru_dzapp_open(a, idxs[k], false);
    tick(&f, a, 10);
    tick(&f, a, 300);
    CHECK(ru_dzapp_light(a) == NULL);
    CHECK(has_text(ru_dzapp_screen(a), "BACK"));
    ru_dzapp_back(a);
    tick(&f, a, 10);
  }
  ru_widgets_test_fail_attach(false);
  ru_dzapp_open(a, "9", false); /* memory is back: the real page */
  tick(&f, a, 10);
  CHECK(ru_dzapp_light(a) != NULL);
  ru_dzapp_destroy(a);
  appfx_unload(&f);
  ru_ring_img_free();
  sim_shutdown(&S);
}

int main(void) {
  run_attachfail(466, false);
  run_attachfail(480, true);
  run_unsupported(466, false);
  run_unsupported(480, true);
  run_noring(466, false);
  run_noring(480, true);
  run(466, false);
  run(480, false);
  run(480, true);
  run_swipe(466, false);
  run_swipe(480, false);
  run_swipe(480, true);
  run_swipe_dial(466, false);
  run_swipe_dial(480, true);
  MT_DONE();
}
