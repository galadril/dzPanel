/* Guition ESP32-S3-4848S040 pin map and panel timing. Header-only macros (no includes) so the bring-up probe can print them.
 *
 * Pins were cross-checked against four independent sources: the ESPHome device page for the Guition 4848S040,
 * Arduino_GFX (moononournation, st7701_type9 / Guition 4848S040 example), the rzeldent platformio-espressif32-sunton board
 * files, and NorthernMan54/ESP32-4848S040. NOT verified on the real board yet.
 *
 * Only pins listed in this file are ever configured. Deliberately NOT touched: GPIO1, 2, 40 (I2S amp / relay-or-LED lines of
 * unconfirmed function), GPIO17 as a relay (it is VSYNC here), GPIO41/42 (SD), GPIO35-37 (octal PSRAM). Relays are out of scope. */
#pragma once

/* ---- Panel geometry ---- */
#define RU_GUITION_HRES 480
#define RU_GUITION_VRES 480

/* ---- RGB control lines ---- */
#define RU_GUITION_PIN_DE 18
#define RU_GUITION_PIN_HSYNC 16
#define RU_GUITION_PIN_VSYNC 17
#define RU_GUITION_PIN_PCLK 21

/* ---- RGB565 data lines. Wiring per colour component (LSB first within each component). ---- */
#define RU_GUITION_PIN_R0 11
#define RU_GUITION_PIN_R1 12
#define RU_GUITION_PIN_R2 13
#define RU_GUITION_PIN_R3 14
#define RU_GUITION_PIN_R4 0
#define RU_GUITION_PIN_G0 8
#define RU_GUITION_PIN_G1 20
#define RU_GUITION_PIN_G2 3
#define RU_GUITION_PIN_G3 46
#define RU_GUITION_PIN_G4 9
#define RU_GUITION_PIN_G5 10
#define RU_GUITION_PIN_B0 4
#define RU_GUITION_PIN_B1 5
#define RU_GUITION_PIN_B2 6
#define RU_GUITION_PIN_B3 7
#define RU_GUITION_PIN_B4 15
/* esp_lcd data_gpio_nums initializer, data0 first: indices 0-4 = B0..B4, 5-10 = G0..G5, 11-15 = R0..R4. */
#define RU_GUITION_DATA_GPIOS                                                                                          \
  {RU_GUITION_PIN_B0, RU_GUITION_PIN_B1, RU_GUITION_PIN_B2, RU_GUITION_PIN_B3, RU_GUITION_PIN_B4,                   \
   RU_GUITION_PIN_G0, RU_GUITION_PIN_G1, RU_GUITION_PIN_G2, RU_GUITION_PIN_G3, RU_GUITION_PIN_G4, RU_GUITION_PIN_G5, \
   RU_GUITION_PIN_R0, RU_GUITION_PIN_R1, RU_GUITION_PIN_R2, RU_GUITION_PIN_R3, RU_GUITION_PIN_R4}

/* ---- ST7701 3-wire SPI (software; 9-bit frames: D/C bit then 8 bits MSB first). No IO expander, no reset GPIO. ---- */
#define RU_GUITION_PIN_LCD_CS 39
#define RU_GUITION_PIN_LCD_SCK 48
#define RU_GUITION_PIN_LCD_SDA 47

/* ---- Backlight: GPIO38, active HIGH, LEDC PWM ---- */
#define RU_GUITION_PIN_BL 38
#define RU_GUITION_BL_FREQ_HZ 2000

/* ---- GT911 touch: I2C, INT and RST not connected. Address is 0x5D or 0x14 (probed in that order). ---- */
#define RU_GUITION_PIN_TOUCH_SDA 19
#define RU_GUITION_PIN_TOUCH_SCL 45 /* GPIO45 is a strapping pin (VDD_SPI) */
#define RU_GUITION_PIN_TOUCH_INT (-1)
#define RU_GUITION_PIN_TOUCH_RST (-1)
#define RU_GUITION_TOUCH_I2C_HZ 400000

/* ---- Colour order: UNRESOLVED (sources disagree). 0 = RGB (panel MADCTL 0x00, what the Arduino_GFX table yields: it never
 * writes MADCTL), 1 = BGR (MADCTL bit 3, 0x08). Symptom of the wrong value: red and blue swapped. Build with
 * -DRU_GUITION_BGR=1 (or edit here) and rerun the probe's red/green/blue screens to settle it. ---- */
#ifndef RU_GUITION_BGR
#define RU_GUITION_BGR 0
#endif

/* ---- COLMOD (0x3A) written by the init table. The vendor table (Arduino_GFX) sends 0x60 (RGB666) even though only 16 data
 * lines are wired; it is the value used by the Arduino_GFX table for this board and is UNVERIFIED on this hardware. 0x50 would be the textbook RGB565 value; try it if colours
 * look crushed or noisy. The esp_lcd_st7701 component first sends 0x50 (bits_per_pixel = 16), then the table overrides it. ---- */
#ifndef RU_GUITION_COLMOD
#define RU_GUITION_COLMOD 0x60
#endif

/* ---- RGB timing start values (tune here). Refresh = pclk / ((480+hsw+hfp+hbp) * (480+vsw+vfp+vbp)). ---- */
#ifndef RU_GUITION_PCLK_HZ
#define RU_GUITION_PCLK_HZ (12 * 1000 * 1000)
#endif
#define RU_GUITION_HSYNC_PW 8
#define RU_GUITION_HSYNC_FP 10
#define RU_GUITION_HSYNC_BP 50
#define RU_GUITION_VSYNC_PW 8
#define RU_GUITION_VSYNC_FP 10
#define RU_GUITION_VSYNC_BP 20
#define RU_GUITION_PCLK_ACTIVE_NEG 0
#define RU_GUITION_HSYNC_IDLE_LOW 0
#define RU_GUITION_VSYNC_IDLE_LOW 0
#define RU_GUITION_DE_IDLE_HIGH 0
#define RU_GUITION_PCLK_IDLE_HIGH 0
/* Bounce buffer: 20 lines of 480 px (two of them are allocated in internal DMA RAM, 2 x 19.2 KB; the 10-line buffers of the
 * earlier builds were 2 x 9.6 KB, so +19.2 KB internal). Must divide h_res*v_res; the driver refills it from the PSRAM frame
 * buffer, which keeps the pixel clock fed while the octal PSRAM is busy (XIP-from-PSRAM). A longer buffer gives the refill ISR
 * twice the time before the panel runs dry (a late refill shifts the picture for a frame). The board falls back to 10 lines
 * when internal RAM cannot take 20 (panel_new_bounce logs which one was used). */
#define RU_GUITION_BOUNCE_PX (RU_GUITION_HRES * 20)
