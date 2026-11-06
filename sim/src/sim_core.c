#include "sim_core.h"
#include "ru_theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_clock;
static bool g_real_clock;
static int g_px, g_py;
static bool g_down;
static lv_display_t *g_disp;
static lv_indev_t *g_indev;
static sim_touch_hook_t g_hook;
static void *g_hook_ud;
void sim_set_touch_hook(sim_touch_hook_t fn, void *ud) { g_hook = fn; g_hook_ud = ud; }

#ifdef RU_SIM_SDL
extern uint32_t sim_sdl_ticks(void);
#endif

static uint32_t tick_cb(void) {
#ifdef RU_SIM_SDL
  if (g_real_clock) return sim_sdl_ticks();
#endif
  return g_clock;
}

static long g_inv_px, g_inv_n, g_inv_full;
static int g_size_px;
static uint8_t *g_cover; /* pixels already invalidated since the last render: LVGL redraws the union, so only new pixels count */
static void inv_cb(lv_event_t *e) {
  const lv_area_t *a = lv_event_get_param(e);
  if (!a || !g_cover) return;
  int x1 = a->x1 < 0 ? 0 : a->x1, y1 = a->y1 < 0 ? 0 : a->y1;
  int x2 = a->x2 >= g_size_px ? g_size_px - 1 : a->x2, y2 = a->y2 >= g_size_px ? g_size_px - 1 : a->y2;
  long fresh = 0;
  for (int y = y1; y <= y2; y++)
    for (int x = x1; x <= x2; x++) {
      uint8_t *c = &g_cover[(size_t)y * (size_t)g_size_px + (size_t)x];
      if (!*c) { *c = 1; fresh++; }
    }
  if (!fresh) return;
  g_inv_px += fresh;
  g_inv_n++;
  if (x2 - x1 + 1 >= g_size_px && y2 - y1 + 1 >= g_size_px) g_inv_full++;
}
static void refr_start_cb(lv_event_t *e) {
  (void)e;
  if (g_cover) memset(g_cover, 0, (size_t)g_size_px * (size_t)g_size_px);
}
void sim_inv_reset(void) { g_inv_px = g_inv_n = g_inv_full = 0; }
long sim_inv_pixels(void) { return g_inv_px; }
long sim_inv_count(void) { return g_inv_n; }
long sim_inv_full(void) { return g_inv_full; }

static sim_t *g_sim;
static void *g_buf;
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  /* PARTIAL mode like the device: LVGL renders each invalidated area into the draw buffer, copied here into sim_t.fb */
  int w = lv_area_get_width(a), h = lv_area_get_height(a);
  uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)w, LV_COLOR_FORMAT_XRGB8888);
  for (int y = 0; y < h; y++)
    memcpy(&g_sim->fb[(size_t)(a->y1 + y) * (size_t)g_sim->size + (size_t)a->x1], px + (size_t)y * stride, (size_t)w * 4u);
  lv_display_flush_ready(d);
}

