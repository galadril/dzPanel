/* Guition 4848S040 bring-up probe. Everything goes to UART0 at 115200 (the CH341 bridge). */
#include <stdio.h>
#include <stdlib.h>
#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ru_board_guition_4848s040.h"
#include "ru_hal.h"
#include "ru_hal_port.h"

static const char *TAG = "probe";

#define STR2(x) #x
#define STR(x) STR2(x)
#define ORDER_STR (RU_GUITION_BGR ? "BGR (MADCTL 0x08)" : "RGB (MADCTL 0x00)")

static const char *reset_reason_str(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
  }
}

static void log_system(void) {
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  uint32_t flash = 0;
  esp_flash_get_size(NULL, &flash);
  ESP_LOGI(TAG, "chip: ESP32-S3 rev v%d.%d, %d cores, features 0x%lx", ci.revision / 100, ci.revision % 100, ci.cores,
           (unsigned long)ci.features);
  ESP_LOGI(TAG, "flash: %lu MB (embedded flash feature: %s)", (unsigned long)(flash >> 20),
           (ci.features & CHIP_FEATURE_EMB_FLASH) ? "yes" : "no");
#ifdef CONFIG_SPIRAM_MODE_OCT
  const char *mode = "octal";
#else
  const char *mode = "quad";
#endif
  ESP_LOGI(TAG, "psram: %s, initialised=%d, size=%u bytes, mode=%s @ %s MHz", esp_psram_is_initialized() ? "present" : "ABSENT",
           (int)esp_psram_is_initialized(), (unsigned)esp_psram_get_size(), mode, STR(CONFIG_SPIRAM_SPEED));
  ESP_LOGI(TAG, "heap: internal free %u (largest %u), psram free %u (largest %u)",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  esp_reset_reason_t r = esp_reset_reason();
  ESP_LOGI(TAG, "reset reason: %d (%s)", (int)r, reset_reason_str(r));
  ESP_LOGI(TAG, "config: colour order %s, COLMOD 0x%02X, pclk %d Hz", ORDER_STR, RU_GUITION_COLMOD, RU_GUITION_PCLK_HZ);
  ESP_LOGI(TAG, "timing: H pw/fp/bp %d/%d/%d, V pw/fp/bp %d/%d/%d, pclk_active_neg %d, bounce %d px", RU_GUITION_HSYNC_PW,
           RU_GUITION_HSYNC_FP, RU_GUITION_HSYNC_BP, RU_GUITION_VSYNC_PW, RU_GUITION_VSYNC_FP, RU_GUITION_VSYNC_BP,
           RU_GUITION_PCLK_ACTIVE_NEG, RU_GUITION_BOUNCE_PX);
}

static void scan_touch_bus(void) {
  i2c_master_bus_handle_t bus = ru_guition_touch_bus();
  if (!bus) {
    ESP_LOGW(TAG, "i2c: bus not created (display start failed before touch?)");
    return;
  }
  int found = 0;
  for (int a = 0x08; a < 0x78; a++) {
    if (i2c_master_probe(bus, (uint16_t)a, 20) == ESP_OK) {
      ESP_LOGI(TAG, "i2c: device found at 0x%02X%s", a, (a == 0x5D || a == 0x14) ? " (GT911 candidate)" : "");
      found++;
    }
  }
  if (!found) ESP_LOGW(TAG, "i2c: NO devices on SDA=%d SCL=%d", RU_GUITION_PIN_TOUCH_SDA, RU_GUITION_PIN_TOUCH_SCL);
  ESP_LOGI(TAG, "touch: GT911 %s (addr 0x%02X)", ru_guition_touch() ? "initialised" : "NOT initialised", ru_guition_touch_addr());
}

static lv_obj_t *s_root;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t c) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}

static void clear_root(void) {
  lv_obj_clean(s_root);
}

static void fill_screen(const char *name, lv_color_t c) {
  ESP_LOGI(TAG, "colour test: full-screen %s (expect %s; if red and blue are swapped flip RU_GUITION_BGR)", name, name);
  if (!ru_port_lock(UINT32_MAX)) return;
  clear_root();
  box(s_root, 0, 0, RU_GUITION_HRES, RU_GUITION_VRES, c);
  ru_port_unlock();
  vTaskDelay(pdMS_TO_TICKS(2000));
}

