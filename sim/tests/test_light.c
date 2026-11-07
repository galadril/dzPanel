/* Colour ring image and the light widget (ru_light_*). Runs at 466 round, 480 round and 480 square. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dz_color.h"
#include "mini_test.h"
#include "ru_ring_img.h"
#include "ru_widgets.h"
#include "sim_core.h"

static uint32_t hue_fn(float p) { uint8_t r, g, b; dz_hsb2rgb(p * 360.0f, &r, &g, &b); return (uint32_t)r << 16 | (uint32_t)g << 8 | b; }
static uint32_t temp_fn(float p) { return dz_temp_rgb(p); }

static uint32_t px565(const lv_image_dsc_t *im, int x, int y) {
  const uint16_t *b = (const uint16_t *)im->data;
  return b[y * (int)im->header.w + x];
}
static uint32_t at(const lv_image_dsc_t *im, int d, float r, float deg_cw_from_3) {
  float a = deg_cw_from_3 * 3.14159265f / 180.0f;
  int x = (int)lroundf((float)d / 2.0f + r * cosf(a)), y = (int)lroundf((float)d / 2.0f + r * sinf(a));
  return px565(im, x, y);
}
static void rgb565(uint32_t v, int *r, int *g, int *b) { *r = (int)((v >> 11) & 31) * 255 / 31; *g = (int)((v >> 5) & 63) * 255 / 63; *b = (int)(v & 31) * 255 / 31; }

static void test_ring_img(int size) {
  sim_t S;
  CHECK(sim_init_ex(&S, size, false));
  ru_ring_img_free();
  int d = ru_u(62), w = ru_u(5);
  uint32_t g0 = ru_ring_img_generations();
  const lv_image_dsc_t *im = ru_ring_img_get(d, w, RU_RING_HUE, hue_fn);
  CHECK(im && im->header.w == (uint32_t)d && im->header.h == (uint32_t)d && im->header.cf == LV_COLOR_FORMAT_RGB565 && im->header.stride == (uint32_t)d * 2);
  CHECK(ru_ring_img_generations() == g0 + 1);
  float rm = (float)d / 2 - (float)w / 2; /* mid radius of the band */
  int r, g, b;
  /* hue ring: 270 degrees of arc = hue 0..360; left (180) = yellow (p 1/6), upper left (225) = green, top (270) = cyan,
   * upper right (315) = blue, right (0) = magenta; start and end are red */
  rgb565(at(im, d, rm, 180), &r, &g, &b); CHECK(r > 230 && g > 230 && b < 20);
  rgb565(at(im, d, rm, 225), &r, &g, &b); CHECK(g > 230 && r < 20 && b < 20);
  rgb565(at(im, d, rm, 270), &r, &g, &b); CHECK(g > 230 && b > 230 && r < 20);
  rgb565(at(im, d, rm, 315), &r, &g, &b); CHECK(b > 230 && r < 20 && g < 20);
  rgb565(at(im, d, rm, 0), &r, &g, &b); CHECK(r > 230 && b > 230 && g < 20);
  rgb565(at(im, d, rm, 135.5f), &r, &g, &b); CHECK(r > 230 && g < 20 && b < 20);   /* start: red */
  rgb565(at(im, d, rm, 44.5f), &r, &g, &b); CHECK(r > 230 && g < 20 && b < 40);    /* end: red (magenta side) */
  /* outside the annulus and the hole stay transparent (zero), the centre too */
  CHECK(at(im, d, rm - (float)w, 270) == 0 && at(im, d, 4, 270) == 0 && px565(im, d / 2, d / 2) == 0 && px565(im, 0, 0) == 0);
  /* the same request again: cached, no new generation */
  CHECK(ru_ring_img_get(d, w, RU_RING_HUE, hue_fn) == im && ru_ring_img_generations() == g0 + 1);
  /* temperature: one buffer reused (same pointer), regenerated; ends amber and cool, dithered middle is cream */
  const uint16_t *buf = (const uint16_t *)im->data;
  const lv_image_dsc_t *im2 = ru_ring_img_get(d, w, RU_RING_TEMP, temp_fn);
  CHECK(im2 && (const uint16_t *)im2->data == buf && ru_ring_img_generations() == g0 + 2);
  rgb565(at(im2, d, rm, 150), &r, &g, &b); CHECK(r > 240 && g > 135 && g < 185 && b < 100);     /* left end: amber FF9F45 */
  rgb565(at(im2, d, rm, 270), &r, &g, &b); CHECK(r > 240 && g > 205 && b > 160 && b < 215);     /* top: cream FFE3BF */
  rgb565(at(im2, d, rm, 30), &r, &g, &b); CHECK(b > 235 && r > 190 && r < 235);                 /* right end: cool D6E6FF */
  CHECK(ru_ring_img_get(d, w, RU_RING_HUE, hue_fn) && ru_ring_img_generations() == g0 + 3);
  CHECK(!ru_ring_img_get(4, 2, RU_RING_HUE, hue_fn) && !ru_ring_img_get(d, w, RU_RING_HUE, NULL));
  /* allocation failure: NULL (no fallback memory), nothing generated; works again afterwards */
  ru_ring_img_free();
  ru_ring_img_test_fail_alloc(true);
  uint32_t gf = ru_ring_img_generations();
  CHECK(ru_ring_img_get(d, w, RU_RING_HUE, hue_fn) == NULL && ru_ring_img_generations() == gf);
  CHECK(ru_ring_img_get(d, w, RU_RING_TEMP, temp_fn) == NULL);
  ru_ring_img_test_fail_alloc(false);
  CHECK(ru_ring_img_get(d, w, RU_RING_HUE, hue_fn) != NULL);
  ru_ring_img_free();
  sim_shutdown(&S);
}

