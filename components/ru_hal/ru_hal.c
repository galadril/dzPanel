#include "sdkconfig.h"
#include "ru_hal.h"
#include "ru_hal_port.h"
#include <stddef.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ru_geom.h"

static const char *TAG = "ru_hal";

static bool g_inited;
static bool g_touch;
static lv_display_t *g_disp;
static lv_indev_t *g_indev;
static lv_indev_read_cb_t g_orig_read;
static ru_hal_touch_cb_t g_sink;
static void *g_sink_ud;
static int g_rot;          /* rotation the port was started with */
static int g_pending_rot;  /* chosen before init */
static bool g_hw_rot;      /* the port turns the picture in hardware: LVGL needs logical touch points from us */

/* Runs in the LVGL task. Wraps the port's read callback so LVGL still gets its input while ru_gesture sees every sample. */
static void touch_read_wrap(lv_indev_t *indev, lv_indev_data_t *data) {
  g_orig_read(indev, data);
  /* data->point is what the touch panel reports: panel coordinates. LVGL turns them for a software rotation by itself; for a
   * hardware one we do it here. ru_gesture always gets logical coordinates, the ones the UI is laid out in. */
  int lx = data->point.x, ly = data->point.y;
  if (g_rot) {
    ru_rot_to_logical(g_rot, ru_board()->diameter, data->point.x, data->point.y, &lx, &ly);
    if (g_hw_rot) { data->point.x = lx; data->point.y = ly; }
  }
  if (!g_sink) return;
  ru_touch_t s = {
      .x = (int16_t)lx,
      .y = (int16_t)ly,
      .t_ms = (uint32_t)(esp_timer_get_time() / 1000),
      .down = data->state == LV_INDEV_STATE_PRESSED,
      .contacts = 0, /* the port read path does not expose contact count or size */
      .size = 0,
  };
  g_sink(&s, g_sink_ud);
}

bool ru_hal_set_rotation(int deg) {
  if (g_inited || (deg != 0 && deg != 90 && deg != 180 && deg != 270)) return false;
  if (!((ru_board()->rotations | 1u) >> (deg / 90) & 1u)) return false;
  g_pending_rot = deg;
  return true;
}
int ru_hal_rotation(void) { return g_rot; }

ru_hal_err_t ru_hal_init(void) {
  if (g_inited) return RU_HAL_OK;
  g_disp = ru_port_start(&g_indev, g_pending_rot);
  if (!g_disp) {
    ESP_LOGE(TAG, "display start failed");
    return RU_HAL_ERR_DISPLAY;
  }
  g_rot = ru_port_rotation();
  if (g_rot != g_pending_rot) ESP_LOGW(TAG, "rotation %d requested, the port applied %d", g_pending_rot, g_rot);
  g_hw_rot = g_rot != 0 && lv_display_get_rotation(g_disp) == LV_DISPLAY_ROTATION_0;
  ESP_LOGI(TAG, "rotation %d (%s)", g_rot, g_rot == 0 ? "none" : g_hw_rot ? "panel scan direction" : "LVGL / adapter");
  if (g_indev) {
    g_orig_read = lv_indev_get_read_cb(g_indev);
    if (g_orig_read) {
      lv_indev_set_read_cb(g_indev, touch_read_wrap);
      g_touch = true;
    }
  }
  if (!g_touch) ESP_LOGW(TAG, "no touch input device");
  g_inited = true;
  return RU_HAL_OK;
}

bool ru_hal_has_touch(void) { return g_touch; }

bool ru_hal_lock(int timeout_ms) {
  return ru_port_lock(timeout_ms < 0 ? UINT32_MAX : (uint32_t)timeout_ms);
}
void ru_hal_unlock(void) { ru_port_unlock(); }

void ru_hal_set_brightness(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  ru_port_brightness_set(percent);
}
int ru_hal_get_brightness(void) { return ru_port_brightness_get(); }

void ru_hal_set_touch_sink(ru_hal_touch_cb_t cb, void *ud) {
  g_sink_ud = ud;
  g_sink = cb;
}

void ru_hal_heap_log(const char *tag) {
#if CONFIG_RU_HEAP_LOG
  ESP_LOGI(TAG, "%s heap: internal free %u (largest %u), psram free %u (largest %u), min int %u psram %u", tag ? tag : "",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
#else
  ESP_LOGI(TAG, "%s heap: internal free %u (largest %u), psram free %u (largest %u)", tag ? tag : "",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#endif
}
