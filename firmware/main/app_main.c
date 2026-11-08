/* dzPanel firmware entry: NVS -> boot counter -> HAL/LVGL -> UI shell -> Wi-Fi + console + Domoticz service.
 * Compiled only, never run on a board. */
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "boot_health.h"
#include "ru_bus.h"
#include "ru_cfg.h"
#include "ru_hal.h"
#include "ru_prefs.h"
#include "svc_glue.h"
#include "heap_log.h"
#include "ui_shell.h"

static const char *TAG = "dzpanel";
static ru_bus_t g_bus;

void app_main(void) {
  ESP_LOGI(TAG, "dzPanel, IDF %s", esp_get_idf_version());
  const ru_board_t *brd = ru_board();
  ESP_LOGI(TAG, "board %s, display %dx%d %s", brd->id, brd->diameter, brd->diameter, brd->round ? "round" : "square");
  if (ru_kv_nvs_init() != 0) {
    ESP_LOGE(TAG, "NVS init failed");
    return;
  }
  ru_kv_t *kv = ru_kv_nvs_get();
  bool safe = bh_boot_begin(kv);
  ru_heap_log_start();
  ru_hal_heap_log("boot");

  /* the interface preferences (rotation, start page) are read BEFORE the display comes up: a rotation is applied by the panel
   * driver at start, so a change in Settings takes effect at the next restart */
  ru_prefs_t prefs;
  ru_prefs_load(kv, &prefs);
  if (prefs.rotation != 0 && !ru_hal_set_rotation(prefs.rotation))
    ESP_LOGW(TAG, "rotation %d is not supported by this board: upright", prefs.rotation);
  ESP_LOGI(TAG, "prefs: rotation %d, start page %s", prefs.rotation, prefs.start == RU_START_FAVS ? "favourites" : "clock");

  ru_hal_err_t he = ru_hal_init();
  bh_set_display_ok(he == RU_HAL_OK);
  if (he != RU_HAL_OK) ESP_LOGE(TAG, "HAL init failed: running headless (console and network only)");
  ru_hal_heap_log("after hal");

  ru_bus_init(&g_bus);
  bool svc_ok = svc_init(kv, &g_bus, !safe);
  if (!svc_ok && !safe) {
    ESP_LOGE(TAG, "service unavailable: safe screen");
    safe = true;
  }

  if (he == RU_HAL_OK && ru_hal_lock(-1)) {
    ui_shell_start(safe ? NULL : svc_service(), safe, bh_fail_count(), kv);
    ru_hal_unlock();
    ru_hal_set_brightness(80);
  }
  ru_hal_heap_log("ui up");

  svc_start();
  bh_start(safe);
  ru_hal_heap_log("network up");
}
