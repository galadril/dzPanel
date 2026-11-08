#include "ru_geom.h"
#include <math.h>

void ru_geom_init(ru_geom_t *g, int diameter) {
  g->diameter = diameter;
  g->r = diameter / 2.0f;
  g->rs = RU_SAFE_FACTOR * g->r;
  g->u = diameter / 100.0f;
}

float ru_geom_chord(const ru_geom_t *g, float d) {
  float a = fabsf(d);
  if (a >= g->rs) return 0.0f;
  return 2.0f * sqrtf(g->rs * g->rs - a * a);
}

float ru_geom_row_width(const ru_geom_t *g, float y_top, float y_bottom) {
  float dt = y_top - g->r, db = y_bottom - g->r;
  /* worst edge = the one farthest from centre; if the row spans centre, that's still max(|dt|,|db|) */
  float d = fmaxf(fabsf(dt), fabsf(db));
  return ru_geom_chord(g, d);
}

ru_pt_t ru_geom_polar(const ru_geom_t *g, float radius, float deg) {
  float a = deg * (float)M_PI / 180.0f;
  return (ru_pt_t){g->r + radius * sinf(a), g->r - radius * cosf(a)};
}

ru_rect_t ru_geom_inscribed(const ru_geom_t *g, float aspect) {
  /* half-diagonal = Rs; w = 2 Rs a/sqrt(1+a^2), h = w/a */
  float w = 2.0f * g->rs * aspect / sqrtf(1.0f + aspect * aspect);
  float h = w / aspect;
  return (ru_rect_t){g->r - w / 2, g->r - h / 2, w, h};
}

bool ru_geom_rect_in_safe(const ru_geom_t *g, ru_rect_t r) {
  const float eps = 1e-3f;
  float xs[2] = {r.x, r.x + r.w}, ys[2] = {r.y, r.y + r.h};
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 2; j++) {
      float dx = xs[i] - g->r, dy = ys[j] - g->r;
      if (sqrtf(dx * dx + dy * dy) > g->rs + eps) return false;
    }
  return true;
}

void ru_rot_to_panel(int rot, int D, int x, int y, int *ox, int *oy) {
  int px, py;
  switch (rot) {
    case 90: px = y; py = D - 1 - x; break;
    case 180: px = D - 1 - x; py = D - 1 - y; break;
    case 270: px = D - 1 - y; py = x; break;
    default: px = x; py = y; break;
  }
  *ox = px;
  *oy = py;
}

void ru_rot_to_logical(int rot, int D, int x, int y, int *ox, int *oy) {
  int lx, ly;
  switch (rot) {
    case 90: lx = D - 1 - y; ly = x; break;
    case 180: lx = D - 1 - x; ly = D - 1 - y; break;
    case 270: lx = y; ly = D - 1 - x; break;
    default: lx = x; ly = y; break;
  }
  *ox = lx;
  *oy = ly;
}
