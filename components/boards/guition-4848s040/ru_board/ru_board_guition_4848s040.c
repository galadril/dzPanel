#include "ru_board_guition_4848s040.h"
#include "ru_hal.h"
#include "ru_hal_port.h"
#include "board_pins.h"
#include "st7701_init_guition.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"

static const char *TAG = "ru_board_guition";

/* Guition ESP32-S3-4848S040: 480 px square, ST7701S RGB panel + GT911, 16 MB flash, 8 MB octal PSRAM. Pins: board_pins.h. */
static const ru_board_t k_board = {
    .id = "guition_4848s040",
    .diameter = 480,
    .safe_inset_px = 0,
    .round = false,
    .has_touch = true,
    .has_brightness = true,
    .draw_buf_psram = true,
    .draw_buf_lines = 50,
    .flash_mb = 16,
};

const ru_board_t *ru_board(void) { return &k_board; }

static int g_brightness;
static i2c_master_bus_handle_t g_bus;
static esp_lcd_touch_handle_t g_tp;
static uint8_t g_tp_addr;

i2c_master_bus_handle_t ru_guition_touch_bus(void) { return g_bus; }
esp_lcd_touch_handle_t ru_guition_touch(void) { return g_tp; }
uint8_t ru_guition_touch_addr(void) { return g_tp_addr; }

static esp_err_t backlight_init(void) {
  const ledc_timer_config_t t = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_10_BIT,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = RU_GUITION_BL_FREQ_HZ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "bl timer");
  const ledc_channel_config_t c = {
      .gpio_num = RU_GUITION_PIN_BL,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .timer_sel = LEDC_TIMER_0,
      .duty = 0, /* active high: 0 = off */
      .hpoint = 0,
  };
  return ledc_channel_config(&c);
}

void ru_port_brightness_set(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  g_brightness = percent;
  const uint32_t duty = (uint32_t)percent * 1023u / 100u;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
int ru_port_brightness_get(void) { return g_brightness; }

/* Startup failures are terminal: ru_port_start returns NULL and the caller never retries, so ru_port_start does not tear down
 * the adapter or backlight; panel_new and touch_new do release their own handles. */
/* Panel: 3-wire SPI IO -> esp_lcd_st7701 (vendor init table) -> RGB panel. */
static esp_err_t panel_new(esp_lcd_panel_handle_t *out_panel) {
  const esp_lcd_panel_io_3wire_spi_config_t io_cfg = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(
      ((spi_line_config_t){
          .cs_io_type = IO_TYPE_GPIO,
          .cs_gpio_num = RU_GUITION_PIN_LCD_CS,
          .scl_io_type = IO_TYPE_GPIO,
          .scl_gpio_num = RU_GUITION_PIN_LCD_SCK,
          .sda_io_type = IO_TYPE_GPIO,
          .sda_gpio_num = RU_GUITION_PIN_LCD_SDA,
          .io_expander = NULL,
      }),
      0 /* data sampled on the rising SCK edge */);
  esp_lcd_panel_io_handle_t io = NULL;
  esp_err_t e = esp_lcd_new_panel_io_3wire_spi(&io_cfg, &io);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "3-wire SPI io failed: %s", esp_err_to_name(e));
    return e;
  }

  const uint8_t nfb = esp_lv_adapter_get_required_frame_buffer_count(ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE, ESP_LV_ADAPTER_ROTATE_0);
  const esp_lcd_rgb_panel_config_t rgb = {
      .clk_src = LCD_CLK_SRC_DEFAULT,
      .timings =
          {
              .pclk_hz = RU_GUITION_PCLK_HZ,
              .h_res = RU_GUITION_HRES,
              .v_res = RU_GUITION_VRES,
              .hsync_pulse_width = RU_GUITION_HSYNC_PW,
              .hsync_front_porch = RU_GUITION_HSYNC_FP,
              .hsync_back_porch = RU_GUITION_HSYNC_BP,
              .vsync_pulse_width = RU_GUITION_VSYNC_PW,
              .vsync_front_porch = RU_GUITION_VSYNC_FP,
              .vsync_back_porch = RU_GUITION_VSYNC_BP,
              .flags =
                  {
                      .hsync_idle_low = RU_GUITION_HSYNC_IDLE_LOW,
                      .vsync_idle_low = RU_GUITION_VSYNC_IDLE_LOW,
                      .de_idle_high = RU_GUITION_DE_IDLE_HIGH,
                      .pclk_active_neg = RU_GUITION_PCLK_ACTIVE_NEG,
                      .pclk_idle_high = RU_GUITION_PCLK_IDLE_HIGH,
                  },
          },
      .data_width = 16,
      .bits_per_pixel = 16,
      .num_fbs = nfb,
      .bounce_buffer_size_px = RU_GUITION_BOUNCE_PX,
      .hsync_gpio_num = RU_GUITION_PIN_HSYNC,
      .vsync_gpio_num = RU_GUITION_PIN_VSYNC,
      .de_gpio_num = RU_GUITION_PIN_DE,
      .pclk_gpio_num = RU_GUITION_PIN_PCLK,
      .disp_gpio_num = -1,
      .data_gpio_nums = RU_GUITION_DATA_GPIOS,
      .flags = {.fb_in_psram = 1},
  };
  const st7701_vendor_config_t vendor = {
      .init_cmds = k_guition_init,
      .init_cmds_size = sizeof(k_guition_init) / sizeof(k_guition_init[0]),
      .rgb_config = &rgb,
      .flags = {.mirror_by_cmd = 0, .enable_io_multiplex = 0},
  };
  const esp_lcd_panel_dev_config_t dev = {
      .reset_gpio_num = -1, /* no reset line: the component sends the 0x01 software reset */
      .rgb_ele_order = RU_GUITION_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = 16,
      .vendor_config = (void *)&vendor,
  };
  esp_lcd_panel_handle_t panel = NULL;
  const char *stage = "st7701 panel";
  e = esp_lcd_new_panel_st7701(io, &dev, &panel);
  if (e == ESP_OK) {
    stage = "panel reset";
    e = esp_lcd_panel_reset(panel);
  }
  if (e == ESP_OK) {
    stage = "panel init"; /* sends the init table incl. sleep out and display on */
    e = esp_lcd_panel_init(panel);
  }
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "%s failed: %s", stage, esp_err_to_name(e));
    if (panel) esp_lcd_panel_del(panel); /* does not delete the 3-wire io (enable_io_multiplex = 0) */
    esp_lcd_panel_io_del(io);
    return e;
  }
  *out_panel = panel;
  return ESP_OK;
}

