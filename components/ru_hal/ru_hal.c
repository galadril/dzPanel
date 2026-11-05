#include "ru_hal.h"
#include "ru_hal_port.h"
#include <stddef.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "ru_hal";

static bool g_inited;
static bool g_touch;
static lv_display_t *g_disp;
static lv_indev_t *g_indev;
static lv_indev_read_cb_t g_orig_read;
static ru_hal_touch_cb_t g_sink;
static void *g_sink_ud;

/* Runs in the LVGL task. Wraps the port's read callback so LVGL still gets its input while ru_gesture sees every sample. */
static void touch_read_wrap(lv_indev_t *indev, lv_indev_data_t *data) {
  g_orig_read(indev, data);
  if (!g_sink) return;
  ru_touch_t s = {
      .x = (int16_t)data->point.x,
      .y = (int16_t)data->point.y,
      .t_ms = (uint32_t)(esp_timer_get_time() / 1000),
      .down = data->state == LV_INDEV_STATE_PRESSED,
      .contacts = 0, /* the port read path does not expose contact count or size */
      .size = 0,
  };
  g_sink(&s, g_sink_ud);
}

ru_hal_err_t ru_hal_init(void) {
  if (g_inited) return RU_HAL_OK;
  g_disp = ru_port_start(&g_indev);
  if (!g_disp) {
    ESP_LOGE(TAG, "display start failed");
    return RU_HAL_ERR_DISPLAY;
  }
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
  ESP_LOGI(TAG, "%s heap: internal free %u (largest %u), psram free %u (largest %u)", tag ? tag : "",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
