# Verified hardware facts (Stage 1)

Checked 2026-10-31 against the cloned vendor repos in `vendor/` (both Apache-2.0):
`waveshareteam/ESP32-S3-Touch-AMOLED-1.75` and `waveshareteam/ESP32-S3-Touch-AMOLED-2.16`.
Anything not listed here is still unverified (see `DESIGN.md` section 14).

## Resolved

| Claim in DESIGN.md | Result | Source |
|---|---|---|
| 1.75": CO5300, CST9217, TCA9554, AXP2101, PCF85063, QMI8658, ES8311, ES7210 | **Confirmed**. 8 MB PSRAM, 16 MB flash | 1.75 `HARDWARE_REFERENCE.md` |
| 1.75" pins: I²C SDA 15 / SCL 14, TP_INT 11, TP_RST 40, LCD_RST 39, LCD_CS 12, LCD_TE 13, PCLK 38, DATA0-3 = 4-7 | **Confirmed** | same |
| Display reset and touch reset are separate GPIOs | **Confirmed** (39 and 40) | same |
| Audio MCLK on GPIO16 (one research source) | **Wrong**. MCLK is GPIO42. GPIO16 is a free header GPIO | same |
| AMOLED brightness | Panel command only, no backlight PWM | same |
| PWR button | Via AXP2101, readable on TCA9554 P4 | same |
| 2.16": CO5300 480x480 QSPI, CST9220, AXP2101, QMI8658, ES7210, ES8311 | **Confirmed** | 2.16 `README.md` |
| 2.16" display/touch/I²C pins | Same as 1.75" (LCD 4-7/38/12/39, I²C 15/14, TP 11/40) | 2.16 `Mylibrary/pin_config.h` |
| 2.16" has no IO expander | **Plausible, not proven**: `pin_config.h` drives LCD and touch reset straight from GPIO 39/40. Confirm with an I²C scan (C0) | same |
| LVGL version | Both demos pin `lvgl/lvgl` 9.x (1.75: `9.4.*`) | `02_lvgl_demo_v9/main/idf_component.yml` |
| BSP packages | `waveshare/esp32_s3_touch_amoled_1_75` (`*`), `waveshare/esp32_s3_touch_amoled_2_16` (`^2.0.1`) | same |
| Toolchain | 2.16 CI tests ESP-IDF **5.5.5 and 6.0.2** | 2.16 `README.md` |
| 2.16" firmware | Factory image is a recovery binary only; no source | 2.16 `docs/firmware.md` |

## New finding that changes the design

**The 2.16" panel is a rounded square, not a circle.** The vendor product render shows a square display with rounded corners (`docs/assets/ESP32-S3-Touch-AMOLED-2.16-details-1.jpg` in the 2.16 repo). `DESIGN.md` assumes both boards are round, with a circular safe radius `Rs = 0.92 R`.

Consequences to decide (proposed ADR-008):

- The board profile needs a `shape` field: `round` or `rounded_rect` (with a corner radius).
- Geometry (`ru_ui/geom`) needs a safe-area function per shape. For the square the safe area is nearly the full rectangle minus the corner arcs, so chord-width maths only applies to the 1.75".
- The "ring" idiom still works on the square, but there is room for square-friendly layouts. One layout per shape, not pixel constants in apps.
- Corner radius is still unmeasured. The mockups use 9% of the width as a stand-in.

## Added later, unverified on hardware

These come from reading the BSP and adapter sources, not from a board.

- **Screen rotation, AMOLED 1.75.** `bsp_display_rotation_set` writes MADCTL (`0x60` for 90, `0xC0` for 180, `0xA0` for 270). Working through MV / MX / MY, that is the same mapping as `ru_rot_to_panel` (`ru_geom.h`, LVGL's convention). Not seen on the panel: whether the CO5300 column/row window offsets follow the scan direction (a shift of a few pixels would show), and whether the touch controller reports the same coordinates after the change (we assume it does: touch stays in panel coordinates and `ru_hal` maps them).
- **Screen rotation, Guition 4848S040.** `esp_lv_adapter` rotates in software and refuses a rotation under `TEAR_AVOID_MODE_NONE`, so a rotated start uses `TRIPLE_PARTIAL` and 3 frame buffers in PSRAM (`esp_lv_adapter_get_required_frame_buffer_count`). The adapter's `ROTATE_90` maps logical (x, y) to panel (D-1-y, x), which is our 270 (the names are swapped in `ru_board_guition_4848s040.c`). Unverified: that the RGB panel with its bounce buffer runs with three PSRAM buffers, tearing, and the PSRAM cost (1.3 MB). If it does not, set `.rotations = 0x1` for that board and the Settings screen offers only the rotation in force.

## Still open

- 2.16" flash size (8 vs 16 MB) and whether it has a TCA9554: needs the schematic PDF (`schematic/*.pdf`) or a board.
- 2.16" corner radius and visible-area inset.
- Exact BSP versions to pin: read the registry manifest when ESP-IDF is installed.
