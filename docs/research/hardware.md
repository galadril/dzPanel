# dzPanel hardware and toolchain research

Date: 2026-10-31. Scope: Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466) and -2.16 (480x480).

**Method and confidence caveat.** Direct page fetches (WebFetch, curl) were blocked by the sandbox proxy. Every fact below comes from web-search result summaries of the cited pages, not from reading the pages in full. Treat all facts as secondary until checked against the primary source. Items I could not confirm are marked **UNVERIFIED**. Conflicts between sources are marked **CONFLICT**.

Primary sources to re-read first:
- 1.75 docs: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75
- 1.75 wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75
- 1.75 repo: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75
- 1.75 hardware reference: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/HARDWARE_REFERENCE.md
- 1.75 schematic: https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75/ESP32-S3-Touch-AMOLED-1.75.pdf
- 2.16 docs: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16
- 2.16 repo: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16
- 2.16 product page: https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm

## 1. Summary

- Both boards: ESP32-S3, 16 MB flash, 8 MB PSRAM, CO5300 AMOLED controller over QSPI, capacitive touch over a shared I2C bus (SDA=GPIO15, SCL=GPIO14), AXP2101 PMIC, QMI8658 IMU, PCF85063 RTC, ES8311 codec with ES7210 mic ADC, microSD, native USB-C.
- Display controller is the same on both (CO5300), so one display driver can serve both with different resolution/offset config.
- Touch controller differs: CST9217 on the 1.75, CST9220 on the 2.16. UNVERIFIED whether the same driver works for both. The 2.16 BSP depends on Waveshare's `esp_lcd_touch_cst9217` component, which suggests a shared driver family.
- IO expander: TCA9554 on the 1.75. One third-party source says the 2.16 has none (UNVERIFIED). This is the biggest board-profile difference to check.
- Official baseline: ESP-IDF v5.5.x with LVGL v9 for both. Arduino examples exist for both. Arduino examples use LVGL v8.4 on the 2.16.

## 2. Board comparison table

| Item | 1.75 | 2.16 | Source |
|---|---|---|---|
| Resolution / shape | 466x466 round | 480x480, "round" per the project brief. UNVERIFIED that the panel is physically round | 1.75: https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm ; 2.16: https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm |
| SoC | ESP32-S3, dual LX7 240 MHz | ESP32-S3R8 | https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75 ; https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16 |
| Flash | 16 MB external | 16 MB NOR. **CONFLICT**: one third-party repo says 8 MB | https://github.com/waveshareteam/ESP32-AIChats/issues/20 |
| PSRAM | 8 MB stacked | 8 MB. Mode (octal or quad) UNVERIFIED for both. One third-party source says "8 MB OPI" for the 2.16 | https://github.com/waveshareteam/ESP32-AIChats/issues/20 |
| Display IC / bus | CO5300, QSPI | CO5300, QSPI | https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75 ; https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16 |
| Touch IC | CST9217, I2C 0x5A | CST9220, I2C 0x5A | same |
| Touch INT / RST | INT=GPIO11. RST: **CONFLICT** (see 3.1) | INT=GPIO11, RST=GPIO40 | https://github.com/esp-rs/esp-hal/issues/6330 ; 2.16 docs |
| IO expander | TCA9554 (EXIO4 = PWR button state) | None per one third-party source. UNVERIFIED | https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/HARDWARE_REFERENCE.md ; https://github.com/waveshareteam/ESP32-AIChats/issues/20 |
| PMIC | AXP2101 (0x34) | AXP2101 (0x34) | docs pages |
| RTC | PCF85063 (0x51) | PCF85063 (0x51) | same |
| IMU | QMI8658 (0x6B). INT2=GPIO21, INT1 via TCA9554 | QMI8658 (0x6B). INT1 **CONFLICT**: GPIO17 (Waveshare docs) vs GPIO13 (two GitHub issues) | same |
| Audio | ES8311 + ES7210 | ES8311 + ES7210 (ES8311 0x18, ES7210 0x40) | same |
| Mics | Dual digital mic array | Dual mics with echo-cancellation circuit | product pages |
| Speaker | Onboard codec/amp path. Speaker connector type UNVERIFIED. PA_CTRL=GPIO46 | A speaker is in a retailer feature list. UNVERIFIED connector. PA pin UNVERIFIED | https://www.robotshop.com/products/waveshare-esp32-s3-216-amoled-touch-display-development-board-480480-dual-microphones-wi-fi-bluetooth-5-no-battery |
| microSD | GPIO1 CMD, GPIO2 CLK, GPIO3 D0 (1-bit SDMMC). GPIO41 = D3/CS, unused by BSP | GPIO1 CMD/MOSI, GPIO2 CLK, GPIO3 D0/MISO. CS pin not listed in results | HARDWARE_REFERENCE.md ; 2.16 docs |
| Buttons | BOOT=GPIO0. PWR via AXP2101, read on EXIO4 | BOOT=GPIO0, KEY3=GPIO18 (10k pull-up), PWR button on AXP2101 | same |
| USB | Native USB-C | Native USB-C, D-=GPIO19, D+=GPIO20 | same |
| Variants | Standard, -B (enclosure), -G (GNSS). A separate "1.75C" board has 32 MB flash and is a different product | Not checked | https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C |
| ESP-IDF BSP | `waveshare/esp32_s3_touch_amoled_1_75` v1.0.2 | `waveshare/esp32_s3_touch_amoled_2_16` v2.0.1 | https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_75 ; https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_2_16/versions/2.0.1/readme |