/* ---- widget ---- */

typedef struct { int n, finals; ru_light_cmd_t last; int lastv; bool lastfinal; int cmd_count[5]; int nonfinal; } rec_t;
static void on_cmd(lv_obj_t *w, ru_light_cmd_t c, int v, bool fin, void *ud) {
  (void)w;
  rec_t *r = ud;
  r->n++;
  r->last = c; r->lastv = v; r->lastfinal = fin;
  if (fin) { r->finals++; r->cmd_count[c]++; } else r->nonfinal++;
}

static const char *hue_word_f(float h) { return dz_hue_word(h); }
static const char *warm_word_f(int w) { return dz_warmth_word(w); }
static const ru_light_hooks_t HK = { hue_fn, temp_fn, hue_word_f, warm_word_f };

static int count_desc(lv_obj_t *o) {
  int n = 0;
  uint32_t c = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < c; i++) n += 1 + count_desc(lv_obj_get_child(o, (int32_t)i));
  return n;
}

static ru_light_state_t on_state(int mode, int level) {
  ru_light_state_t s;
  memset(&s, 0, sizeof s);
  s.known = true; s.on = true; s.level = level; s.mode = mode;
  s.has_hue = mode == RU_LIGHT_COLOR; s.hue = 30; s.warmth = 62; s.color = mode == RU_LIGHT_COLOR ? 0xFF8000 : 0;
  return s;
}

static void ring_xy(int size, float r_u, float deg, int *x, int *y) {
  float a = deg * 3.14159265f / 180.0f, r = r_u * (float)size / 100.0f;
  *x = (int)lroundf((float)size / 2.0f + r * cosf(a));
  *y = (int)lroundf((float)size / 2.0f + r * sinf(a));
}

static void touch(int x, int y, bool down) { sim_pointer(x, y, down); sim_advance(50); }

static void run_widget(int size, bool square) {
  sim_t S;
  CHECK(sim_init_ex(&S, size, false));
  ru_ring_img_free();
  lv_obj_t *scr = ru_screen_new();
  lv_screen_load(scr);
  int d = ru_u(86);
  printf("  %d %s: ring %d px\n", size, square ? "square" : "round", d);
  static const int CAPS[] = { 0, 1, 3, 7, 4 };
  static const int CHIPS_SQ[] = { 0, 0, 2, 2, 0 };
  static const int CHIPS_RD[] = { 0, 2, 3, 3, 2 };
  for (size_t i = 0; i < 5; i++) {
    ru_light_cfg_t cfg = { CAPS[i], square, 100, &HK };
    lv_obj_t *w = ru_light_create(scr, d, &cfg);
    ru_light_state_t st = on_state(CAPS[i] & 1 ? RU_LIGHT_COLOR : RU_LIGHT_TEMP, 70);
    ru_light_set(w, &st);
    lv_obj_update_layout(scr);
    int n = count_desc(w);
    if (n > 24) printf("    caps %d: %d objects\n", CAPS[i], n);
    CHECK(n <= 24);
    CHECK(ru_light_chips(w) == (square ? CHIPS_SQ[i] : CHIPS_RD[i]));
    /* everything inside the inscribed circle, the chips and the disc too */
    float R = (float)size / 2.0f;
    for (int p = 1; p <= 8; p++) {
      lv_obj_t *o = ru_light_part(w, p);
      if (!o || p == 6) continue;
      lv_area_t a;
      lv_obj_get_coords(o, &a);
      float cx = (float)(a.x1 + a.x2 + 1) / 2 - R, cy = (float)(a.y1 + a.y2 + 1) / 2 - R;
      float hw = (float)(a.x2 - a.x1 + 1) / 2, hh = (float)(a.y2 - a.y1 + 1) / 2;
      if (p == 1 || p == 2) continue; /* the rings: arcs are round, their boxes are squares */
      float fx = fabsf(cx) + hw, fy = fabsf(cy) + hh;
      CHECK(sqrtf(fx * fx + fy * fy) <= R);
    }
    lv_obj_delete(w);
  }
  sim_render();
  ru_ring_img_free();
  sim_shutdown(&S);
}

