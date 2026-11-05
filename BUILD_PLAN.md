# dzPanel Build Plan

How we get from an empty folder to a flashable, beautiful firmware. Pairs with [`DESIGN.md`](DESIGN.md), which says *what* and *why*. This file says *in what order* and *how we know it worked*.

> Commands here are sketches. Anything touching ESP-IDF flags, ESP Web Tools or Waveshare packages is **unverified** until run. Section 8 tracks that.

---

## 0. Shape of the plan

Three tracks run in parallel. Tracks A and B need no hardware, so they start today.

```
Week   1    2    3    4    5    6    7    8 ...
A  Host:   ████████████████████                     simulator, UI toolkit, geometry, fixtures
B  Pipe:   ██████████████                           repo, CI, installer page, release flow
C  Board:            ░░░░░░░░ ████████████████      bring-up when hardware arrives
           └─ prep only ┘
```

| Track | Goal | Needs hardware |
|---|---|---|
| **A. Host** | Simulator, geometry, round UI toolkit, mock data, Domoticz parser | No |
| **B. Pipeline** | Repo, CI, release artifacts, web installer, docs | No (a board is needed only for the final flash test) |
| **C. Board** | Display, touch, Wi-Fi, power on real boards | **Yes** |

Milestone numbers match the roadmap in `DESIGN.md` section 13.

---

## 1. Stage 0: repository and decisions (days 1–3)

**Do**

- [ ] Create the repo. (The original brief now lives at `docs/PROJECT_DESIGN.md`.) Keep `DESIGN.md` and `BUILD_PLAN.md` at the root.
- [ ] Move `docs/research/*.md` under `docs/research/` as the permanent record (already there).
- [ ] Add `README.md`, `CONTRIBUTING.md`, `.gitignore` (include `sdkconfig`, `build/`, `secrets*`, `*.local.*`), issue templates.
- [ ] **Pick a licence** (ADR-007). Check the licences of any Waveshare code before copying anything.
- [ ] Add `docs/decisions/` with ADR-001 to ADR-007 as short files (title, status, context, decision, consequences). Mark 006 and 007 as open.
- [ ] Commit a `config.example.json` and make secrets git-ignored from the first commit.

**Exit:** repo exists, ADR stubs exist, nothing secret can be committed by accident.

---

## 2. Stage 1: read the vendor path (days 2–5, parallel with Stage 0)

Goal: replace every *unverified* hardware claim with a checked one, **before** it shapes code.

- [ ] Clone both Waveshare repos. Read `HARDWARE_REFERENCE.md` for the 1.75" and the equivalent docs for the 2.16". Check the schematics.
- [ ] Fill in `docs/hardware/amoled_175.md` and `amoled_216.md` with checked pins, I²C addresses and reset wiring. Where sources conflict, record both and the one you trust.
- [ ] Read each BSP's `idf_component.yml` to get exact LVGL, `esp_lvgl_port` and driver versions. Pin those.
- [ ] Check licences of examples and BSP components.
- [ ] Read the ESP Web Tools and Improv docs, and the ESP-IDF OTA docs. Resolve every `[VERIFY]` in `docs/research/flashing.md`.

**Exit:** the verification ledger in `DESIGN.md` section 14 shrinks, and each remaining item is a hardware-only check.

---

## 3. Stage 2: toolchain (days 3–7)

- [ ] Install ESP-IDF 5.5.x (latest patch the BSPs build on). Record the exact version and setup commands in `docs/development-setup.md`.
- [ ] Build the **unmodified** official `02_lvgl_demo_v9` example for each board target. Do not change anything yet. This proves the toolchain.
- [ ] Capture compiler versions, build times and binary sizes.
- [ ] Decide the container image for CI (pinned `espressif/idf` tag, and digest if possible).

```bash
# expected shape; verify against the vendor docs
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
```

**Exit:** both vendor examples compile from a clean clone using documented steps. (Flashing them waits for hardware.)

---

## 4. Track A: host-side work (starts week 1)

Build the part of dzPanel that does not care about the board. Order matters: each step unblocks the next.

