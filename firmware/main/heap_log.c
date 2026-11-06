#include "sdkconfig.h"
#include "heap_log.h"

#if CONFIG_RU_HEAP_LOG

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_shell.h"

/* Diagnostics only. Logs numbers and fixed task names: no configuration value, SSID or credential is ever formatted here. */

static const char *TAG = "heap";

/* Permanent tasks only: each is created once at boot and never deleted (ru_wifi: W.stop is never set; ru_console and
 * ui_bridge loop forever; dz_service is only joined by dz_service_destroy, which firmware never calls). Transient tasks
 * (ru_portal, ru_dns, dz-net, boot_health) are excluded: an unprotected handle to a task that exits between
 * xTaskGetHandle and uxTaskGetStackHighWaterMark would point at a freed TCB. Handles are cached once found; only the
 * heaplog task touches the cache, so no locking is needed. Tasks not yet created (NULL) are skipped. */
static const char *const k_tasks[] = {"ru_wifi", "ru_console", "ui_bridge", "dz_service"};
#define NTASKS (sizeof k_tasks / sizeof k_tasks[0])
static TaskHandle_t s_handles[NTASKS];

static char g_line[320]; /* static: nothing is allocated per iteration, and the task stack stays small */

/* Failed-alloc hook: may run in any context, so no locks, no allocation, no esp_timer. Rate limited to one line per
 * 200 ms by tick count (xTaskGetTickCount is task-context only, so ISRs use the FromISR variant); suppressed failures
 * are counted and reported by the periodic line (alloc-fail-dropped). The racy static counters are acceptable for
 * diagnostics. `fn` is deliberately NOT printed: it is a __func__ string in flash rodata, and with SPIRAM_RODATA /
 * XIP_FROM_PSRAM it is unreadable if the failure fires while the flash cache is disabled. Size and caps suffice. */
static uint32_t s_last_fail_tick;
static bool s_have_fail;
static volatile uint32_t s_dropped;

static void failed_alloc_cb(size_t size, uint32_t caps, const char *fn) {
  (void)fn;
  uint32_t now = (uint32_t)(xPortInIsrContext() ? xTaskGetTickCountFromISR() : xTaskGetTickCount());
  if (s_have_fail && (uint32_t)(now - s_last_fail_tick) < pdMS_TO_TICKS(200)) {
    s_dropped++;
    return;
  }
  s_have_fail = true;
  s_last_fail_tick = now;
  ESP_DRAM_LOGE("heap", "alloc failed: size %u caps 0x%x", (unsigned)size, (unsigned)caps);
}

#define LOW_FREE_BYTES 24576
#define LOW_RATE_MS 60000
#define SAMPLE_MS 100
#define IC (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static const char *volatile s_last_tag = "none"; /* static strings only: the heap task reads it from another task */

/* One "low" line when internal free is below LOW_FREE_BYTES, at most once per LOW_RATE_MS. Numbers and a fixed tag only. */
static void check_low(void) {
  static TickType_t last;
  static bool have;
  size_t f = heap_caps_get_free_size(IC);
  if (f >= LOW_FREE_BYTES) return;
  TickType_t now = xTaskGetTickCount();
  if (have && (now - last) < pdMS_TO_TICKS(LOW_RATE_MS)) return;
  have = true;
  last = now;
  ESP_LOGW(TAG, "low int free %u big %u min %u after mark %s", (unsigned)f, (unsigned)heap_caps_get_largest_free_block(IC),
           (unsigned)heap_caps_get_minimum_free_size(IC), s_last_tag);
}

void ru_heap_log_mark(const char *tag) {
  s_last_tag = tag;
  ESP_LOGI(TAG, "mark %s int free %u big %u min %u", tag, (unsigned)heap_caps_get_free_size(IC),
           (unsigned)heap_caps_get_largest_free_block(IC), (unsigned)heap_caps_get_minimum_free_size(IC));
}

void ru_heap_log_mark_res(const char *tag, int status, int err, unsigned bytes, unsigned elapsed_ms) {
  s_last_tag = tag;
  ESP_LOGI(TAG, "mark %s int free %u big %u min %u status %d err %d bytes %u ms %u", tag, (unsigned)heap_caps_get_free_size(IC),
           (unsigned)heap_caps_get_largest_free_block(IC), (unsigned)heap_caps_get_minimum_free_size(IC), status, err, bytes,
           elapsed_ms);
}

static void heap_task(void *arg) {
  (void)arg;
  const TickType_t period = pdMS_TO_TICKS(CONFIG_RU_HEAP_LOG_PERIOD_S * 1000);
  TickType_t next = xTaskGetTickCount();
  for (;;) {
    const uint32_t ic = IC;
    size_t n = (size_t)snprintf(g_line, sizeof g_line, "int free %u big %u min %u | psram free %u big %u min %u | stack-free-min",
                                (unsigned)heap_caps_get_free_size(ic), (unsigned)heap_caps_get_largest_free_block(ic),
                                (unsigned)heap_caps_get_minimum_free_size(ic),
                                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    for (size_t i = 0; i < NTASKS && n < sizeof g_line; i++) {
      if (!s_handles[i]) s_handles[i] = xTaskGetHandle(k_tasks[i]);
      TaskHandle_t h = s_handles[i];
      if (!h) continue;
      int w = snprintf(g_line + n, sizeof g_line - n, " %s=%u", k_tasks[i], (unsigned)uxTaskGetStackHighWaterMark(h));
      if (w < 0) break;
      n += (size_t)w;
    }
    if (n < sizeof g_line) {
      int w = snprintf(g_line + n, sizeof g_line - n, " heaplog=%u alloc-fail-dropped=%u",
                           (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)s_dropped);
      (void)w;
    }
    ESP_LOGI(TAG, "%s", g_line);
    ui_shell_log_disp(); /* the `disp` line next to the heap line */
    next += period;
    while ((int32_t)(next - xTaskGetTickCount()) > 0) { /* sample often so a short burst below 24 KB is still caught */
      check_low();
      vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
  }
}

void ru_heap_log_start(void) {
  heap_caps_register_failed_alloc_callback(failed_alloc_cb);
  if (xTaskCreate(heap_task, "heaplog", 4096, NULL, 1, NULL) != pdPASS) ESP_LOGE(TAG, "heap log task failed");
}

#else

void ru_heap_log_start(void) {}

#endif
