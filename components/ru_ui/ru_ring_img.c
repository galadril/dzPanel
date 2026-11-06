#include "ru_ring_img.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *big_alloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}
#define big_free heap_caps_free
#else
static void *big_alloc(size_t n) { return malloc(n); }
#define big_free free
#endif

#define PI_F 3.14159265f

static struct {
  lv_image_dsc_t dsc;
  uint16_t *buf;
  size_t cap;
  int d, w, kind;
  bool valid;
  uint32_t gen;
} S;

uint32_t ru_ring_img_generations(void) { return S.gen; }

void ru_ring_img_free(void) {
  if (S.buf) big_free(S.buf);
  memset(&S, 0, sizeof S);
}

static const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static uint16_t pack565(uint32_t rgb, int x, int y, bool dither) {
  int r = (int)(rgb >> 16 & 255u), g = (int)(rgb >> 8 & 255u), b = (int)(rgb & 255u);
  if (dither) { /* ordered dither: the quantisation step is 8 (R, B) and 4 (G); add a fraction of it before truncating */
    int t = BAYER[y & 3][x & 3];
    r += t / 2; g += t / 4; b += t / 2;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
  }
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

const lv_image_dsc_t *ru_ring_img_get(int d, int w, ru_ring_kind_t kind, ru_ring_color_fn fn) {
  if (d < 8 || w < 1 || w > d / 2 || !fn) return NULL;
  if (S.valid && S.d == d && S.w == w && S.kind == (int)kind) return &S.dsc;
  size_t need = (size_t)d * (size_t)d * 2u;
  if (S.buf && S.cap < need) { big_free(S.buf); S.buf = NULL; S.cap = 0; }
  if (!S.buf) {
    S.buf = big_alloc(need);
    if (!S.buf) { S.valid = false; return NULL; }
    S.cap = need;
  }
  S.valid = false;
  memset(S.buf, 0, need);
  /* colour per 1/512 of the sweep: one call of fn each, not one per pixel */
  enum { LUT_N = 513 };
  uint32_t lut[LUT_N];
  for (int i = 0; i < LUT_N; i++) lut[i] = fn((float)i / (float)(LUT_N - 1));
  float R = (float)d / 2.0f, r_in = R - (float)w - 1.0f, r_out = R + 1.0f;
  float c = (float)d / 2.0f;
  bool dither = kind == RU_RING_TEMP;
  for (int y = 0; y < d; y++) {
    float dy = (float)y + 0.5f - c;
    for (int x = 0; x < d; x++) {
      float dx = (float)x + 0.5f - c;
      float rr2 = dx * dx + dy * dy;
      if (rr2 < r_in * r_in || rr2 > r_out * r_out) continue;
      float deg = atan2f(dy, dx) * 180.0f / PI_F; /* 0 = 3 o'clock, clockwise (y points down) */
      if (deg < 0.0f) deg += 360.0f;
      float p = (deg - 135.0f) / 270.0f;
      if (p < 0.0f) p += 360.0f / 270.0f; /* 0..360 -> 0 .. 1.333: the part past 1 is the gap and the round caps */
      if (p > 1.0f) p = p < 1.0f + (1.0f / 3.0f) * 0.5f ? 1.0f : 0.0f; /* the gap: the nearer end */
      S.buf[y * d + x] = pack565(lut[(int)lroundf(p * (float)(LUT_N - 1))], x, y, dither);
    }
  }
  memset(&S.dsc, 0, sizeof S.dsc);
  S.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  S.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  S.dsc.header.w = (uint32_t)d;
  S.dsc.header.h = (uint32_t)d;
  S.dsc.header.stride = (uint32_t)d * 2u;
  S.dsc.data_size = need;
  S.dsc.data = (const uint8_t *)S.buf;
  S.d = d; S.w = w; S.kind = (int)kind;
  S.valid = true;
  S.gen++;
  return &S.dsc;
}
