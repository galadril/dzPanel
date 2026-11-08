#include "ru_hal.h"
#include "ru_hal_port.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"

/* Waveshare ESP32-S3-Touch-AMOLED-1.75: 466 px round, CO5300 + CST9217, 16 MB flash, 8 MB PSRAM.
 * Other boards get their own directory under components/boards; ru_hal.c stays unchanged. */
static const ru_board_t k_board = {
    .id = "amoled_175",
    .diameter = 466,
    .safe_inset_px = 14,   /* unverified: calibrate on the real glass (BUILD_PLAN C3) */
    .round = true,
    .has_touch = true,
    .has_brightness = true,
    .draw_buf_psram = true,
    .draw_buf_lines = 50,
    .flash_mb = 16,
    .rotations = 0xF, /* all four, by the CO5300 scan direction (MADCTL): no extra frame buffer, no tearing. UNVERIFIED on the glass. */
};
static const char *TAG = "ru_board_amoled";
static int g_rot_applied;

const ru_board_t *ru_board(void) { return &k_board; }

/* Port functions: the Waveshare BSP is the only place that knows pins and drivers for this board. */
/* Our rotation (ru_geom.h: 90 turns the logical top edge to the panel's left edge) in the BSP's enum. The BSP writes MADCTL: 0x60 for
 * ROTATE_90 (MV|MX: transpose, then mirror the column), 0xC0 for 180 (MX|MY), 0xA0 for 270 (MV|MY), which is the same mapping as
 * ru_rot_to_panel (checked by working through the bits, not on the panel). LVGL is not told: it keeps drawing upright at 0 and ru_hal
 * maps the touch points. */
static bool bsp_rotation(int deg, bsp_display_rotation_t *out) {
  switch (deg) {
    case 0: *out = BSP_DISPLAY_ROTATE_0; return true;
    case 90: *out = BSP_DISPLAY_ROTATE_90; return true;
    case 180: *out = BSP_DISPLAY_ROTATE_180; return true;
    case 270: *out = BSP_DISPLAY_ROTATE_270; return true;
    default: return false;
  }
}

lv_display_t *ru_port_start(lv_indev_t **indev, int rot) {
  /* the BSP's own bsp_display_start() configuration (see esp32_s3_touch_amoled_1_75.c), passed explicitly */
  bsp_display_cfg_t cfg = {
      .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
      .rotation = ESP_LV_ADAPTER_ROTATE_0, /* the adapter does not rotate: the panel does */
      .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
      .touch_flags = {.swap_xy = 0, .mirror_x = 1, .mirror_y = 1},
  };
  lv_display_t *disp = bsp_display_start_with_config(&cfg);
  g_rot_applied = 0;
  bsp_display_rotation_t br;
  if (disp && rot != 0 && bsp_rotation(rot, &br)) {
    esp_err_t e = bsp_display_rotation_set(br);
    if (e == ESP_OK) g_rot_applied = rot;
    else ESP_LOGE(TAG, "bsp_display_rotation_set(%d): %s, staying upright", rot, esp_err_to_name(e));
  }
  /* bsp_display_start() fails hard (returns NULL) if touch init fails; if it returned a display we have touch. */
  *indev = disp ? bsp_display_get_input_dev() : NULL;
  return disp;
}

int ru_port_rotation(void) { return g_rot_applied; }

bool ru_port_lock(uint32_t ms) { return bsp_display_lock(ms) == ESP_OK; }
void ru_port_unlock(void) { bsp_display_unlock(); }
void ru_port_brightness_set(int percent) { bsp_display_brightness_set(percent); }
int ru_port_brightness_get(void) { return bsp_display_brightness_get(); }