/* Touch: GT911 without INT/RST; try 0x5D then 0x14. A failure here leaves the display usable. */
static esp_err_t touch_new(esp_lcd_touch_handle_t *out) {
  const i2c_master_bus_config_t bus_cfg = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = RU_GUITION_PIN_TOUCH_SDA,
      .scl_io_num = RU_GUITION_PIN_TOUCH_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  esp_err_t e = i2c_new_master_bus(&bus_cfg, &g_bus);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "i2c bus failed: %s", esp_err_to_name(e));
    g_bus = NULL;
    return e;
  }
  static const uint8_t addrs[] = {ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP};
  uint8_t addr = 0;
  for (size_t i = 0; i < sizeof(addrs); i++) {
    if (i2c_master_probe(g_bus, addrs[i], 50) == ESP_OK) {
      addr = addrs[i];
      break;
    }
  }
  if (!addr) {
    ESP_LOGE(TAG, "no GT911 at 0x5D or 0x14: %s", esp_err_to_name(ESP_ERR_NOT_FOUND)); /* g_bus is kept for the probe scan */
    return ESP_ERR_NOT_FOUND;
  }
  esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
  io_cfg.dev_addr = addr;
  io_cfg.scl_speed_hz = RU_GUITION_TOUCH_I2C_HZ;
  esp_lcd_panel_io_handle_t tio = NULL;
  e = esp_lcd_new_panel_io_i2c(g_bus, &io_cfg, &tio); /* bus stays up for the probe scan */
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "touch io failed: %s", esp_err_to_name(e));
    return e;
  }
  const esp_lcd_touch_config_t tcfg = {
      .x_max = RU_GUITION_HRES,
      .y_max = RU_GUITION_VRES,
      .rst_gpio_num = GPIO_NUM_NC,
      .int_gpio_num = GPIO_NUM_NC,
      .levels = {.reset = 0, .interrupt = 0},
      .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
  };
  e = esp_lcd_touch_new_i2c_gt911(tio, &tcfg, out);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "gt911: %s", esp_err_to_name(e));
    esp_lcd_panel_io_del(tio); /* g_bus is kept on purpose: the bring-up probe scans it via ru_guition_touch_bus() */
    return e;
  }
  g_tp_addr = addr;
  return ESP_OK;
}

/* Startup failure: say so, because the backlight is only switched on at the very end and a dark screen must not be mistaken
 * for a polarity problem. */
static lv_display_t *start_failed(const char *stage, esp_err_t e) {
  ESP_LOGE(TAG, "%s failed: %s", stage, esp_err_to_name(e));
  ESP_LOGE(TAG, "display init failed: backlight stays OFF (this is not a backlight polarity problem)");
  return NULL;
}

lv_display_t *ru_port_start(lv_indev_t **indev) {
  *indev = NULL;
  esp_err_t e;
  const esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
  if ((e = esp_lv_adapter_init(&cfg)) != ESP_OK) return start_failed("esp_lv_adapter_init", e);
  if ((e = backlight_init()) != ESP_OK) return start_failed("backlight_init", e);

  esp_lcd_panel_handle_t panel = NULL;
  if ((e = panel_new(&panel)) != ESP_OK) return start_failed("panel_new", e);

  /* Bring-up: one frame buffer in PSRAM, no tear avoidance. */
  esp_lv_adapter_display_config_t dcfg = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(panel, NULL, RU_GUITION_HRES, RU_GUITION_VRES,
                                                                                    ESP_LV_ADAPTER_ROTATE_0);
  dcfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
  dcfg.profile.use_psram = true;
  lv_display_t *disp = esp_lv_adapter_register_display(&dcfg);
  if (!disp) return start_failed("register_display", ESP_FAIL);

  e = touch_new(&g_tp);
  if (e == ESP_OK) {
    const esp_lv_adapter_touch_config_t tc = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, g_tp);
    *indev = esp_lv_adapter_register_touch(&tc);
  } else {
    g_tp = NULL;
    ESP_LOGW(TAG, "touch init failed (%s); display only", esp_err_to_name(e));
  }
  if ((e = esp_lv_adapter_start()) != ESP_OK) return start_failed("esp_lv_adapter_start", e);
  ru_port_brightness_set(100); /* ru_hal does not set brightness at start */
  return disp;
}

bool ru_port_lock(uint32_t ms) { return esp_lv_adapter_lock(ms == UINT32_MAX ? -1 : (int32_t)ms) == ESP_OK; }
void ru_port_unlock(void) { esp_lv_adapter_unlock(); }
