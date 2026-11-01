#include "boot_health.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "ru_osal.h"

static const char *TAG = "boot";
#define NS "boot"
#define KEY "fails"

static ru_kv_t *g_kv;
static unsigned g_prev_fails;
static atomic_uint g_beat;
static atomic_bool g_display_ok;
static bool g_safe;

static unsigned read_count(void) {
  char b[12];
  if (ru_kv_get_str(g_kv, NS, KEY, b, sizeof b) != RU_KV_OK) return 0;
  unsigned long v = strtoul(b, NULL, 10);
  return v > 1000 ? 1000 : (unsigned)v;
}
static void write_count(unsigned n) {
  char b[12];
  snprintf(b, sizeof b, "%u", n);
  if (ru_kv_set_str(g_kv, NS, KEY, b) != RU_KV_OK) ESP_LOGW(TAG, "could not store boot counter");
}

bool bh_boot_begin(ru_kv_t *kv) {
  g_kv = kv;
  g_prev_fails = read_count();
  write_count(g_prev_fails + 1); /* cleared again once this boot proves healthy */
  g_safe = g_prev_fails >= BH_SAFE_AFTER_FAILS;
  ESP_LOGI(TAG, "previous failed boots: %u%s", g_prev_fails, g_safe ? " -> SAFE MODE" : "");
  return g_safe;
}

unsigned bh_fail_count(void) { return g_prev_fails; }
void bh_set_display_ok(bool ok) { atomic_store(&g_display_ok, ok); }
void bh_heartbeat(void) { atomic_fetch_add(&g_beat, 1); }
void bh_clear(void) {
  if (g_kv) write_count(0);
}

/* Every 5 s the heartbeat must have moved. If the UI loop stalls we restart (the counter keeps growing, so a
 * persistent stall ends in safe mode; a pending OTA image is rolled back by the bootloader on that reset). */
static void watch_task(void *arg) {
  (void)arg;
  uint32_t t0 = ru_now_ms();
  unsigned last = atomic_load(&g_beat);
  uint32_t next = t0 + 5000;
  for (;;) {
    ru_sleep_ms(500);
    uint32_t now = ru_now_ms();
    if ((int32_t)(now - next) < 0) continue;
    next += 5000;
    unsigned b = atomic_load(&g_beat);
    if (b == last) {
      ESP_LOGE(TAG, "UI loop stalled; restarting");
      esp_restart();
    }
    last = b;
    if (now - t0 >= BH_STABLE_MS) break;
  }
  if (!atomic_load(&g_display_ok)) {
    ESP_LOGW(TAG, "display not up: not marking the image valid");
    return;
  }
  if (g_safe) {
    ESP_LOGW(TAG, "safe mode: image stays unconfirmed, counter kept");
    return;
  }
  esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
  write_count(0);
  ESP_LOGI(TAG, "healthy for %u s: image marked valid (%s), boot counter cleared", (unsigned)(BH_STABLE_MS / 1000),
           esp_err_to_name(e));
}

void bh_start(bool safe_mode) {
  g_safe = safe_mode;
  /* The watcher returns after the stable window; ru_task has no detached mode, so the task object is leaked on purpose
   * (one small struct per boot). */
  if (!ru_task_create("boot_health", watch_task, NULL)) ESP_LOGE(TAG, "watcher task failed");
}
