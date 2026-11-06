/* Colour ring image and the light widget (ru_light_*). Runs at 466 round, 480 round and 480 square. */
#include <math.h>
#include <stdio.h>
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
  ru_ring_img_free();
  sim_shutdown(&S);
}

int main(void) {
  test_ring_img(466);
  test_ring_img(480);
  MT_DONE();
}
