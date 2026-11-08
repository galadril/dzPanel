/* Boot splash behaviour: overlay covers what is underneath, completes, fades out and frees itself; deleting it
 * mid-animation is safe (ASAN would catch a dangling animation var); the error variant is stable. */
#include "mini_test.h"
#include "sim_core.h"
#include "ru_splash.h"
#include "ru_theme.h"
#include <stdio.h>

static uint32_t px(const sim_t *s, int x, int y) { return s->fb[y * s->size + x] & 0xFFFFFF; }

/* a bright full-screen object on the real screen, so "covered" and "uncovered" are visible in the framebuffer */
static lv_obj_t *white_underlay(void) {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_screen_load(scr);
  return scr;
}

static void run(int size) {
  sim_t s;
  CHECK(sim_init(&s, size));
  int c = size / 2;
  white_underlay();
  sim_render();
  CHECK(px(&s, c, c) == 0xFFFFFF); /* sanity: the underlay is visible before any splash */

  /* overlay: on the top layer, covers the underlay, absorbs touches */
  ru_splash_cfg_t cfg = { "dzPanel", "for Domoticz", "v0.1.0" };
  lv_obj_t *o = ru_splash_overlay_create(&cfg);
  CHECK(o != NULL);
  CHECK(lv_obj_get_parent(o) == lv_layer_top());
  CHECK(lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE));
  sim_render();
  CHECK(px(&s, c, c) != 0xFFFFFF);        /* covered: not the white underlay (the house mark is unlit at 0) */
  CHECK(px(&s, c, c) == 0x000000);        /* at progress 0 nothing is drawn yet */
  CHECK(px(&s, c / 2, c / 2) == 0x000000);
  CHECK(!ru_splash_is_complete(o));
  long lit0 = sim_count_lit(&s);

  /* play: complete after the full draw+fade time, with more drawn than at the start, and still inside the circle */
  ru_splash_start(o);
  sim_advance(RU_SPLASH_TOTAL_MS / 2);
  CHECK(!ru_splash_is_complete(o));
  sim_advance(RU_SPLASH_TOTAL_MS);
  sim_render();
  CHECK(ru_splash_is_complete(o));
  CHECK(sim_count_lit(&s) > lit0);
  CHECK(sim_count_outside_lit(&s) == 0);
  {
    /* the house mark (blue) is lit once complete: some pixel inside the ring is blue-dominant */
    int blue = 0, cy = c - ru_u(9);
    for (int y = cy - ru_u(8); y <= cy + ru_u(8); y++)
      for (int x = c - ru_u(8); x <= c + ru_u(8); x++) {
        uint32_t v = px(&s, x, y);
        if ((v & 0xFF) > 0x80 && (v & 0xFF) > ((v >> 16) & 0xFF) + 0x60) blue++;
      }
    CHECK(blue > 50);
  }
  sim_advance(5000); /* the bead keeps orbiting: still alive, still covering */
  sim_render();
  CHECK(px(&s, c, c) != 0xFFFFFF && sim_count_outside_lit(&s) == 0);

  /* dismiss with a fade: the underlay returns and nothing is left on the top layer */
  ru_splash_dismiss(o, 200);
  CHECK(!lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)); /* usable during the fade */
  sim_advance(400);
  sim_render();
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
  CHECK(px(&s, c, c) == 0xFFFFFF);

  /* deleting mid-animation must not leave a dangling animation (ASAN/UBSAN would flag a use-after-free) */
  lv_obj_t *m = ru_splash_overlay_create(&cfg);
  ru_splash_start(m);
  sim_advance(300);
  lv_obj_delete(m);
  sim_advance(3000);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);

  /* dismiss with 0 ms deletes at once; NULL is always safe */
  lv_obj_t *z = ru_splash_overlay_create(NULL); /* default texts */
  CHECK(z != NULL);
  ru_splash_dismiss(z, 0);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
  ru_splash_start(NULL);
  ru_splash_set_progress(NULL, 500);
  ru_splash_set_error(NULL, "x", "y");
  ru_splash_dismiss(NULL, 100);
  CHECK(!ru_splash_is_complete(NULL));

  /* error variant: complete at once, red ring, progress calls no longer change it, no animation resumes it */
  lv_obj_t *e = ru_splash_overlay_create(&cfg);
  ru_splash_start(e);
  sim_advance(200);
  ru_splash_set_error(e, "E12 DISPLAY", "HOLD TO RECOVER");
  CHECK(ru_splash_is_complete(e));
  sim_render();
  long err_lit = sim_count_lit(&s);
  ru_splash_set_progress(e, 100); /* must be ignored */
  ru_splash_start(e);             /* must be ignored */
  sim_advance(2000);
  sim_render();
  CHECK(sim_count_lit(&s) == err_lit);
  CHECK(sim_count_outside_lit(&s) == 0);
  ru_splash_set_error(e, "E13", NULL); /* a second error just updates the text */
  sim_render();
  ru_splash_dismiss(e, 0);

  /* a screen-sized splash (not overlay) also builds and frees */
  lv_obj_t *scr = ru_splash_create(&cfg);
  CHECK(scr != NULL);
  ru_splash_set_progress(scr, 1000);
  CHECK(ru_splash_is_complete(scr));
  ru_splash_set_progress(scr, -50);   /* clamped */
  CHECK(!ru_splash_is_complete(scr));
  ru_splash_set_progress(scr, 99999); /* clamped */
  CHECK(ru_splash_is_complete(scr));
  lv_obj_delete(scr);
  sim_shutdown(&s); /* a second sim_init in the same process needs a clean display */
}

int main(void) {
  run(466);
  run(480);
  MT_DONE();
}
