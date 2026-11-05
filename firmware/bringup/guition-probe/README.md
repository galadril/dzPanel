# Guition ESP32-S3-4848S040 bring-up probe

A small IDF project that exercises only the panel (ST7701S over RGB), the backlight and the GT911 touch controller, using the
same `ru_board` component and sdkconfig defaults as the dzPanel firmware (`RU_BOARD=guition-4848s040`). No networking, no dzPanel UI.
Nothing here has run on the real board yet.

## Build

```
tools/build_probe.sh guition-probe            # -> firmware/bringup/guition-probe/build-guition-probe/
```

The first configure downloads the managed components (esp_lcd_st7701, esp_lcd_panel_io_additions, esp_lcd_touch_gt911,
esp_lvgl_adapter, lvgl) into `managed_components/` and is slow. Pins and timing live in
`components/boards/guition-4848s040/ru_board/board_pins.h`; every value can be overridden there.

Flash the merged image at offset 0x0 (see docs/flashing.md; `idf.py -B build-guition-probe merge-bin` or the image in `dist/`).
Monitor: UART0 (GPIO43/44 through the CH341 bridge) at **115200**, e.g. `idf.py -p COMx monitor` or any terminal. Native USB is not usable.

Serial caveat: GPIO0 is both the boot strap and RGB R4. The CH341 DTR/RTS auto-reset can leave GPIO0 low after a port is opened and the chip boots into the ROM download loader (black screen, silent console, ROM prints `waiting for download`). Open the port with DTR and RTS deasserted (miniterm `--dtr 0 --rts 0`), then press RST once. The web installer page resets via DTR/RTS, so you may need to press RST after opening it.

Flashing the probe onto a provisioned board leaves the Wi-Fi/Domoticz config in NVS (it is not erased and the probe never reads it).

## What to expect

Serial, in order:

1. Chip info (ESP32-S3 rev v0.2 expected), flash 16 MB, PSRAM "present", octal, 8 MB, heap, reset reason, the configured colour
   order, COLMOD and timing values.
2. `display started; backlight 100%`. If instead `ru_port_start FAILED` appears, the error lines before it say which stage failed
   (3-wire SPI, ST7701 init, RGB panel allocation, adapter).
3. `i2c: device found at 0x5D` (or `0x14`) and `touch: GT911 initialised`. No devices on the bus means SDA=19 / SCL=45 or power is wrong.

Screen:

1. Full-screen red, green, blue, white, 2 s each. If the red screen shows blue (and blue shows red), the colour order is wrong:
   set `RU_GUITION_BGR` to the other value in `board_pins.h`, rebuild, reflash. Wrong colours other than a swap (noise, crushed
   levels) point at `RU_GUITION_COLMOD` (0x60 vs 0x50) or the data pin order.
2. Eight vertical bars (red, green, blue, yellow, cyan, magenta, grey, black) with a 1-px white border on all four edges and a label
   with the colour order and timing. All four border edges must be visible: a missing edge means the visible window is shifted
   (tune the porches). Shifted or rolling picture, flicker or snow: lower `RU_GUITION_PCLK_HZ` or adjust porches / `pclk_active_neg`.
3. Touch loop: a white crosshair follows the finger. Serial prints `touch x=.. y=.. -> corner NW|NE|SW|SE|centre`. Touching the
   top-left of the glass must print `corner NW` with small x and y; if axes are swapped or mirrored, note which (swap_xy / mirror_x /
   mirror_y in the board file).

Backlight: if the screen stays dark but serial says everything started, GPIO38 polarity or the PWM is wrong. If only a white or
black screen shows with the backlight on, the init table or the timing is wrong.
