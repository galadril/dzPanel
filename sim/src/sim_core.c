#include "sim_core.h"
#include "ru_theme.h"
#include <stdlib.h>
#include <string.h>

static uint32_t g_clock;
static bool g_real_clock;
static int g_px, g_py;
static bool g_down;
static lv_display_t *g_disp;
static lv_indev_t *g_indev;

#ifdef RU_SIM_SDL
extern uint32_t sim_sdl_ticks(void);
#endif

static uint32_t tick_cb(void) {
#ifdef RU_SIM_SDL
  if (g_real_clock) return sim_sdl_ticks();
#endif
  return g_clock;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  (void)a; (void)px; /* LVGL renders straight into sim_t.fb (full mode) */
  lv_display_flush_ready(d);
}

static void indev_cb(lv_indev_t *in, lv_indev_data_t *data) {
  (void)in;
  data->point.x = g_px;
  data->point.y = g_py;
  data->state = g_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool sim_init(sim_t *s, int size) {
  if (size < 100 || size > 1000) return false;
  memset(s, 0, sizeof *s);
  s->size = size;
  s->fb = calloc((size_t)size * (size_t)size, 4);
  if (!s->fb) return false;
  g_clock = 0;
  g_real_clock = false;
  g_down = false;
  lv_init();
  lv_tick_set_cb(tick_cb);
  g_disp = lv_display_create(size, size);
  lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_XRGB8888);
  lv_display_set_buffers(g_disp, s->fb, NULL, (uint32_t)size * (uint32_t)size * 4u, LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(g_disp, flush_cb);
  g_indev = lv_indev_create();
  lv_indev_set_type(g_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(g_indev, indev_cb);
  ru_theme_init(size);
  return true;
}

void sim_shutdown(sim_t *s) {
  lv_deinit();
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

long sim_count_lit(const sim_t *s) {
  long n = 0;
  for (int y = 0; y < s->size; y++)
    for (int x = 0; x < s->size; x++)
      if ((s->fb[y * s->size + x] & 0xFFFFFF) != 0 && !sim_outside_circle(s->size, x, y)) n++;
  return n;
}

void sim_masked_copy(const sim_t *s, uint32_t *dst) {
  for (int y = 0; y < s->size; y++)
    for (int x = 0; x < s->size; x++)
      dst[y * s->size + x] = sim_outside_circle(s->size, x, y) ? 0 : (s->fb[y * s->size + x] & 0xFFFFFF);
}

/* LVGL 9.2.2 upstream bug: lv_draw_sw_arc() never frees the radius mask it inits for rounded arcs
 * (lv_draw_sw_arc.c, circle_mask_param). Suppress only that allocation so LeakSanitizer still
 * catches leaks in RoundOS code. Remove when LVGL is bumped. */
const char *__lsan_default_suppressions(void) { return "leak:lv_draw_sw_mask_radius_init\n"; }