## 3. Per-board detail

### 3.1 ESP32-S3-Touch-AMOLED-1.75 (466x466)

Sources: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75 , https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/HARDWARE_REFERENCE.md , https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75

- SoC: dual-core Xtensa LX7 up to 240 MHz, 2.4 GHz Wi-Fi, BLE 5. 512 KB SRAM, 384 KB ROM, 8 MB stacked PSRAM, 16 MB external flash.
- Display: CO5300 over QSPI.
- Touch: CST9217, I2C 0x5A.
- PMIC/battery: AXP2101 (0x34), lithium battery interface.
- RTC: PCF85063 (0x51), powered via the AXP2101. RTC interrupt may be on EXIO3. UNVERIFIED.
- IMU: QMI8658 (0x6B).
- Audio: ES8311 (DAC/speaker) and ES7210 (mic ADC) sharing I2S.
- microSD, native USB-C, BOOT and PWR buttons.

Pin map (partly conflicting, see below):
- I2C bus: SDA=GPIO15, SCL=GPIO14 (shared by touch, AXP2101, QMI8658, PCF85063, TCA9554, codecs). Source: https://github.com/esp-rs/esp-hal/issues/6330 and the 2.16 docs for the same bus.
- Display QSPI: SCK=GPIO38, D0..D3=GPIO4,5,6,7, CS=GPIO12. Source: https://lcdwizard.com/boards/waveshare-s3-touch-amoled-175c (this page is for the **1.75C**, not the 1.75) and the esp-hal issue above.
- LCD reset: GPIO1 (LCD Wizard, 1.75C) vs GPIO39 (Waveshare hardware reference and schematic text). **CONFLICT.** GPIO1 is SD_CMD on the 1.75, so GPIO1 for LCD reset is unlikely on this board. LCD reset may also be via the TCA9554. UNVERIFIED.
- Touch: INT=GPIO11. Touch RST: GPIO2 (LCD Wizard, 1.75C) vs GPIO40 (Waveshare reference). **CONFLICT.** GPIO2 is SD_CLK on the 1.75, so GPIO2 is unlikely. Touch RST may be via the TCA9554. UNVERIFIED.
- Audio: MCLK=GPIO42, BCLK=GPIO9, LRCK/WS=GPIO45, DOUT=GPIO8 (to ES8311), DIN=GPIO10 (from ES7210), PA_CTRL=GPIO46. Source: search summary of the Waveshare hardware reference and schematic. UNVERIFIED against the primary file.
- SD: CMD=GPIO1, CLK=GPIO2, D0=GPIO3, D3/CS=GPIO41 (unused by BSP). Source: HARDWARE_REFERENCE.md.
- Buttons: BOOT=GPIO0. PWR state on TCA9554 P4/EXIO4 (SYS_OUT, high = pressed). Source: HARDWARE_REFERENCE.md and firmware README https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/firmware/README.md
- IMU INT2=GPIO21. INT1 via TCA9554.
- UART0: TX=GPIO43, RX=GPIO44.
- TCA9554 I2C address on the 1.75: UNVERIFIED (0x20 is the default A2:A0=000 and was reported for the 1.8 V2 board only).
- Full EXIO0-3 and EXIO5-7 assignments: UNVERIFIED. Read HARDWARE_REFERENCE.md.

