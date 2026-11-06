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

The board is chosen at build time with `RU_BOARD` (`tools/build_fw.sh <board>` passes `-DRU_BOARD=<board> -B build-<board>`). Each board has `components/boards/<board>/` (the `ru_board` component), `firmware/boards/<board>/sdkconfig.defaults` and its own `firmware/dependencies.lock.<board>`. Use one build directory per board: the sdkconfig lives in the build dir, and CMake refuses to reuse a build dir for a different board. A build dir that has an sdkconfig but no `ru_board.stamp` (configured before stamping) is also refused with a fatal error, because its board is unknown: delete it or reconfigure from scratch. A fresh dir (no sdkconfig) is stamped on first configure. The Guition bring-up probe is built with `tools/build_probe.sh` (see `firmware/bringup/guition-probe/README.md` and `docs/bringup-guition-4848s040.md`).

On a normal machine with a writable ESP-IDF install, none of the shadow directory is needed.

**Flashable images:** `tools/make_image.sh <board>` builds with `tools/build_fw.sh`, merges the build's `flash_args` into one image at offset 0 (`esptool merge_bin --flash_mode dio --flash_size 16MB --flash_freq 80m`) and writes **one** loose file, `dist/dzpanel-<board>-latest.bin` (overwritten each time), plus `dist/dzpanel-<board>-latest.bin.sha256` and a refreshed `dist/SHA256SUMS`. It then deletes older loose builds of that board (`dist/dzpanel-<board>-*-merged.bin`, `dist/roundos-<board>-*-merged.bin`) and nothing else: names containing `factory` and the other recovery or reference images are never touched. `--dry-run` lists what the cleanup would delete (nothing is built or written), `--clean` runs only the cleanup, `--skip-build` merges the existing build. `tools/make_image.sh --probe guition-4848s040` does the same for the bring-up probe (`dist/guition-4848s040-probe-latest.bin`). `dist/` is git-ignored. Boards: `amoled-175`, `guition-4848s040`.

Practical notes:

- Managed components (amoled-175: Waveshare BSP, LVGL 9.4.0, CO5300 display, CST9217 touch, TCA9554 expander; guition-4848s040: esp_lvgl_adapter, esp_lcd_st7701, esp_lcd_panel_io_additions, esp_lcd_touch_gt911; exact versions in the per-board lock files) download from the Espressif registry on the first `set-target`. On a slow filesystem (a Windows mount under WSL) this took over 20 minutes. Run it in the background and let it finish.
- The Guition board uses a CH341 USB-UART bridge on UART0. Opening the port from a Web Serial page toggles DTR/RTS, which auto-resets the board, so the installer page may restart the device when it connects.
- **Guition GPIO0 / ROM download loader:** GPIO0 is both the boot strap and RGB data line R4. The CH341 DTR/RTS auto-reset can leave GPIO0 low after a serial port is opened, and the chip then boots into the ROM download loader. Symptom: black screen and a silent console (the ROM prints `waiting for download`). Workaround: open the port with DTR and RTS deasserted (miniterm `--dtr 0 --rts 0`), then press RST once. The web installer page resets the board through DTR/RTS, so after opening the port from it you may need to press RST.
- ESP log lines share UART0 with the RU+CFG and Improv framing, so a host-side parser must tolerate (skip) log lines between frames.
- Builds are slow: run `idf.py build` in the background and wait on the log.
- The unmodified Waveshare LVGL demo for the 1.75" board builds with this setup (about 1 MB, 12% of an 8 MB slot).

### Heap diagnostics (RU_HEAP_LOG)

`CONFIG_RU_HEAP_LOG` (default n, y in the Guition defaults) enables a log task (4 KB stack, period `CONFIG_RU_HEAP_LOG_PERIOD_S`, default 5 s) and a failed-allocation hook. Only numbers and fixed task names are logged. Line format:

```
I (12345) heap: int free <N> big <N> min <N> | psram free <N> big <N> min <N> | stack-free-min ru_wifi=<n> ru_console=<n> ui_bridge=<n> dz_service=<n> heaplog=<n> alloc-fail-dropped=0
```

