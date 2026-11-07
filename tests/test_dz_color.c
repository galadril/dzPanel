#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dz_color.h"
#include "mini_test.h"

static void test_hsb(void) {
  uint8_t r, g, b;
  static const struct { float h; uint8_t r, g, b; } T[] = {
    { 0, 255, 0, 0 }, { 60, 255, 255, 0 }, { 120, 0, 255, 0 }, { 180, 0, 255, 255 }, { 240, 0, 0, 255 }, { 300, 255, 0, 255 },
    { 30, 255, 128, 0 }, { 360, 255, 0, 0 }, { -60, 255, 0, 255 }, { 420, 255, 255, 0 } };
  for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
    dz_hsb2rgb(T[i].h, &r, &g, &b);
    CHECK(r == T[i].r && g == T[i].g && b == T[i].b);
  }
  /* round trip over the whole circle */
  for (int h = 0; h < 360; h++) {
    float back, sat;
    dz_hsb2rgb((float)h, &r, &g, &b);
    CHECK(dz_rgb_to_hue(r, g, b, &back, &sat));
    CHECK(dz_hue_dist(back, (float)h) <= 1.0f && sat > 0.99f);
  }
}

static void test_hue_edges(void) {
  float h = -1, s;
  CHECK(!dz_rgb_to_hue(0, 0, 0, &h, &s) && h == -1);          /* black: no hue */
  CHECK(!dz_rgb_to_hue(255, 255, 255, &h, &s));                /* white */
  CHECK(!dz_rgb_to_hue(255, 230, 220, &h, &s));                /* pale: saturation < 0.15 */
  CHECK(dz_rgb_to_hue(255, 0, 0, &h, &s) && h == 0.0f && s == 1.0f);
  CHECK(dz_rgb_to_hue(255, 0, 1, &h, &s) && h > 358.0f && h < 360.0f);
  CHECK(dz_rgb_to_hue(10, 5, 0, &h, &s) && dz_hue_dist(h, 30) < 1);
  CHECK_NEAR(dz_hue_dist(350, 10), 20, 1e-4);
  CHECK_NEAR(dz_hue_dist(10, 350), 20, 1e-4);
  CHECK_NEAR(dz_hue_dist(0, 180), 180, 1e-4);
  CHECK_NEAR(dz_hue_dist(90, 90), 0, 1e-4);
}

static void test_temp(void) {
  CHECK(dz_kelvin_to_warmth(2700) == 100 && dz_kelvin_to_warmth(6500) == 0 && dz_kelvin_to_warmth(4600) == 50);
  CHECK(dz_kelvin_to_warmth(1000) == 100 && dz_kelvin_to_warmth(9000) == 0);
  CHECK(dz_warmth_to_t(dz_kelvin_to_warmth(2700)) == 255);
  CHECK(dz_warmth_to_t(dz_kelvin_to_warmth(4600)) == 128);
  CHECK(dz_warmth_to_t(dz_kelvin_to_warmth(6500)) == 0);
  CHECK(dz_warmth_to_kelvin(100) == 2700 && dz_warmth_to_kelvin(0) == 6500 && dz_warmth_to_kelvin(50) == 4600);
  CHECK(dz_warmth_to_kelvin(-5) == 6500 && dz_warmth_to_kelvin(500) == 2700);
  CHECK(dz_t_to_warmth(157) == 62 && dz_t_to_warmth(255) == 100 && dz_t_to_warmth(0) == 0 && dz_t_to_warmth(999) == 100);
  /* warmth <-> t round trip within one step */
  for (int w = 0; w <= 100; w++) CHECK(dz_t_to_warmth(dz_warmth_to_t(w)) == w);
  /* gradient ends and middle */
  CHECK(dz_temp_rgb(0.0f) == 0xFF9F45u && dz_temp_rgb(0.5f) == 0xFFE3BFu && dz_temp_rgb(1.0f) == 0xD6E6FFu);
  CHECK(dz_temp_rgb(-3.0f) == 0xFF9F45u && dz_temp_rgb(7.0f) == 0xD6E6FFu);
  CHECK(dz_warmth_rgb(100) == 0xFF9F45u && dz_warmth_rgb(0) == 0xD6E6FFu);
}

static void test_words(void) {
  CHECK(!strcmp(dz_hue_word(0), "RED") && !strcmp(dz_hue_word(359), "RED") && !strcmp(dz_hue_word(25), "ORANGE") &&
        !strcmp(dz_hue_word(55), "YELLOW") && !strcmp(dz_hue_word(120), "GREEN") && !strcmp(dz_hue_word(180), "CYAN") &&
        !strcmp(dz_hue_word(240), "BLUE") && !strcmp(dz_hue_word(270), "PURPLE") && !strcmp(dz_hue_word(320), "PINK") &&
        !strcmp(dz_hue_word(-10), "RED") && !strcmp(dz_hue_word(480), "GREEN"));
  CHECK(!strcmp(dz_warmth_word(100), "WARM") && !strcmp(dz_warmth_word(62), "SOFT") && !strcmp(dz_warmth_word(50), "NEUTRAL") &&
        !strcmp(dz_warmth_word(20), "COOL") && !strcmp(dz_warmth_word(0), "DAYLIGHT"));
}

static void test_hsb_nonfinite(void) {
  uint8_t r, g, b, r0, g0, b0;
  dz_hsb2rgb(0.0f, &r0, &g0, &b0);
  float bad[] = { NAN, INFINITY, -INFINITY };
  for (int i = 0; i < 3; i++) {
    dz_hsb2rgb(bad[i], &r, &g, &b);
    CHECK(r == r0 && g == g0 && b == b0); /* treated as hue 0 */
  }
}

static void test_mode(void) {
  dz_device_t d;
  memset(&d, 0, sizeof d);
  CHECK(dz_light_mode(NULL) == DZ_LM_NONE && dz_light_mode(&d) == DZ_LM_NONE);
  d.has_color = true;
  static const dz_light_mode_t M[] = { DZ_LM_NONE, DZ_LM_WHITE, DZ_LM_TEMP, DZ_LM_COLOR, DZ_LM_COLOR, DZ_LM_NONE };
  for (int m = 0; m < 6; m++) { d.col_m = (uint8_t)m; CHECK(dz_light_mode(&d) == M[m]); }
}

int main(void) {
  test_hsb();
  test_hsb_nonfinite();
  test_hue_edges();
  test_temp();
  test_words();
  test_mode();
  MT_DONE();
}
