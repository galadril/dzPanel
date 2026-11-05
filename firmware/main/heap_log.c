#include "heap_log.h"

#if CONFIG_RU_HEAP_LOG

#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Diagnostics only. Logs numbers and fixed task names: no configuration value, SSID or credential is ever formatted here. */

static const char *TAG = "heap";

/* Tasks looked up by name on every tick (a cached handle would dangle after a task exits). Missing ones are skipped. */
static const char *const k_tasks[] = {"ru_wifi", "ru_console", "ui_bridge", "dz_service", "dz-net", "boot_health",
                                      "lvgl",    "swdraw",     "ru_portal", "ru_dns"};
#define NTASKS (sizeof k_tasks / sizeof k_tasks[0])

static char g_line[320]; /* static: nothing is allocated per iteration, and the task stack stays small */

static void failed_alloc_cb(size_t size, uint32_t caps, const char *fn) {
  ESP_DRAM_LOGE("heap", "alloc failed: size %u caps 0x%x in %s", (unsigned)size, (unsigned)caps, fn ? fn : "?");
}

static void heap_task(void *arg) {
  (void)arg;
  const TickType_t period = pdMS_TO_TICKS(CONFIG_RU_HEAP_LOG_PERIOD_S * 1000);
  for (;;) {
    const uint32_t ic = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t n = (size_t)snprintf(g_line, sizeof g_line, "int free %u big %u min %u | psram free %u big %u min %u | stack-free-min",
                                (unsigned)heap_caps_get_free_size(ic), (unsigned)heap_caps_get_largest_free_block(ic),
                                (unsigned)heap_caps_get_minimum_free_size(ic),
                                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    for (size_t i = 0; i < NTASKS && n < sizeof g_line; i++) {
      TaskHandle_t h = xTaskGetHandle(k_tasks[i]);
      if (!h) continue;
      int w = snprintf(g_line + n, sizeof g_line - n, " %s=%u", k_tasks[i], (unsigned)uxTaskGetStackHighWaterMark(h));
      if (w < 0) break;
      n += (size_t)w;
    }
    if (n < sizeof g_line) {
      int w = snprintf(g_line + n, sizeof g_line - n, " heaplog=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
      (void)w;
    }
    ESP_LOGI(TAG, "%s", g_line);
    vTaskDelay(period);
  }
}

void ru_heap_log_start(void) {
  heap_caps_register_failed_alloc_callback(failed_alloc_cb);
  if (xTaskCreate(heap_task, "heaplog", 3072, NULL, 1, NULL) != pdPASS) ESP_LOGE(TAG, "heap log task failed");
}

#else

void ru_heap_log_start(void) {}

#endif
