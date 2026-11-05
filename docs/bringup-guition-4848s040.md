# Guition ESP32-S3-4848S040 bring-up

Nothing here has run on the real board yet. Flash the **probe first**, confirm panel and touch, then flash dzPanel.

## Images

Both live in `dist/` (git-ignored): `guition-4848s040-probe-merged.bin` and `dzpanel-guition-4848s040-merged.bin`. Build with `tools/build_probe.sh guition-probe` and `tools/build_fw.sh guition-4848s040`, one at a time (shared `managed_components`).

## Flashing (Windows)

```
python -m esptool --chip esp32s3 -p COMx -b 460800 write_flash 0x0 <image>.bin
```

If the connection fails, hold BOOT, tap RST, release BOOT, and retry. The probe image carries its own bootloader and partition table: **after the probe you must flash the full dzPanel image** (same command) to get dzPanel's layout back.

## What to look for in the probe (UART0, 115200, CH341 bridge)

- Serial: chip info, `display started; backlight 100%`, `i2c: device found at 0x5D` (or 0x14), then the colour and touch test lines. `ru_port_start FAILED` names the failing stage above it.
- Screen: full-screen red, green, blue, white (2 s each), then 8 colour bars with a white 1-px border, then the touch crosshair.
- Colour order: red shown as blue means swapped; rebuild with `-DRU_GUITION_BGR=1` (or 0 if it is 1 now). Noise or crushed levels: `RU_GUITION_COLMOD` (0x60 is from the Arduino_GFX table, unverified here; try 0x50).
- Timing: a missing border edge, shift, flicker or snow: lower `RU_GUITION_PCLK_HZ`, tune porches / `pclk_active_neg` in `components/boards/guition-4848s040/ru_board/board_pins.h`.
- Touch axes: top-left must log `corner NW` with small x and y; if swapped or mirrored, note which (swap_xy, mirror_x, mirror_y).

Opening the port from a terminal or Web Serial toggles DTR/RTS and resets the board.
