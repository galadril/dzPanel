#include "dz_color.h"

#include <math.h>

dz_light_mode_t dz_light_mode(const dz_device_t *d) {
  if (!d || !d->has_color) return DZ_LM_NONE;
  switch (d->col_m) {
    case 1: return DZ_LM_WHITE;
    case 2: return DZ_LM_TEMP;
    case 3: case 4: return DZ_LM_COLOR;
    default: return DZ_LM_NONE;
  }
}

void dz_hsb2rgb(float hue, uint8_t *r, uint8_t *g, uint8_t *b) {
  hue = fmodf(hue, 360.0f);
  if (hue < 0.0f) hue += 360.0f;
  float h = hue / 60.0f;
  int i = (int)floorf(h);
  float f = h - (float)i;
  float q = 1.0f - f, t = f; /* p = 0 for saturation 1, value 1 */
  float rr, gg, bb;
  switch (i % 6) {
    case 0: rr = 1; gg = t; bb = 0; break;
    case 1: rr = q; gg = 1; bb = 0; break;
    case 2: rr = 0; gg = 1; bb = t; break;
    case 3: rr = 0; gg = q; bb = 1; break;
    case 4: rr = t; gg = 0; bb = 1; break;
    default: rr = 1; gg = 0; bb = q; break;
  }
  *r = (uint8_t)lroundf(rr * 255.0f);
  *g = (uint8_t)lroundf(gg * 255.0f);
  *b = (uint8_t)lroundf(bb * 255.0f);
}

bool dz_rgb_to_hue(uint8_t r, uint8_t g, uint8_t b, float *hue, float *sat) {
  int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  if (mx == 0) return false;
  float s = (float)(mx - mn) / (float)mx;
  if (s < 0.15f) return false;
  float d = (float)(mx - mn), h;
  if (mx == r) h = fmodf((float)(g - b) / d, 6.0f);
  else if (mx == g) h = (float)(b - r) / d + 2.0f;
  else h = (float)(r - g) / d + 4.0f;
  h *= 60.0f;
  if (h < 0.0f) h += 360.0f;
  if (h >= 360.0f) h -= 360.0f;
  if (hue) *hue = h;
  if (sat) *sat = s;
  return true;
}

float dz_hue_dist(float a, float b) {
  float d = fabsf(fmodf(a - b, 360.0f));
  return d > 180.0f ? 360.0f - d : d;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int dz_warmth_to_kelvin(int w) { return 6500 - 38 * clampi(w, 0, 100); }
int dz_kelvin_to_warmth(int k) { return clampi((int)lroundf((6500.0f - (float)k) / 38.0f), 0, 100); }
int dz_warmth_to_t(int w) { return clampi((int)lroundf((float)clampi(w, 0, 100) * 2.55f), 0, 255); }
int dz_t_to_warmth(int t) { return clampi((int)lroundf((float)clampi(t, 0, 255) / 2.55f), 0, 100); }

static uint32_t lerp_rgb(uint32_t a, uint32_t b, float f) {
  uint32_t out = 0;
  for (int sh = 16; sh >= 0; sh -= 8) {
    float ca = (float)((a >> sh) & 255u), cb = (float)((b >> sh) & 255u);
    out |= (uint32_t)lroundf(ca + (cb - ca) * f) << sh;
  }
  return out;
}

uint32_t dz_temp_rgb(float p) {
  if (!(p > 0.0f)) p = 0.0f;
  if (p > 1.0f) p = 1.0f;
  if (p < 0.5f) return lerp_rgb(0xFF9F45u, 0xFFE3BFu, p * 2.0f);
  return lerp_rgb(0xFFE3BFu, 0xD6E6FFu, (p - 0.5f) * 2.0f);
}

uint32_t dz_warmth_rgb(int w) { return dz_temp_rgb(1.0f - (float)clampi(w, 0, 100) / 100.0f); }

const char *dz_hue_word(float h) {
  h = fmodf(h, 360.0f);
  if (h < 0.0f) h += 360.0f;
  if (h < 15.0f || h >= 345.0f) return "RED";
  if (h < 40.0f) return "ORANGE";
  if (h < 70.0f) return "YELLOW";
  if (h < 160.0f) return "GREEN";
  if (h < 200.0f) return "CYAN";
  if (h < 255.0f) return "BLUE";
  if (h < 290.0f) return "PURPLE";
  return "PINK";
}

const char *dz_warmth_word(int w) {
  w = clampi(w, 0, 100);
  if (w >= 80) return "WARM";
  if (w >= 55) return "SOFT";
  if (w >= 35) return "NEUTRAL";
  if (w >= 15) return "COOL";
  return "DAYLIGHT";
}