| # | Task | Output | Test |
|---|---|---|---|
| A1 | `ru_core`: error types, log macros, OS abstraction (`ru_osal`: queue, mutex, task, timer) with a pthread port | Compiles on host | Unit tests |
| A2 | `ru_ui/geom`: unit `u`, safe radius, chord width, polar placement, inscribed rect | Pure C | **Golden values at 466 and 480**; property test: no box exceeds `Rs` |
| A3 | `sim/`: CMake project, LVGL 9 + SDL2, `--size 466\|480`, circular mask overlay | A window that shows a round screen | Visual check |
| A4 | Theme tokens (Void + Ember), baked fonts (Inter, Space Grotesk subsets), first icon set | `ru_ui/theme`, `tools/fonts`, `tools/icons` | Screenshot |
| A5 | Widgets: `value_tile` (OK/STALE/UNKNOWN), ring gauge, status rim, bead | `ru_ui/widgets` | Screenshot at both sizes |
| A6 | Event bus (coalescing, drop counters) and `ru_ui_post` | `ru_core/bus` | Unit tests, TSAN |
| A7 | `ru_data`: `ru_val_t`, units, formatting, model store | Pure C | Unit tests: unknown never prints as 0 |
| A8 | Runtime: registry, lifecycle, per-app screens, low-memory levels | `ru_runtime` | 1000 switch loop with a heap cap, no leak |
| A9 | Home, Clock and Settings apps with fake data | `apps/*` | Golden images at both sizes |
| A10 | Domoticz parser with **sanitised real fixtures**, plus fuzz target | `int_domoticz/parse` | Unit tests, libFuzzer |
| A11 | Mock Domoticz server for the simulator with latency and error injection | `sim/mock` | Stale and error states demo-able |
| A12 | Domoticz and Energy apps against the mock | `apps/*` | Golden images: loading, stale, error, empty |
| A13 | Gesture recogniser on recorded and synthetic traces | `ru_shell/gesture` | Unit tests (annulus and sector rules) |

**Gate for hardware (Track C):** A1 to A9 done, so bring-up lands on a runtime that already works.

---

## 5. Track B: pipeline (starts week 1)

| # | Task | Notes |
|---|---|---|
| B1 | CI job 1: host build, unit tests, ASAN and UBSAN | Runs on every PR |
| B2 | CI job 2: simulator screenshots at 466 and 480, circle-bounds and lit-pixel checks | Uploads PNGs as artifacts |
| B3 | CI job 3: lint (`clang-format`, `clang-tidy`, `cppcheck`) | |
| B4 | CI job 4: ESP-IDF build matrix, boards × IDF versions, with a size report | Start with 5.5.x and 175 only. Add 216 and 6.x as profiles land. |
| B5 | `tools/make_manifest.py`, `assemble_pages.sh` | Per-board ESP Web Tools manifest, `versions.json`, OTA JSON |
| B6 | Release workflow: tag `v*` produces stable, nightly runs on a schedule | One merged `.bin` and one app-only `.bin` per board, plus SHA-256 |
| B7 | `site/`: the installer page | Board picker, version picker, install button, Improv step, console tab. Vendor ESP Web Tools at a pinned version. |
| B8 | Deploy `site/` and binaries to GitHub Pages (same-origin) | Keep the last N versions, prune the rest |
| B9 | `docs/development-setup.md`, `docs/flashing.md`, troubleshooting (hold BOOT, drivers, `udev`) | |
| B10 | Component lockfile (`dependencies.lock`) committed | Reproducible builds |

**Installer page must handle:** no `navigator.serial` (show a notice and the `esptool` command), per-board BOOT-button instructions, and the post-flash port re-enumeration (re-prompt for the port).

**Exit for Track B:** pushing a tag produces downloadable per-board binaries and a working installer page. Until a real board exists, test the page with a dry run against a stub firmware.

---

## 6. Track C: hardware bring-up (when boards arrive)

Do these in order. Never skip a step because a later step "looks done".

### C0. First look (day 1)

- [ ] Photograph both boards, record markings, revision and variant.
- [ ] Plug in, record how USB enumerates. Try the documented download-mode procedure for each board.
- [ ] `esptool flash_id` for flash size. Boot log for PSRAM size and mode.
- [ ] I²C scan on both boards, record every address. Settles the expander question.
- [ ] Flash the **stock vendor demo** to confirm the hardware works before blaming our code.
- [ ] Save a factory-restore procedure (Waveshare provides firmware binaries).

### C1. Display (v0.1)

- [ ] Minimal IDF project, board profile for the 1.75", registry CO5300 driver behind `hal_display`.
- [ ] Solid colours, then a test pattern with a circle at the exact radius. Confirm orientation, offsets and the physical visible area.
- [ ] Measure flush time and frame rate with partial rendering (2 buffers, internal RAM).
- [ ] Print internal and PSRAM free memory.

### C2. Touch (v0.1)

- [ ] Touch driver behind `hal_touch`. Draw a dot under the finger.
- [ ] Verify coordinates at all four edges and the centre. Fix swap and mirror in the profile.
- [ ] Record touch traces to CSV for the gesture tests (Track A13).

### C3. dzPanel on the board (v0.1 to v0.2)

- [ ] Run the A8 runtime and A9 apps on hardware.
- [ ] Compare the golden images with real photos. Calibrate the safe radius.
- [ ] 1000-switch soak with heap logging.

### C4. Second board (v0.2 to v0.3)