static void dump(sim_t *S, const char *tag, int size, bool square) {
  const char *dir = getenv("LIGHT_SHOT_DIR");
  if (!dir) return;
  char path[256];
  snprintf(path, sizeof path, "%s/light_%s_%d%s.png", dir, tag, size, square ? "sq" : "");
  sim_render();
  sim_save_png(S, path, true);
}

static void run_touch(int size, bool square) {
  sim_t S;
  CHECK(sim_init_ex(&S, size, false));
  ru_ring_img_free();
  lv_obj_t *scr = ru_screen_new();
  lv_screen_load(scr);
  int d = ru_u(86);
  float bri_mid = square ? 43.0f : 43.0f - 2.2f, col_mid = square ? 28.5f : bri_mid;
  printf("  %d %s: touch\n", size, square ? "square" : "round");
  ru_light_cfg_t cfg = { 7, square, 100, &HK };
  lv_obj_t *w = ru_light_create(scr, d, &cfg);
  rec_t rec;
  memset(&rec, 0, sizeof rec);
  ru_light_on_change(w, on_cmd, &rec);
  ru_light_state_t st = on_state(RU_LIGHT_COLOR, 70);
  ru_light_set(w, &st);
  sim_render();
  dump(&S, "color", size, square);
  int cx = size / 2, cy = size / 2, x, y;

  /* colour ring (round: first switch the single ring with the COLOUR chip, which sends nothing) */
  if (!square) { ru_light_press_chip(w, 1); CHECK(rec.n == 0 && ru_light_view(w) == RU_RING_HUE); }
  CHECK(ru_light_view(w) == RU_RING_HUE);
  uint32_t g0 = ru_ring_img_generations();
  /* hue 120 = 1/3 of the sweep = 90 degrees after the start (135): 225 */
  ring_xy(size, col_mid, 225, &x, &y);
  touch(x, y, true);
  CHECK(rec.finals == 0);
  ring_xy(size, col_mid, 226, &x, &y);
  touch(x, y, true);
  CHECK(rec.finals == 0 && rec.nonfinal >= 1);
  dump(&S, "drag", size, square);
  ring_xy(size, col_mid, 225, &x, &y);
  touch(x, y, true);
  touch(x, y, false);
  CHECK(rec.finals == 1 && rec.last == RU_LIGHT_CMD_HUE && rec.lastfinal && abs(rec.lastv - 120) <= 2);
  if (!(rec.finals == 1 && abs(rec.lastv - 120) <= 2)) printf("    hue cmd: finals %d last %d v %d\n", rec.finals, (int)rec.last, rec.lastv);
  /* after the release the knob stays on the released value (pending) until the model moves */
  lv_obj_t *carc = ru_light_part(w, 2);
  CHECK(abs(lv_arc_get_value(carc) - 120) <= 2);
  dump(&S, "released", size, square);
  st.hue = 120; ru_light_set(w, &st);
  CHECK(abs(lv_arc_get_value(carc) - 120) <= 1);
  /* the disc text follows the finger; the large text still the brightness */
  CHECK(!strcmp(lv_label_get_text(ru_light_part(w, 4)), "70%"));
  CHECK(!strcmp(lv_label_get_text(ru_light_part(w, 5)), "GREEN"));
  dump(&S, "green", size, square);

  /* brightness ring: 50 % = half the sweep = 135 + 135 = 270 (top) */
  int before = rec.finals;
  ring_xy(size, bri_mid, 270, &x, &y);
  if (!square) { ru_light_press_chip(w, 0); CHECK(ru_light_view(w) == -1); }
  touch(x, y, true);
  touch(x, y, false);
  CHECK(rec.finals == before + 1 && rec.last == RU_LIGHT_CMD_LEVEL && abs(rec.lastv - 50) <= 2);

  /* the disc: a tap switches off (the lamp is on), a drag across it sends nothing, a press in the dead zone sends nothing */
  before = rec.finals;
  touch(cx, cy, true);
  touch(cx, cy, false);
  CHECK(rec.finals == before + 1 && rec.last == RU_LIGHT_CMD_POWER && rec.lastv == 0);
  before = rec.finals;
  touch(cx - 20, cy, true);
  touch(cx + 20, cy, true);
  touch(cx + 20, cy, false);
  CHECK(rec.finals == before);
  ring_xy(size, 43.0f, 90, &x, &y); /* straight down: the gap of the sweep, outside the +- 8 degrees */
  touch(x, y, true);
  touch(x, y, false);
  CHECK(rec.finals == before);
  CHECK(ru_light_ring_at(w, cx - (int)(40.0f * (float)size / 100.0f), cy) && !ru_light_ring_at(w, cx, cy) && !ru_light_ring_at(w, x, y));
  /* off: the tap switches on */
  st.on = false; ru_light_set(w, &st);
  CHECK(!strcmp(lv_label_get_text(ru_light_part(w, 4)), "OFF"));
  dump(&S, "off", size, square);
  touch(cx, cy, true);
  touch(cx, cy, false);
  CHECK(rec.last == RU_LIGHT_CMD_POWER && rec.lastv == 1);
  st.on = true; ru_light_set(w, &st);

  /* chips swap the ring, send nothing; the image is generated once per kind change */
  if (square) {
    before = rec.n;
    g0 = ru_ring_img_generations();
    ru_light_press_chip(w, 1); /* WHITE */
    CHECK(ru_light_view(w) == RU_RING_TEMP && rec.n == before && ru_ring_img_generations() == g0 + 1);
    dump(&S, "white", size, square);
    ru_light_press_chip(w, 0);
    CHECK(ru_light_view(w) == RU_RING_HUE && rec.n == before && ru_ring_img_generations() == g0 + 2);
    ru_light_press_chip(w, 0);
    CHECK(ru_ring_img_generations() == g0 + 2);
  }

  /* feedback and read only */
  ru_light_set_feedback(w, RU_LIGHT_FB_SENDING, "SENDING");
  CHECK(!strcmp(lv_label_get_text(ru_light_part(w, 5)), "SENDING"));
  ru_light_set_feedback(w, RU_LIGHT_FB_NONE, "");
  ru_light_set_readonly(w, true, "PROTECTED");
  CHECK(!strcmp(lv_label_get_text(ru_light_part(w, 5)), "PROTECTED"));
  CHECK(ru_light_part(w, 7) && lv_obj_has_flag(ru_light_part(w, 7), LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(ru_light_part(w, 6), LV_OBJ_FLAG_CLICKABLE));
  before = rec.n;
  touch(cx, cy, true);
  touch(cx, cy, false);
  ring_xy(size, bri_mid, 270, &x, &y);
  touch(x, y, true);
  touch(x, y, false);
  CHECK(rec.n == before);
  dump(&S, "protected", size, square);
  ru_light_set_readonly(w, false, "");

  /* the same state again invalidates nothing */
  ru_light_set(w, &st);
  sim_advance(100);
  sim_inv_reset();
  for (int i = 0; i < 3; i++) { ru_light_set(w, &st); ru_light_set_feedback(w, RU_LIGHT_FB_NONE, ""); ru_light_set_readonly(w, false, ""); }
  sim_advance(100);
  CHECK(sim_inv_pixels() == 0);
  /* a brightness change redraws a part of the screen, not all of it */
  st.level = 40;
  ru_light_set(w, &st);
  sim_advance(100);
  CHECK(sim_inv_pixels() > 0 && sim_inv_pixels() < (long)size * size * 4 / 10);
  if (sim_inv_pixels() >= (long)size * size * 4 / 10) printf("    level change: %ld px\n", sim_inv_pixels());
  lv_obj_clean(scr);
  ru_ring_img_free();
  sim_shutdown(&S);
}

int main(void) {
  test_ring_img(466);
  test_ring_img(480);
  run_widget(466, false);
  run_widget(480, false);
  run_widget(480, true);
  run_touch(466, false);
  run_touch(480, false);
  run_touch(480, true);
  MT_DONE();
}