static void indev_cb(lv_indev_t *in, lv_indev_data_t *data) {
  (void)in;
  data->point.x = g_px;
  data->point.y = g_py;
  data->state = g_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static bool g_square;
bool sim_panel_square(void) { return g_square; }

bool sim_init(sim_t *s, int size) { return sim_init_ex(s, size, false); }

bool sim_init_ex(sim_t *s, int size, bool square) {
  if (size < 100 || size > 1000) return false;
  memset(s, 0, sizeof *s);
  s->size = size;
  s->square = square;
  g_square = square;
  s->fb = calloc((size_t)size * (size_t)size, 4);
  if (!s->fb) return false;
  g_clock = 0;
  g_real_clock = false;
  g_down = false;
  g_hook = NULL;
  lv_init();
  lv_tick_set_cb(tick_cb);
  g_disp = lv_display_create(size, size);
  lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_XRGB8888);
  g_sim = s;
  g_buf = malloc((size_t)size * (size_t)size * 4 + 64);
  lv_display_set_buffers(g_disp, g_buf, NULL, (uint32_t)size * (uint32_t)size * 4u, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(g_disp, flush_cb);
  g_size_px = size;
  free(g_cover);
  g_cover = calloc((size_t)size * (size_t)size, 1);
  sim_inv_reset();
  lv_display_add_event_cb(g_disp, inv_cb, LV_EVENT_INVALIDATE_AREA, NULL);
  lv_display_add_event_cb(g_disp, refr_start_cb, LV_EVENT_REFR_START, NULL);
  g_indev = lv_indev_create();
  lv_indev_set_type(g_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(g_indev, indev_cb);
  ru_theme_init(size);
  return true;
}

void sim_shutdown(sim_t *s) {
  lv_deinit();
  free(g_buf);
  g_buf = NULL;
  free(g_cover);
  g_cover = NULL;
  g_disp = NULL;
  g_indev = NULL;
  free(s->fb);
  s->fb = NULL;
}

uint32_t sim_now_ms(void) { return lv_tick_get(); }
void sim_use_real_clock(void) { g_real_clock = true; }

void sim_advance(uint32_t ms) {
  while (ms > 0) {
    uint32_t step = ms > 16 ? 16 : ms;
    g_clock += step;
    ms -= step;
    if (g_hook) g_hook(g_px, g_py, g_down, g_clock, g_hook_ud);
    lv_timer_handler();
  }
}

void sim_pointer(int x, int y, bool down) { g_px = x; g_py = y; g_down = down; }

void sim_render(void) {
  lv_obj_invalidate(lv_screen_active());
  sim_advance(100);
  lv_obj_invalidate(lv_screen_active());
  sim_advance(64);
}

bool sim_outside_circle(int size, int x, int y) {
  float c = (float)size / 2.0f;
  float dx = (float)x + 0.5f - c, dy = (float)y + 0.5f - c;
  return dx * dx + dy * dy > c * c;
}

long sim_count_outside_lit(const sim_t *s) {
  long n = 0;
  for (int y = 0; y < s->size; y++)
    for (int x = 0; x < s->size; x++)
      if ((s->fb[y * s->size + x] & 0xFFFFFF) != 0 && sim_outside_circle(s->size, x, y)) n++;
  return n;
}

long sim_count_outside_lit_except(const sim_t *s, const lv_area_t *r) {
  long n = 0;
  for (int y = 0; y < s->size; y++)
    for (int x = 0; x < s->size; x++) {
      if ((s->fb[y * s->size + x] & 0xFFFFFF) == 0 || !sim_outside_circle(s->size, x, y)) continue;
      if (r && x >= r->x1 && x <= r->x2 && y >= r->y1 && y <= r->y2) continue;
      n++;
    }
  return n;
}

long sim_count_lit_in(const sim_t *s, const lv_area_t *r) {
  long n = 0;
  for (int y = r->y1; y <= r->y2; y++)
    for (int x = r->x1; x <= r->x2; x++)
      if (x >= 0 && y >= 0 && x < s->size && y < s->size && (s->fb[y * s->size + x] & 0xFFFFFF) != 0) n++;
  return n;
}

long sim_count_lit(const sim_t *s) {
  long n = 0;
  for (int y = 0; y < s->size; y++)
    for (int x = 0; x < s->size; x++)
      if ((s->fb[y * s->size + x] & 0xFFFFFF) != 0 && !sim_outside_circle(s->size, x, y)) n++;
  return n;
}

void sim_masked_copy(const sim_t *s, uint32_t *dst) {
  if (s->square) { for (int i = 0; i < s->size * s->size; i++) dst[i] = s->fb[i] & 0xFFFFFF; return; }
  for (int y = 0; y < s->size; y++)
    for (int x = 0; x < s->size; x++)
      dst[y * s->size + x] = sim_outside_circle(s->size, x, y) ? 0 : (s->fb[y * s->size + x] & 0xFFFFFF);
}

/* LVGL 9.2.2 upstream bug: lv_draw_sw_arc() never frees the radius mask it inits for rounded arcs
 * (lv_draw_sw_arc.c, circle_mask_param). Suppress only that allocation so LeakSanitizer still
 * catches leaks in dzPanel code. Remove when LVGL is bumped. */
const char *__lsan_default_suppressions(void) { return "leak:lv_draw_sw_mask_radius_init\n"; }