- `int` is internal RAM (`MALLOC_CAP_INTERNAL|8BIT`), `psram` is external RAM: `free` now, `big` largest single block, `min` approximate lowest free value since boot (sum of per-heap minima, so not an exact simultaneous low-water mark). The line values above are placeholders (`<N>`/`<n>`), not measurements. `int` uses `INTERNAL|8BIT`, whereas the `ru_hal` lines use `INTERNAL` only, so their numbers differ slightly.
- `stack-free-min` is the least free stack in bytes each task has ever had (high-water mark): a small value means the stack is nearly used up. Only permanent tasks are listed (`ru_wifi`, `ru_console`, `ui_bridge`, `dz_service`; transient ones could exit while being inspected); tasks not yet created are omitted.
- Internal `min` approaching the 32 KB reserve (or 0) is the warning sign for the Wi-Fi `ESP_ERR_NO_MEM` abort.
- A failed allocation logs `E (..) heap: alloc failed: size N caps 0x...`, at most one line per 200 ms; suppressed ones are counted in `alloc-fail-dropped`. The caller function is not printed (its string may be unreadable with the flash cache disabled when rodata is in PSRAM).
- The `ru_hal` boot lines (`boot`, `after hal`, `ui up`, `network up`) also print `min int N psram M` (internal and PSRAM minima).
- Request marks (firmware/main/svc_glue.c wraps the Domoticz transport, only under RU_HEAP_LOG): `I heap: mark <tag> int free N big N min N` before each HTTP request and `... status S err E bytes B ms M` after it. Tags are fixed words (`get devs light begin`, `get devs full end`, `get scenes begin`, `cmd switch end`, `probe begin`, ...); URLs and headers are never logged. `min` is the lowest internal free ever.
- `W heap: low int free N big N min N after mark <tag>`: internal free fell below 24 KB (sampled every 100 ms), at most once per 60 s; the tag is the most recent mark, so it names the request (or the gap after it) in which the dip happened.
- Task stack sizes are Kconfig values (`RU_TASK_STACK_*` in components/ru_core/Kconfig, applied by `ru_task_create` by task name); the Guition defaults are smaller than the 10240 default, see the measurements in that file.
- New `sdkconfig.defaults` values apply only on a FRESH configure: delete `firmware/build-<board>/sdkconfig` (or the whole build dir) to pick up changed defaults. The build-dir stamp already prevents board mix-ups.

## History and implementation notes

- Commit `ac70762` (per-task stacks, one LVGL draw unit, smaller Wi-Fi pools) also contains the page-model restructure by accident (another session's uncommitted edits were swept in): `components/ru_apps/CMakeLists.txt`, `ru_apps.h`, `ru_dzapp.h`/`.c`, `ru_apps_table.c` and `sim/CMakeLists.txt`. History is not rewritten; read that commit as two changes. The old app id `org.dzpanel.devices` is gone, and nothing persists a section number: the only NVS namespace is the secrets/config one, the current section and page live in RAM and start at Home.
- Screen refresh (`ru_dzapp.c`): the UI tick compares a cheap control signature (model generation, units, connection, view, section, page). On the tile grids and Utility rows a data change updates the existing tiles IN PLACE (setters that get identical input invalidate nothing); only a change of structure (which entries are on the page, names, kinds, page count, available sections) rebuilds the screen, and such a background rebuild happens at most once per 2 s (`REBUILD_MIN_MS`). What the user or the connection changes rebuilds at once, and so do the detail, energy and sub screens (no limit there; they hash the whole device entry). A pure reorder on the server bumps the model generation, so the grids follow it.
- Guition panel: the RGB panel is created from a short-lived task pinned to core 1 (`lcdinit`, 6 KB stack, deleted afterwards) so that the VSYNC and GDMA interrupts are allocated on the core without Wi-Fi; it hands the result back by task notification. The panel is fed through a bounce buffer of 20 lines (10 if internal RAM is short), two of them in internal DMA RAM; the framebuffer is in PSRAM. The boot log prints the task's stack high-water mark.

## 5. Flashing

`docs/flashing.md`. **No board has been flashed**: nothing in the firmware has run on hardware.

## 6. Secrets

- Host tools read the Domoticz token from `DOMOTICZ_ACCESSTOKEN`, or from the variable named by `--token-env NAME`. Never write it to a file, a URL, argv or a log.
- `tests/dz_live_check` has no default server: `--url URL` is required. Read-only checks: `--tabs`, `--lights`. Anything that writes (`--mcp`, `--push-test`) needs `--idx N` (the one test device) and `--allow-write`, and prints only fixed words, HTTP statuses and numbers. Never write the URL, idx or device names into the repo.
- Never upload raw Domoticz responses: they contain home coordinates and internal URLs. Fixtures in `tests/fixtures/real_*.json` are scrubbed; scrub new ones the same way.
