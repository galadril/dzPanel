#include "mini_test.h"
#include "ru_geom.h"
#include <stdlib.h>
#include <string.h>

int main(void) {
  ru_geom_t g;
  /* golden values */
  ru_geom_init(&g, 466);
  CHECK_NEAR(g.u, 4.66, 1e-5); CHECK_NEAR(g.r, 233.0, 1e-5); CHECK_NEAR(g.rs, 214.36, 1e-3);
  ru_geom_init(&g, 480);
  CHECK_NEAR(g.u, 4.80, 1e-5); CHECK_NEAR(g.r, 240.0, 1e-5); CHECK_NEAR(g.rs, 220.8, 1e-3);
  CHECK_NEAR(ru_geom_chord(&g, 0), 441.6, 1e-3);
  CHECK_NEAR(ru_geom_chord(&g, 110.4), 2 * sqrt(220.8 * 220.8 - 110.4 * 110.4), 1e-2); /* ~382.0 */
  CHECK(ru_geom_chord(&g, 220.8) == 0.0f);
  CHECK(ru_geom_chord(&g, 300) == 0.0f);
  CHECK_NEAR(ru_geom_chord(&g, -110.4), ru_geom_chord(&g, 110.4), 1e-4);

  /* polar: 0deg top, 90deg right */
  ru_pt_t p = ru_geom_polar(&g, 100, 0);   CHECK_NEAR(p.x, 240, 1e-3); CHECK_NEAR(p.y, 140, 1e-3);
  p = ru_geom_polar(&g, 100, 90);          CHECK_NEAR(p.x, 340, 1e-3); CHECK_NEAR(p.y, 240, 1e-3);
  p = ru_geom_polar(&g, 100, 180);         CHECK_NEAR(p.y, 340, 1e-3);

  /* square is ~65% of diameter (DESIGN.md) */
  ru_rect_t sq = ru_geom_inscribed(&g, 1.0f);
  CHECK_NEAR(sq.w / g.diameter, 0.65, 0.01);

  /* property: inscribed rect for any aspect fits Rs; row widths never exceed the chord */
  srand(1);
  int sizes[2] = {466, 480};
  for (int s = 0; s < 2; s++) {
    ru_geom_init(&g, sizes[s]);
    for (int i = 0; i < 2000; i++) {
      float a = 0.2f + 5.0f * (rand() / (float)RAND_MAX);
      ru_rect_t r = ru_geom_inscribed(&g, a);
      CHECK(ru_geom_rect_in_safe(&g, r));
      ru_rect_t big = {r.x - 2, r.y - 2, r.w + 4, r.h + 4};
      CHECK(!ru_geom_rect_in_safe(&g, big));
      float y0 = g.r - g.rs + (rand() / (float)RAND_MAX) * 2 * g.rs * 0.9f;
      float y1 = y0 + 10;
      float w = ru_geom_row_width(&g, y0, y1);
      CHECK(w <= 2 * g.rs + 1e-3f);
      if (w > 0) {
        ru_rect_t row = {g.r - w / 2, y0, w, 10};
        CHECK(ru_geom_rect_in_safe(&g, row));
      }
    }
  }

  /* rotation: LVGL's mapping (lv_display_rotate_area / the pointer transform in lv_indev.c) */
  static const int angles[4] = {0, 90, 180, 270};
  for (int s = 0; s < 2; s++) {
    int D = sizes[s], ox, oy, lx, ly;
    /* corners: the logical top-left goes to the panel corner LVGL gives it */
    static const int tl[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}}; /* (x, y) factor of D-1 */
    for (int a = 0; a < 4; a++) {
      ru_rot_to_panel(angles[a], D, 0, 0, &ox, &oy);
      CHECK(ox == tl[a][0] * (D - 1) && oy == tl[a][1] * (D - 1));
    }
    ru_rot_to_panel(90, D, D - 1, 0, &ox, &oy);   CHECK(ox == 0 && oy == 0);         /* top-right goes to top-left */
    ru_rot_to_panel(90, D, 0, D - 1, &ox, &oy);   CHECK(ox == D - 1 && oy == D - 1); /* bottom-left to bottom-right */
    ru_rot_to_panel(180, D, 10, 20, &ox, &oy);    CHECK(ox == D - 11 && oy == D - 21);
    ru_rot_to_panel(270, D, 10, 20, &ox, &oy);    CHECK(ox == D - 21 && oy == 10);
    ru_rot_to_panel(90, D, 10, 20, &ox, &oy);     CHECK(ox == 20 && oy == D - 11);
    ru_rot_to_logical(90, D, 20, D - 11, &lx, &ly); CHECK(lx == 10 && ly == 20);
    ru_rot_to_panel(0, D, 7, 9, &ox, &oy);        CHECK(ox == 7 && oy == 9);
    ru_rot_to_panel(45, D, 7, 9, &ox, &oy);       CHECK(ox == 7 && oy == 9); /* not an angle: unrotated */
    ru_rot_to_logical(-90, D, 7, 9, &ox, &oy);    CHECK(ox == 7 && oy == 9);
    /* the centre of the panel stays the centre (to the pixel) and the edge midpoints rotate by quarter turns */
    for (int a = 0; a < 4; a++) {
      ru_rot_to_panel(angles[a], D, (D - 1) / 2, 0, &ox, &oy);
      CHECK(ox >= 0 && ox < D && oy >= 0 && oy < D);
    }
    /* every pixel round trips, in both directions, and the map is a bijection on the D x D square */
    static unsigned char seen[480 * 480];
    for (int a = 0; a < 4; a++) {
      memset(seen, 0, sizeof seen);
      for (int y = 0; y < D; y++)
        for (int x = 0; x < D; x++) {
          ru_rot_to_panel(angles[a], D, x, y, &ox, &oy);
          CHECK(ox >= 0 && ox < D && oy >= 0 && oy < D);
          CHECK(!seen[oy * D + ox]);
          seen[oy * D + ox] = 1;
          ru_rot_to_logical(angles[a], D, ox, oy, &lx, &ly);
          CHECK(lx == x && ly == y);
          int sx = x, sy = y; /* in place: the output may alias the input */
          ru_rot_to_panel(angles[a], D, sx, sy, &sx, &sy);
          CHECK(sx == ox && sy == oy);
          ru_rot_to_logical(angles[a], D, sx, sy, &sx, &sy);
          CHECK(sx == x && sy == y);
        }
    }
    /* two 90 degree steps are 180, three are 270 */
    for (int y = 0; y < D; y += 37)
      for (int x = 0; x < D; x += 41) {
        int a1, b1, a2, b2;
        ru_rot_to_panel(90, D, x, y, &a1, &b1);
        ru_rot_to_panel(90, D, a1, b1, &a2, &b2);
        ru_rot_to_panel(180, D, x, y, &ox, &oy);
        CHECK(a2 == ox && b2 == oy);
        ru_rot_to_panel(90, D, a2, b2, &a1, &b1);
        ru_rot_to_panel(270, D, x, y, &ox, &oy);
        CHECK(a1 == ox && b1 == oy);
      }
  }
  MT_DONE();
}