USB / boot:
- Type-C is ESP32-S3 native USB for flash and serial/JTAG. UNVERIFIED whether it enumerates as USB-Serial-JTAG (CDC) in the stock firmware. This is the S3 default on GPIO19/20.
- Download mode: hold BOOT, press and release RESET, then release BOOT. Source: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75 (via search summary).

### 3.2 ESP32-S3-Touch-AMOLED-2.16 (480x480)

Sources: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16 , https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16 , https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm

- SoC: ESP32-S3R8 (8 MB PSRAM), 16 MB NOR flash (see conflict in the table).
- Display: CO5300 over QSPI, 480x480. Display pins reported as GPIO4/5/6/7 (data), 38 (SCK), 12 (CS), 39. Role of GPIO39 (likely LCD reset) UNVERIFIED.
- Touch: CST9220, I2C 0x5A, SDA=GPIO15, SCL=GPIO14, INT=GPIO11, RST=GPIO40.
- I2C addresses: ES8311 0x18, ES7210 0x40, QMI8658 0x6B, AXP2101 0x34, PCF85063 0x51.
- Audio I2S: MCLK=GPIO42, BCLK=GPIO9, LRCK=GPIO45, DOUT=GPIO8 (ES8311), DIN=GPIO10 (ES7210). Same as the 1.75 per search summary.
- Battery: MX1.25 2-pin 3.7 V lithium header, charge/discharge via the AXP2101. GPIO16 is marked SYS_OUT on the schematic.
- Buttons: BOOT=GPIO0 (also user button in the IDF example), KEY3=GPIO18, PWR button on the AXP2101.
- microSD: GPIO1, GPIO2, GPIO3 as listed in the table.
- USB: native USB-C, GPIO19 (D-) and GPIO20 (D+), used for programming and logs.
- Download mode: hold BOOT, plug in USB, release BOOT. Source: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16/Development-Environment-Setup-ESP-IDF
- IO expander: a third-party issue says none. UNVERIFIED. https://github.com/waveshareteam/ESP32-AIChats/issues/20
- Community bring-up in Rust (CO5300 over QSPI, PSRAM framebuffer, 480x480): https://github.com/okhsunrog/waveshare-esp32s3-amoled-2-16

## 4. Software support and examples

### 4.1 1.75
- Repo: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75
- ESP-IDF examples are listed against ESP-IDF v5.5.4 and v6.0.2 (another repo snapshot says v5.5.5 and v6.0.2). The Brookesia firmware uses v5.5.4.
- ESP-IDF examples include `02_lvgl_demo_v9` (LVGL 9, double buffering, DMA, anti-tearing per Waveshare) and `03_esp-brookesia` (phone-style UI).
- Arduino examples tested with Arduino-ESP32 3.3.10, with bundled libraries under `examples/arduino/libraries`. Arduino LVGL version UNVERIFIED. Arduino examples cover RTC clock, IMU UI, AXP2101 battery, music UI widgets, microSD, ES8311 audio.
- Arduino docs: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75/Arduino
- Firmware binaries: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/firmware/README.md

### 4.2 2.16
- Repo: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16 (ESP-IDF and Arduino examples, factory recovery firmware, schematics).
- Wiki states ESP-IDF V5.5 or above is required. Screenshots use v5.5.2. Examples in `examples/ESP-IDF-v5.5`, including `02_lvgl_demo_v9`. Source: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16/Development-Environment-Setup-ESP-IDF
- Arduino examples in `Arduino-v3.3.5`. Use the matching Arduino-ESP32 core. Wiki troubleshooting says use LVGL v8.4.0, not v9, for the Arduino examples. Source: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16/Arduino
- Arduino libraries named: SensorLib (PCF85063, QMI8658), XPowersLib (AXP2101).

### 4.3 ESP-IDF component registry
- Display: `espressif/esp_lcd_co5300` supports SPI, QSPI and MIPI-DSI, all targets. Highest version seen is 2.2.0 (the "latest" labels on the registry pages were inconsistent). https://components.espressif.com/components/espressif/esp_lcd_co5300 . A third-party QSPI-only fork exists: https://components.espressif.com/components/kodediy/esp_lcd_co5300
- Touch: `waveshare/esp_lcd_touch_cst9217` (versions 1.0.3, 1.0.4 and 2.0.0 seen). https://components.espressif.com/components/waveshare/esp_lcd_touch_cst9217 . The 2.16 BSP is listed as a dependent of it, so the 2.16 CST9220 may be handled by this same driver. UNVERIFIED. No dedicated `cst9220` component was found.
- BSPs: `waveshare/esp32_s3_touch_amoled_1_75` (1.0.2) and `waveshare/esp32_s3_touch_amoled_2_16` (2.0.1). The BSP dependency lists (CO5300 driver, LVGL version) were not retrieved. UNVERIFIED. Read each BSP's `idf_component.yml`.
- Other Waveshare AMOLED boards use the same CO5300 pattern (1.8-inch uses SH8601 + FT3168, which is a different controller family): https://www.amazon.com/Waveshare-ESP32-S3-Capacitive-Dual-core-Processor/dp/B0DSVK5576

