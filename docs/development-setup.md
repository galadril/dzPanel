# Development setup

Everything here was run in the authoring sandbox (Linux, ESP-IDF v5.5.6) unless marked **not run**.

## 1. Third-party sources (not in the repo)

`vendor/` is git-ignored. Exact commits are pinned in `vendor.lock`; fetch them with:

```bash
tools/fetch_vendor.sh        # idempotent; shallow clones of the pinned commits
```

| Directory | What | Used for |
|---|---|---|
| `vendor/lvgl/src` | LVGL v9.4.0 | host simulator (the firmware gets the same version from the Waveshare BSP) |
| `vendor/domoticz` | Domoticz source (GPL-3.0) | **reading only**, to check API behaviour. Never copy code from it |
| `vendor/ESP32-S3-Touch-AMOLED-{1.75,2.16}` | Waveshare repos (Apache-2.0) | pins, schematics, examples. Copy an example to `/tmp` before building it; never build inside `vendor/` |

## 2. Host build and tests (no hardware, no ESP-IDF)

```bash
cmake -S . -B /tmp/vb -DRU_BUILD_SIM=OFF      # ASAN + UBSAN on by default, -Werror
cmake --build /tmp/vb -j8
cd /tmp/vb && ctest                           # 19 targets at the time of writing
node tests/site/test_lib.mjs                  # installer page logic
python3 -I -W ignore tests/tools/test_make_manifest.py
node sim/mock/test_mock.js                    # mock Domoticz server
```

`RU_BUILD_SIM=ON` also builds the LVGL simulator and the UI tests. Compiling LVGL takes several minutes: run it in the background and read the log.

Needs: a C11 compiler, CMake 3.16+, SDL2 development headers (simulator only), Node 18+ (mock and site tests), Python 3. `libcurl` headers are **not** required.

## 3. Mock Domoticz

```bash
node sim/mock/domoticz_mock.js --port 18080   # tokens: viewer-token (read), user-token (can switch)
```

It mirrors real 2026.4 response shapes (see `tests/fixtures/real_*.json`, sanitised captures) and can inject latency, 401/403, HTML error pages, hangs and truncated bodies.

## 4. Firmware toolchain

ESP-IDF **v5.5.6** and the Xtensa ESP32-S3 toolchain were installed read-only under `/opt/esp`. The IDF component manager insists on writing into `IDF_TOOLS_PATH`, which fails on a read-only install (`PermissionError ... root_managed_components`). Point it at a writable directory that symlinks the real tools:

```bash
mkdir -p ~/.idf-shadow && for e in /opt/esp/tools/*; do ln -sfn "$e" ~/.idf-shadow/$(basename "$e"); done   # once
export IDF_TOOLS_PATH=~/.idf-shadow
. /opt/esp/idf/export.sh
tools/build_fw.sh amoled-175            # or guition-4848s040; output in firmware/build-<board>
```

The board is chosen at build time with `RU_BOARD` (`tools/build_fw.sh <board>` passes `-DRU_BOARD=<board> -B build-<board>`). Each board has `components/boards/<board>/` (the `ru_board` component), `firmware/boards/<board>/sdkconfig.defaults` and its own `firmware/dependencies.lock.<board>`. Use one build directory per board: the sdkconfig lives in the build dir, and CMake refuses to reuse a build dir for a different board. The Guition bring-up probe is built with `tools/build_probe.sh` (see `firmware/bringup/guition-probe/README.md` and `docs/bringup-guition-4848s040.md`).

On a normal machine with a writable ESP-IDF install, none of the shadow directory is needed.

Practical notes:

- Managed components (amoled-175: Waveshare BSP, LVGL 9.4.0, CO5300 display, CST9217 touch, TCA9554 expander; guition-4848s040: esp_lvgl_adapter, esp_lcd_st7701, esp_lcd_panel_io_additions, esp_lcd_touch_gt911; exact versions in the per-board lock files) download from the Espressif registry on the first `set-target`. On a slow filesystem (a Windows mount under WSL) this took over 20 minutes. Run it in the background and let it finish.
- The Guition board uses a CH341 USB-UART bridge on UART0. Opening the port from a Web Serial page toggles DTR/RTS, which auto-resets the board, so the installer page may restart the device when it connects.
- ESP log lines share UART0 with the RU+CFG and Improv framing, so a host-side parser must tolerate (skip) log lines between frames.
- Builds are slow: run `idf.py build` in the background and wait on the log.
- The unmodified Waveshare LVGL demo for the 1.75" board builds with this setup (about 1 MB, 12% of an 8 MB slot).

## 5. Flashing

`docs/flashing.md`. **No board has been flashed**: nothing in the firmware has run on hardware.

## 6. Secrets

- Host tools read the Domoticz token from `DOMOTICZ_ACCESSTOKEN`. Never write it to a file, a URL or a log.
- Never upload raw Domoticz responses: they contain home coordinates and internal URLs. Fixtures in `tests/fixtures/real_*.json` are scrubbed; scrub new ones the same way.
