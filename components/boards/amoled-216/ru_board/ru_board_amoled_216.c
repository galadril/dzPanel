#include "ru_hal.h"
#include "ru_hal_port.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "bsp/touch.h"

/* Draw buffer height in lines. The buffers live in PSRAM, which the SPI DMA cannot read: the SPI driver copies every flush into an
 * internal-RAM DMA bounce buffer as large as the flush (480 x lines x 2 bytes). The BSP's own 50 lines need one contiguous 48 KB block
 * of internal RAM per flush; on the amoled-175 (same BSP design) that malloc failed with Wi-Fi and TLS up (ESP_ERR_NO_MEM) and strips
 * of the picture were never sent. 10 lines is a 9.6 KB block. */
#define RU_DRAW_BUF_LINES 10

/* Waveshare ESP32-S3-Touch-AMOLED-2.16: 480 x 480 rounded-square AMOLED (CO5300, QSPI) + CST9220 read by the CST9217 driver, no IO
 * expander in the BSP, 16 MB flash (the factory image header says 16 MB), 8 MB octal PSRAM (Waveshare's demo sdkconfig).
 * The glass is a square with rounded corners, so round = false (square layouts). The corner radius is not measured. */
static const ru_board_t k_board = {
    .id = "amoled_216",
    .diameter = 480,
    .safe_inset_px = 0, /* unverified: not used by the UI today; the corner radius of the glass is not measured */
    .round = false,
    .has_touch = true,
    .has_brightness = true,
    .draw_buf_psram = true,
    .draw_buf_lines = RU_DRAW_BUF_LINES,
    .flash_mb = 16,
    .rotations = 0xF, /* all four, by the CO5300 scan direction (MADCTL): no extra frame buffer, no tearing. UNVERIFIED on the glass. */
};
static const char *TAG = "ru_board_amoled216";
static int g_rot_applied;

const ru_board_t *ru_board(void) { return &k_board; }

/* Port functions: the Waveshare BSP is the only place that knows pins and drivers for this board. */

/* This BSP's panel init writes MADCTL 0xA0 (MV|MY), so the UPRIGHT picture is the scan direction that the amoled-175 BSP calls
 * BSP_DISPLAY_ROTATE_270, and the BSP's own touch start flags (swap_xy, mirror_y) match it. Our rotation s (90 degree steps from
 * upright) is therefore the BSP enum (3 + s) mod 4: 90 -> ROTATE_0 (0x00), 180 -> ROTATE_90 (0x60), 270 -> ROTATE_180 (0xC0). The four
 * MADCTL values are the four rotations of one cyclic group, so the offset composes. Touch keeps reporting upright-frame coordinates and
 * ru_hal maps them (ru_rot_to_logical), as on the amoled-175. Worked out from the bits, NOT seen on the panel. */
static bool bsp_rotation(int deg, bsp_display_rotation_t *out) {
  switch (deg) {
    case 90: *out = BSP_DISPLAY_ROTATE_0; return true;
    case 180: *out = BSP_DISPLAY_ROTATE_90; return true;
    case 270: *out = BSP_DISPLAY_ROTATE_180; return true;
    default: return false;
  }
}

/* The pixel-pair rounding the CO5300 wants (even start, odd end coordinates). Same as the BSP's rounder_event_cb. */
static void rounder_cb(lv_event_t *e) {
  lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
  a->x1 = (a->x1 >> 1) << 1;
  a->y1 = (a->y1 >> 1) << 1;
  a->x2 = ((a->x2 >> 1) << 1) + 1;
  a->y2 = ((a->y2 >> 1) << 1) + 1;
}

/* What bsp_display_start_with_config() does, with our own draw buffer height (the BSP hard-codes 50 lines). Every call used here is
 * public BSP or adapter API; bsp_display_new() keeps the panel and IO handles bsp_display_rotation_set() needs. */
static lv_display_t *display_start(bsp_display_cfg_t *cfg, lv_indev_t **indev) {
  if (esp_lv_adapter_init(&cfg->lv_adapter_cfg) != ESP_OK) return NULL;

  const bsp_display_config_t dcfg = {.max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8};
  esp_lcd_panel_handle_t panel = NULL;
  esp_lcd_panel_io_handle_t io = NULL;
  if (bsp_display_new(&dcfg, &panel, &io) != ESP_OK) return NULL;

  esp_lv_adapter_display_config_t dc = {
      .panel = panel,
      .panel_io = io,
      .profile =
          {
              .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
              .rotation = cfg->rotation,
              .hor_res = BSP_LCD_H_RES,
              .ver_res = BSP_LCD_V_RES,
              .buffer_height = RU_DRAW_BUF_LINES,
              .use_psram = true,
              .enable_ppa_accel = false,
              .require_double_buffer = true,
          },
      .tear_avoid_mode = cfg->tear_avoid_mode,
  };
  lv_display_t *disp = esp_lv_adapter_register_display(&dc);
  if (!disp) return NULL;
  lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

  esp_lcd_touch_handle_t tp = NULL;
  if (bsp_touch_new(cfg, &tp) != ESP_OK || !tp) return NULL;
  const esp_lv_adapter_touch_config_t tcfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
  *indev = esp_lv_adapter_register_touch(&tcfg);
  if (!*indev) return NULL;

  if (bsp_display_brightness_init() != ESP_OK) return NULL;
  if (esp_lv_adapter_start() != ESP_OK) return NULL;
  return disp;
}

lv_display_t *ru_port_start(lv_indev_t **indev, int rot) {
  /* the BSP's own bsp_display_start() configuration (see esp32_s3_touch_amoled_2_16.c), passed explicitly. The touch flags differ
   * from the amoled-175: this panel is initialised with MADCTL 0xA0, so touch needs swap_xy and mirror_y. */
  bsp_display_cfg_t cfg = {
      .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
      .rotation = ESP_LV_ADAPTER_ROTATE_0, /* the adapter does not rotate: the panel does */
      .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
      .touch_flags = {.swap_xy = 1, .mirror_x = 0, .mirror_y = 1},
  };
  lv_indev_t *touch = NULL;
  lv_display_t *disp = display_start(&cfg, &touch);
  g_rot_applied = 0;
  bsp_display_rotation_t br;
  if (disp && rot != 0 && bsp_rotation(rot, &br)) {
    esp_err_t e = bsp_display_rotation_set(br);
    if (e == ESP_OK) g_rot_applied = rot;
    else ESP_LOGE(TAG, "bsp_display_rotation_set(%d): %s, staying upright", rot, esp_err_to_name(e));
  }
  /* display_start() returns NULL if touch init fails; if it returned a display we have touch. */
  *indev = disp ? touch : NULL;
  return disp;
}

int ru_port_rotation(void) { return g_rot_applied; }

/* The adapter's lock directly: this BSP's bsp_display_lock() returns bool but passes on the adapter's esp_err_t, so ESP_OK (0) would
 * read as "not taken". ms == UINT32_MAX becomes -1: wait forever. */
bool ru_port_lock(uint32_t ms) { return esp_lv_adapter_lock((int32_t)ms) == ESP_OK; }
void ru_port_unlock(void) { esp_lv_adapter_unlock(); }
void ru_port_brightness_set(int percent) { bsp_display_brightness_set(percent); }
int ru_port_brightness_get(void) { return bsp_display_brightness_get(); }