static void bars_screen(void) {
  ESP_LOGI(TAG, "colour test: 8 bars (red green blue yellow cyan magenta grey black), 1-px white border on all edges");
  static const uint32_t cols[8] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0x808080, 0x000000};
  if (!ru_port_lock(UINT32_MAX)) return;
  clear_root();
  const int bw = RU_GUITION_HRES / 8;
  for (int i = 0; i < 8; i++) box(s_root, i * bw, 0, bw, RU_GUITION_VRES, lv_color_hex(cols[i]));
  const lv_color_t w = lv_color_white();
  box(s_root, 0, 0, RU_GUITION_HRES, 1, w);
  box(s_root, 0, RU_GUITION_VRES - 1, RU_GUITION_HRES, 1, w);
  box(s_root, 0, 0, 1, RU_GUITION_VRES, w);
  box(s_root, RU_GUITION_HRES - 1, 0, 1, RU_GUITION_VRES, w);
  lv_obj_t *strip = box(s_root, 0, 200, RU_GUITION_HRES, 80, lv_color_black());
  lv_obj_set_style_bg_opa(strip, LV_OPA_70, 0);
  lv_obj_t *lbl = lv_label_create(s_root);
  lv_label_set_text_fmt(lbl, "order %s  COLMOD 0x%02X\npclk %d MHz  H %d/%d/%d  V %d/%d/%d", ORDER_STR, RU_GUITION_COLMOD,
                        RU_GUITION_PCLK_HZ / 1000000, RU_GUITION_HSYNC_PW, RU_GUITION_HSYNC_FP, RU_GUITION_HSYNC_BP,
                        RU_GUITION_VSYNC_PW, RU_GUITION_VSYNC_FP, RU_GUITION_VSYNC_BP);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(lbl, RU_GUITION_HRES);
  lv_obj_set_pos(lbl, 0, 212);
  ru_port_unlock();
}

static const char *region(int x, int y) {
  const int m = 80, c0 = 160, c1 = 320;
  const bool west = x < m, east = x >= RU_GUITION_HRES - m, north = y < m, south = y >= RU_GUITION_VRES - m;
  if (north && east) return "corner NE";
  if (north && west) return "corner NW";
  if (south && east) return "corner SE";
  if (south && west) return "corner SW";
  if (x >= c0 && x < c1 && y >= c0 && y < c1) return "centre";
  return "edge/other";
}

static void touch_loop(lv_indev_t *indev) {
  ESP_LOGI(TAG, "touch test: touch the four corners and the centre; expect NW at top-left on screen, x grows right, y grows down");
  if (!indev) ESP_LOGW(TAG, "no touch input device: the loop only keeps the screen alive");
  if (ru_port_lock(UINT32_MAX)) {
    clear_root();
    ru_port_unlock();
  }
  lv_obj_t *hl = NULL, *vl = NULL;
  bool was_down = false;
  int32_t last_x = -1, last_y = -1;
  int tick = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(20));
    if (++tick % 500 == 0) ESP_LOGI(TAG, "alive; heap internal %u psram %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (!indev || !ru_port_lock(100)) continue;
    lv_point_t p = {0, 0};
    lv_indev_get_point(indev, &p);
    bool down = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    if (down) {
      if (!hl) {
        hl = box(s_root, 0, 0, RU_GUITION_HRES, 1, lv_color_white());
        vl = box(s_root, 0, 0, 1, RU_GUITION_VRES, lv_color_white());
      }
      lv_obj_set_pos(hl, 0, p.y);
      lv_obj_set_pos(vl, p.x, 0);
      if (!was_down || p.x != last_x || p.y != last_y) {
        ESP_LOGI(TAG, "touch x=%d y=%d -> %s", (int)p.x, (int)p.y, region(p.x, p.y));
        last_x = p.x;
        last_y = p.y;
      }
    } else if (was_down) {
      ESP_LOGI(TAG, "touch released");
    }
    was_down = down;
    ru_port_unlock();
  }
}

void app_main(void) {
  vTaskDelay(pdMS_TO_TICKS(300));
  ESP_LOGI(TAG, "==== Guition 4848S040 bring-up probe ====");
  log_system();

  lv_indev_t *indev = NULL;
  ESP_LOGI(TAG, "starting display (ST7701 init, RGB panel, GT911)...");
  lv_display_t *disp = ru_port_start(&indev);
  if (!disp) {
    ESP_LOGE(TAG, "ru_port_start FAILED: display did not start; see errors above. Idling.");
    for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
  }
  ESP_LOGI(TAG, "display started; backlight %d%%; touch indev %s", ru_port_brightness_get(), indev ? "yes" : "no");
  ESP_LOGI(TAG, "heap after display: internal %u psram %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  scan_touch_bus();

  if (ru_port_lock(UINT32_MAX)) {
    s_root = lv_screen_active();
    lv_obj_remove_style_all(s_root);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0); /* remove_style_all leaves the background transparent */
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    ru_port_unlock();
  }
  fill_screen("RED", lv_color_hex(0xFF0000));
  fill_screen("GREEN", lv_color_hex(0x00FF00));
  fill_screen("BLUE", lv_color_hex(0x0000FF));
  fill_screen("WHITE", lv_color_hex(0xFFFFFF));
  bars_screen();
  vTaskDelay(pdMS_TO_TICKS(5000));
  touch_loop(indev);
}