### 4.4 ESPHome
- 1.75 is listed in ESPHome devices: https://devices.esphome.io/devices/waveshare-esp32-s3-touch-amoled-175/
- ESPHome `qspi_dbi` with `model: CUSTOM` is used for CO5300 on a related 2.06 board: https://community.home-assistant.io/t/waveshare-esp32-s3-touch-amoled-2-06-watch/914798

## 5. Recommendations

These are my judgement, based on the sources above.

- **Framework:** ESP-IDF with `idf.py`. Both boards ship official IDF examples and BSPs on the registry. Arduino examples exist but the 2.16 Arduino path is pinned to LVGL v8.4, which diverges from the IDF/LVGL 9 path.
- **ESP-IDF version:** v5.5.x. Both vendors' examples support it. The 2.16 requires >= 5.5. The 1.75 also lists v6.0.2, but v5.5.x is the common denominator. Pin to the latest 5.5 patch the BSPs build on (v5.5.4 was named for the 1.75 firmware).
- **LVGL:** v9.x (the IDF `02_lvgl_demo_v9` examples on both boards). Exact minor version UNVERIFIED. Read the example `idf_component.yml` and pin that.
- **idf.py vs PlatformIO:** idf.py. I found no source showing PlatformIO support for these boards. PlatformIO's espressif32 platform typically lags ESP-IDF releases, which matters for 5.5+. UNVERIFIED for these boards specifically.
- **Drivers:** use `espressif/esp_lcd_co5300` for both displays. Use `waveshare/esp_lcd_touch_cst9217` as the first attempt for both touch controllers. Use the vendor BSPs as reference, and wrap them behind the dzPanel board profile.
- **Board profile fields to include:** resolution, display offsets, QSPI pins, reset method (GPIO vs TCA9554), touch RST method, IO expander present, I2C map, IMU INT pins, audio pins, SD pins, button map.

## 6. Open hardware questions to verify on arrival

1. Exact flash size on the 2.16 (16 MB per Waveshare, 8 MB per one third party). Check with `esptool flash_id`.
2. PSRAM mode (octal or quad) and actual size on each board. Check at boot log.
3. Is there a TCA9554 on the 2.16? Run an I2C scan on both boards and record addresses.
4. Real LCD reset and touch reset wiring on the 1.75 (GPIO39/GPIO40 vs TCA9554 EXIO vs GPIO1/GPIO2 from the 1.75C data). GPIO1/GPIO2 are SD pins on the 1.75.
5. What is GPIO39 on the 2.16 (LCD reset?) and does the 2.16 touch RST really use GPIO40?
6. QMI8658 INT1 pin on the 2.16 (GPIO17 vs GPIO13).
7. Does the CST9217 driver work on the CST9220 (2.16)? Check chip ID readout and touch coordinates.
8. Is the 2.16 panel round or square with rounded corners? Confirm the visible circular area and set the safe area.
9. Does the USB-C port enumerate as USB-Serial-JTAG (CDC) in the stock firmware and in a minimal IDF build? Does auto-reset via `idf.py flash` work without manual BOOT?
10. Speaker output: connector type, PA enable pin on the 2.16, and amplifier power.
11. PWR button behavior: how the AXP2101 handles long-press power-off and how firmware reads it on each board (EXIO4 on the 1.75; the 2.16 path UNVERIFIED).
12. Battery: charge current default, battery-present detection, and whether boards run correctly with USB only and no battery.
13. SD wiring on each board: 1-bit SDMMC only, or CS/D3 usable (GPIO41)?
14. Board revision and variant markings (1.75 vs -B vs -G vs 1.75C) and which repo/BSP matches.
15. Audio I2S pin map on the 2.16 matches the 1.75 (it was reported identical, confirm in the schematic).
16. Licences of Waveshare example code and BSP components before copying into dzPanel.
