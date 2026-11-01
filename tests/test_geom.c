#include "mini_test.h"
#include "ru_geom.h"
#include <stdlib.h>

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
  MT_DONE();
}