- [ ] Add the 2.16" profile. Confirm the display driver is shared. Test whether the CST9217 driver reads the CST9220.
- [ ] Run the same app binary logic on both boards. Differences must stay inside `boards/`.

### C5. Services (v0.3)

- [ ] Wi-Fi manager, reconnect after a router reboot.
- [ ] SNTP plus RTC. Time valid offline.
- [ ] Improv Serial provisioning. Check it coexists with log output on the console.
- [ ] HTTP and TLS. Check internal RAM headroom while LVGL runs.
- [ ] PMIC: battery reading, USB-only operation, power button behaviour.

### C6. Domoticz on hardware (v0.4, the MVP)

- [ ] Connect to a real Domoticz. Confirm endpoints for the installed version, update the parser and fixtures.
- [ ] Failure drills: wrong password, host down, slow responses, router reboot. Each must show a sensible state.
- [ ] 8-hour soak. Record heap over time.

### C7. Power and shell (v0.5)

- [ ] Dim, screen off, light sleep with touch wake. Measure current in each state.
- [ ] Pixel-shift and always-on face on the real panel. Check for tint at low grey levels.

---

## 7. Release milestones

### First flashable alpha (target: end of v0.1)

A boot splash, a touch test and a memory readout, installable from the web page on the 1.75". Proves the whole chain: build, release, install, boot.

### MVP (v0.4)

Boot, launcher, Domoticz screen on both boards, soak-tested, documented.

### First public release (v0.5)

Everything in the Definition of Done below, plus a demo video or GIF, hardware photos, known limitations, and a call for contributors.

### Definition of done for a public release

- [ ] Boots reliably on both boards.
- [ ] Launcher, clock and Domoticz work. Failures and stale data are shown clearly.
- [ ] Same app code on both screens, differences isolated in `boards/`.
- [ ] Flash from the browser works from a clean machine, and the offline `esptool` path is documented.
- [ ] OTA with rollback works, and the partition layout is frozen (ADR-006 closed).
- [ ] Recovery tested: bad OTA, boot loop, hold-button, browser re-flash.
- [ ] No secrets in the repo or in logs.
- [ ] A new developer follows the docs and builds the firmware.
- [ ] Licences audited, and the licence ADR is closed.

---

## 8. Open items that block or shape work

| Item | Blocks | Resolve by |
|---|---|---|
| Licence choice | First commit of third-party-derived code | Stage 0 |
| Which hardware facts are true (see `DESIGN.md` section 14) | Board profiles | Stage 1 and C0 |
| Factory partition or not | Partition CSV, merged-image layout, OTA | Before v0.5 (ADR-006) |
| Empty-`otadata` boot behaviour with no factory slot | Merged image design | First board, C0 |
| LVGL minor and `esp_lvgl_port` version | Everything UI | Stage 1 |
| ESP Web Tools manifest and Improv details | Installer page | Stage 1 |
| Pages CORS headers | Installer page | B7 |
| Signing key custody | v0.8 | Before beta |
| Domoticz API for your version | Domoticz client | Needs a real instance |

---

## 9. Suggested first week

| Day | Do |
|---|---|
| 1 | Stage 0: repo, README, licence decision, secrets hygiene, ADR stubs |
| 2 | Stage 1: clone vendor repos, read hardware references, pin versions |
| 3 | Stage 2: install ESP-IDF, build the vendor example for the 1.75" |
| 4 | A1 and A2: OS abstraction and geometry with golden tests. B1: first CI job. |
| 5 | A3: simulator window with the circle mask at both sizes |
| 6 | A4 and A5: theme, fonts and the first widgets. Screenshot CI (B2). |
| 7 | Review: screenshots at both sizes, trim scope, update the verification ledger |

By the end of the week you should have a CI-tested round UI toolkit rendering at both resolutions in a simulator. That is real progress while the boards are still in the post.

---

## 10. Risks to watch

| Risk | Early signal | Response |
|---|---|---|
| Not enough internal RAM for Wi-Fi, TLS and LVGL together | Heap under 40 KB after Wi-Fi up | Move buffers to PSRAM, do OTA from safe mode, cut buffer lines |
| Partial rendering tears or clips | Visible artefacts during animation | Switch the board profile to a full PSRAM framebuffer |
| 2.16" touch needs its own driver | CST9217 driver reads garbage | Write a thin CST9220 driver behind `hal_touch` |
| Frame rate below target | Under 15 fps on scroll | Trim animation scope, pre-render rings, lower the ceiling |
| ESP-IDF 6.x breaks a dependency | CI matrix red | Stay on 5.5.x, track 6.x in CI only |
| Web Serial flashing flaky on these boards | Port vanishes after reset | Keep the offline `esptool` path first-class, document BOOT procedure with photos |
| Scope creep before the MVP | Work on Energy or MQTT before v0.4 | Return to: boot, render, touch, launcher, one Domoticz screen |
