<div align="center">

# dzPanel

**A calm, glanceable application platform for round AMOLED screens.**

*One codebase. Many circles. Pure black, light as information.*

`ESP32-S3` · `ESP-IDF` · `LVGL 9` · `Waveshare AMOLED 1.75" / 2.16"` · `Open source`

</div>

---

> **Status:** Draft v0.1, pre-hardware. Written 2026-10-31.
> **Owner:** Mark Heinis (`@galadril`). **Name:** provisional.
> **Companion docs:** [`BUILD_PLAN.md`](BUILD_PLAN.md) (how we get there), [`docs/PROJECT_DESIGN.md`](docs/PROJECT_DESIGN.md) (original brief), and the four research notes in [`docs/research/`](docs/research/).
>
> **Read this first:** the research agents had no working web access for most lookups. Hardware and tooling facts come from search summaries or memory, not from reading primary sources. Section 14 lists every claim that still needs checking. Nothing marked *unverified* should be treated as fact.

---

## Contents

1. [Vision](#1-vision)
2. [Principles](#2-principles)
3. [Hardware targets](#3-hardware-targets)
4. [System architecture](#4-system-architecture)
5. [The app model](#5-the-app-model)
6. [The shell](#6-the-shell)
7. [Data and integrations](#7-data-and-integrations)
8. [Halo, the visual language](#8-halo-the-visual-language)
9. [Memory and rendering](#9-memory-and-rendering)
10. [Install, update, recover](#10-install-update-recover)
11. [Security and privacy](#11-security-and-privacy)
12. [Testing without hardware](#12-testing-without-hardware)
13. [Roadmap](#13-roadmap)
14. [Decisions, conflicts and open questions](#14-decisions-conflicts-and-open-questions)

---

## 1. Vision

Round-screen ESP32-S3 devices usually ship as demos. dzPanel makes them feel like small products: a consistent look, a consistent way to move around, and a simple way to add apps.

The first showpiece is a **smart-home companion** that shows live Domoticz data, energy flow and notifications, and offers safe touch controls. The platform underneath is not tied to Domoticz or to one screen size.

```
        .-----------------------.
      /      ◜  10:42  ◝   Wi     \
     |       Thu 31 Oct            |
     |                             |
     |     [18°]  [2.1 kW]  [78%]  |        A glance, not a dashboard.
     |                             |        One hero. Everything else is quiet.
      \          ● ○ ○             /
        '-------▁▁▁▁-------------'
```

**Success looks like:** a polished, reproducible foundation that a stranger can flash from a browser in under two minutes, and a contributor can extend with a new app in under an hour.

---

## 2. Principles

| Principle | What it means in practice |
|---|---|
| **Round first** | Layouts use circular safe areas and chord widths. A square UI clipped to a circle is a bug. |
| **One platform, many devices** | Apps never contain pixel constants. Board differences live in board profiles. |
| **Void first** | True-black backgrounds. Depth comes from thin light, not grey slabs. |
| **Honest data** | *Unknown*, *stale* and *zero* look different, by construction (section 7). |
| **Safe by default** | Risky actions need a deliberate hold. Stale or just-woken UI cannot trigger anything. |
| **Degrade, don't crash** | A missing peripheral lowers a capability flag. It never aborts boot. |
| **Small steps** | Boot, render, touch, launcher, one Domoticz screen. Then everything else. |
| **Open and reproducible** | Pinned toolchain, committed lockfile, documented builds, no secrets in Git. |
| **Build on the vendor path** | ESP-IDF stays. We replace nothing underneath us. Vendor examples are the baseline. |

**Terminology.** dzPanel is an application platform and firmware built on ESP-IDF and FreeRTOS. It is not a new kernel.

---

## 3. Hardware targets

Both boards share most of their silicon. That is the main reason one codebase is realistic.

| | **AMOLED 1.75"** | **AMOLED 2.16"** |
|---|---|---|
| Resolution | 466 × 466, round | 480 × 480 (physical shape *unverified*) |
| SoC | ESP32-S3, 16 MB flash, 8 MB PSRAM | ESP32-S3R8, 8 MB PSRAM, flash 16 MB *(conflict: one source says 8 MB)* |
| Display | CO5300 over QSPI | CO5300 over QSPI |
| Touch | CST9217, I²C `0x5A` | CST9220, I²C `0x5A` |
| IO expander | TCA9554 | none, per one third-party source *(unverified)* |
| PMIC / RTC / IMU | AXP2101 / PCF85063 / QMI8658 | same |
| Audio | ES8311 codec + ES7210 mic ADC | same |
| Extras | microSD, native USB-C, BOOT and PWR buttons | microSD, native USB-C, BOOT, KEY3, PWR |
| Shared I²C bus | SDA GPIO15, SCL GPIO14 | same |

Sources and the full pin notes are in [`docs/research/hardware.md`](docs/research/hardware.md). That file also lists the pin conflicts between sources. **Do not hard-code pins from it until they are checked against Waveshare's `HARDWARE_REFERENCE.md` and the schematic.**

### Board profiles

Each board is one profile in `boards/<id>/`: resolution, safe-area inset, display and touch drivers, reset method (GPIO or expander), pin map, flash size, and capability flags. The rest of the OS asks "does this board have an IMU?", never "is this the 1.75?".

---

## 4. System architecture

```
┌────────────────────────────────────────────────────────────────┐
│  Apps        home · clock · domoticz · energy · settings       │
│              later: declarative dashboard apps, then WASM      │
├────────────────────────────────────────────────────────────────┤
│  Shell       launcher · gestures · status rim · notifications  │
│              quick settings · app switcher · lock / sleep      │
├────────────────────────────────────────────────────────────────┤
│  Runtime     registry · lifecycle · permissions · event bus    │
│              UI-thread owner · memory guard                    │
├──────────────────────────────┬─────────────────────────────────┤
│  UI toolkit (ru_ui)          │  Data layer (ru_data)           │
│  round layout · widgets      │  typed models · freshness       │
│  theme · icons · i18n        │  integrations: domoticz, …      │
├──────────────────────────────┴─────────────────────────────────┤
│  Services    config · secrets · wifi · time · http/TLS · mqtt  │
│              log · ota · power · storage                       │
├────────────────────────────────────────────────────────────────┤
│  HAL         display · touch · power · rtc · imu · audio · sd  │
│              board profile + capability flags                  │
├────────────────────────────────────────────────────────────────┤
│  ESP-IDF 5.5.x · FreeRTOS · esp_lcd · LVGL 9 (pinned minor)    │
└────────────────────────────────────────────────────────────────┘
Host build: the same upper layers compile on Linux with SDL2 and mocks.
```

### Technology baseline

| Choice | Decision | Why |
|---|---|---|
| Framework | **ESP-IDF 5.5.x**, built with `idf.py` | Both boards ship official IDF examples and registry BSPs. Arduino examples for the 2.16 pin LVGL 8.4, which splits the ecosystem. |
| ESP-IDF 6.x | CI-only from v0.3, default switch around v0.5 | 6.0 changes mbedTLS and libc. Third-party components may break. |
| GUI | **LVGL 9.x**, one pinned minor | Mature SDL2 simulator, arc and scale widgets, and `esp_lvgl_port`. Exact minor is *unverified*, so pin whatever the Waveshare `02_lvgl_demo_v9` example uses. |
| Language | C core, C++ allowed behind `extern "C"` | A plain-C ABI keeps future dynamic apps possible. |
| Drivers | Registry `espressif/esp_lcd_co5300` and Waveshare's CST9217 touch component, wrapped behind our HAL | Fastest to bring-up. We can replace them later without touching apps. |
| Vendor BSPs | Reference only | They bring their own LVGL init and pin macros that would leak through the HAL. |

Record each choice as an ADR (list in section 14).

### Repository layout

```
dzpanel/
├── main/                 tiny app_main: pick board, start runtime
├── boards/               board_common, amoled_175, amoled_216, sim_466, sim_480
├── components/
│   ├── hal/  hal_drivers/
│   ├── ru_core/  ru_runtime/  ru_ui/  ru_shell/  ru_data/
│   ├── svc_config/ svc_wifi/ svc_time/ svc_http/ svc_mqtt/
│   │   svc_log/ svc_ota/ svc_power/ svc_storage/
│   └── int_domoticz/  (later: int_mqtt, int_ha)
├── apps/                 app_home, app_clock, app_domoticz, app_energy, app_settings
├── sim/                  SDL2 host build, mock services, fixture loader
├── tests/                host unit tests and sanitised fixtures
├── tools/                font + icon pipeline, manifest builder, fixture sanitiser
├── site/                 the web installer (GitHub Pages)
├── docs/                 decisions/ (ADRs), hardware/, research/
└── .github/workflows/
```

Rules: components are prefixed `ru_`, `svc_`, `hal_`. Upward calls go through events only. Apps depend on `ru_runtime`, `ru_ui` and `ru_data` headers, never on `hal_*` or raw ESP-IDF headers. CI checks this.

### Hardware abstraction

Every optional device is a nullable interface. A single capability bitmask is the source of truth:

```c
typedef enum {
  RU_CAP_TOUCH, RU_CAP_BATTERY, RU_CAP_RTC, RU_CAP_IMU, RU_CAP_AUDIO_OUT,
  RU_CAP_MIC, RU_CAP_SDCARD, RU_CAP_PSRAM, RU_CAP_BUTTONS, RU_CAP_BRIGHTNESS, ...
} ru_cap_t;

if (ru_has(RU_CAP_IMU)) { /* wrist-raise wake */ }
```

A device that fails to initialise clears its flag and the boot continues. One task owns the shared I²C bus (touch, PMIC, RTC, IMU, codec, expander).

---

## 5. The app model

Apps are compiled in for now. They get a small explicit lifecycle and a filtered view of the system.

```
register → init → enter ⇄ leave → release
                     │
                     └─ on_low_memory(level)   (optional)
```

```c
typedef struct {
  const char *id;              // "org.dzpanel.domoticz"
  const char *name_key;        // i18n key
  const ru_icon_t *icon;
  uint32_t needs_caps;         // hide the app if missing
  uint32_t wants_caps;         // app copes if missing
  uint32_t perms;              // RU_PERM_NET_HTTP, RU_PERM_NOTIFY_POST, ...
  uint32_t ram_budget_int, ram_budget_psram;
} ru_app_manifest_t;
```

Key rules:

- **One screen per app.** The runtime creates it on `enter` and deletes it after `release`. Leaving an app frees its RAM after a short grace period.
- **Per-app settings** live in an NVS namespace derived from the app id. Secrets are owned by services and are never visible to apps.
- **Permissions** filter the service table an app receives. For compiled-in apps this is API hygiene and future-proofing, **not a security boundary**, and the docs say so.
- **One UI thread.** Only the `lvgl` task touches LVGL objects. Everything else publishes events or calls `ru_ui_post()`. Handlers never block (no HTTP, no flash writes, no SD I/O).
- **Registration** is a generated `apps_table.c` from a list, which also gives a per-board enable list and works identically on host.

### Event bus

Typed topics in ranges per owner (system, network, time, power, input, notify, models). Small payloads are copied. Large payloads are refcounted immutable blobs. Queues are bounded and noisy topics are coalesced, so a stalled UI drops old updates, not new ones.

---

## 6. The shell

The shell lives on LVGL's top layer, above any app, so apps never see it.

### Navigation map

```
                 [ Quick settings ]        swipe down from top edge
                         ▲
 [Favourites] ◂▸ [Lights] ◂▸ [Scenes] ◂▸ [Temperature] ◂▸ [Weather] ◂▸ [Utility] ◂▸ [Energy] ◂▸ [Settings]
                                                   swipe left / right (no wrap); empty pages hidden
                  swipe up / down: the pages of a list ("2/4"), 3x2 tiles a page (Lights, Scenes, Temperature, Weather, Favourites) or 4 rows (Utility)
                         ▼
                   [ Launcher ]            swipe up from bottom edge

 Content swipes (left/right/up/down) are decided in ONE place, ru_dzapp_swipe; the rim recogniser keeps back and home.
 Back:  swipe right from the left edge, or the visible back pill "< BACK" ("< DEVICE NAME" on dial pages, "< CANCEL" on confirm);
        square panels add a corner chevron at the top left (56 px, outside the circle)
 Home:  long-press the rim, or tap the home pill at 6 o'clock
 Rim:   dragging along the outer ring acts as a virtual crown
```

Edge gestures start in an **annulus** (outer ~8–10% of the radius), not a square edge, and are resolved by sector and direction. Apps never receive touches that begin in the annulus.

### Shell parts

| Part | Notes |
|---|---|
| **Home** | A watch-face with one hero (time) and up to three complications on the ring. Four faces planned: Classic, Orbit, Data, Contour (always-on). |
| **Launcher** | Circular carousel for up to 7 apps, honeycomb beyond that. |
| **Status rim** | A hairline ring carrying Wi-Fi, battery and notification state as arc segments. State is never colour alone. |
| **Notifications** | A card stack. Banner for normal priority, full-screen takeover only for critical. |
| **Quick settings** | Brightness arc on the rim, Wi-Fi, do-not-disturb, always-on, night mode. |
| **Power** | Active, dim, screen off, then light sleep. Touch, button or IMU wakes. The first touch after wake only wakes. |
| **Lock** | Optional PIN guarding controls and settings, not the display. |

> **Reconciliation note.** The architecture note placed notifications on swipe-down. The UX note places quick settings on swipe-down and notifications on a second swipe-up or a tap on the rim dot. **This document follows the UX note.**

---

## 7. Data and integrations

### Honest values

Every number a screen shows is a freshness-tagged value:

```c
typedef struct {
  float v; ru_unit_t unit;
  bool  valid;           // false = unknown, never rendered as 0
  uint32_t ts_mono_ms;   // monotonic, immune to clock jumps
  uint32_t ttl_ms;       // freshness window
} ru_val_t;
```

One shared `value_tile` widget renders **OK**, **STALE** (dimmed, dashed ring, "12 min ago") and **UNKNOWN** (`--` with a `?` glyph) the same way in every app. Apps cannot forget the empty or stale case because they do not draw it.

### Integrations

An integration is a plugin with a small interface:

```c
configure · start · stop · list_entities · subscribe · command · conn_state
```

It writes typed models into a shared store. Apps and the shell read models and never see raw API data. **Domoticz is the first integration.** A second (Home Assistant or MQTT) is scheduled at v0.7 to prove the abstraction.

### Domoticz client

- Read-only first. A user-chosen **watch list** of entities, with no hard-coded device indexes.
- Polling intervals depend on what is visible (about 10 s foreground, 30 s glance, 120 s dimmed, suspended with screen off), with exponential backoff.
- `401` or `403` stops polling and raises an auth-failed state. No retry storms.
- Energy values use unit-aware parsing of strings like `"1234 Watt"`, never `atof`. A missing role is *unknown*, never zero.
- Parser tested on the host with sanitised fixtures and fuzzed.
- The exact API endpoints vary by Domoticz version. **They must be checked against a real instance.**

### Control safety

Commands carry a risk level: `SAFE`, `CONFIRM` or `LOCKED`. Lights are safe by default. Locks, doors, heating and scenes need **press-and-hold** (about 1.2 s, with a filling ring). Alarms need a PIN. A command is refused when the model is not fresh, when the screen woke less than 500 ms ago, or when the display is dimmed.

Commands go through the Domoticz MCP endpoint, which **skips Domoticz's own Protected (passcode) check and per-user device check**. dzPanel's refusal is therefore the only guard: a protected device is never controlled, and a pre-flight re-read right before each command refuses one that became protected since the last poll. Controls with no safe MCP path (thermostat mode, setpoints on a Fahrenheit server) are shown read-only.

---

## 8. Halo, the visual language

A round AMOLED in a dark room is a ring of light floating in black. Halo leans into that.

1. **Void first.** Background is `#000000` everywhere. Raised surfaces start at `#0E1013` and stay small.
2. **Rings, not boxes.** The arc is the main idiom. Rectangles only appear inside the chord-safe middle band.
3. **One hero per screen.** One number, one ring, one verb.
4. **Light is information.** Brightness encodes priority. Colour is a second channel, never the only one.
5. **Calm motion.** Few, short, time-based animations that respect QSPI bandwidth.
6. **Honest data.** Stale, unknown and zero look different.
7. **Safe by default.** Danger needs a visible hold.

**Signature elements:** the **rim** (a hairline status ring), the **bead** (a small glowing dot that rides the ring for time, progress and loading) and **stepped-glow arcs** (pre-rendered, no runtime blur).

### Tokens (excerpt)

| | |
|---|---|
| **Accent palettes** | Ember `#FF8A3D` (default, warm, low blue so low power), Aurora `#3DE0C5`, Iris `#8C7CFF`, Rose `#FF5C8A`, Lime `#B6F25A`, Mono `#E6E9EE` |
| **Semantic** | ok `#3DDC84`, warn `#FFC247`, error `#FF5A5F`, info `#4DB3FF`, stale `#8A93A0`. Each pairs with a glyph or shape. |
| **Fonts** | Inter (UI), Space Grotesk or Manrope (hero numerals), JetBrains Mono (diagnostics). All SIL OFL. Baked with `lv_font_conv` at 4 bpp and subset. |
| **Unit** | `u = D / 100` (4.66 px at 466, 4.80 px at 480). All sizes, spacing and strokes are in units. |
| **Safe radius** | `Rs = 0.92 R`. The largest fully safe square is about 65% of the diameter. |
| **Chord width** | `w(d) = 2·√(Rs² − d²)` at distance `d` from centre. Rows use their worst edge. |
| **Touch target** | at least 11u (about 51 px), preferably 14u |

### AMOLED rules

- Average lit pixels: at most **12%** on active screens and **3%** in always-on mode.
- Burn-in mitigation: slow pixel-shift orbit (±2 px), no static bright element beyond 5 minutes, hairlines at or below 30% luminance, hue rotation of always-on elements by the hour.
- No light theme by default. Avoid large blue or white fields. Use stepped glows, not dark-grey gradients.

### Screens designed so far

Boot splash, Wi-Fi setup (phone provisioning first), four home faces, Domoticz overview and detail, **Energy ring** (solar, grid and battery segments around a home-load hero), notification stack, settings and first-run. Wireframes, state patterns (loading, stale, error, offline, empty) and the design-review checklist are in [`docs/research/ux.md`](docs/research/ux.md) (section 10.4a is the Domoticz page model as built). The contrast ratios, chord widths and throughput figures there have **not been checked with tools or on hardware**.

The Temperature and Weather pages show NSPanel-style tiles: device name on top, a Lucide icon coloured by a stepped band (the value text stays white), the big value with unit and trend, one small line. Each device is judged by its own readings only; no outdoor-device concept, no forecast service. Bands, icon rules and the icon font are in [`docs/research/ux.md`](docs/research/ux.md) section 11 (licences in `docs/third-party/`).

---

## 9. Memory and rendering

All figures below are **assumptions to measure on the first board**.

- **Framebuffer cost at RGB565:** 434 KB at 466², 461 KB at 480².
- **Default strategy:** LVGL partial rendering with two draw buffers (about 40 lines each, roughly 75 KB total) in internal DMA RAM. The panel's own GRAM holds the picture.
- **Fallback:** full-screen buffer in PSRAM if partial rendering tears or clips. The choice is a board-profile field, so switching is cheap.
- **LVGL heap** is backed by `heap_caps` in PSRAM. Fonts and icons stay in flash.
- **Low-memory levels:** NORMAL, LOW (flush caches, release suspended apps), CRITICAL (close the foreground app and return home), FATAL (controlled restart, not a panic).
- **Frame budget:** design for a 30 fps ceiling and tolerate 15. Animate small regions and translations, not full-screen blends.

---

## 10. Install, update, recover

Details, a sample manifest, a partition CSV and a CI workflow sketch are in [`docs/research/flashing.md`](docs/research/flashing.md).

### Installing: the web flasher

A static **dzPanel Installer** page on GitHub Pages, built on **ESP Web Tools**:

```
 1  Pick your board          1.75"  /  2.16"          (chip ID can't tell them apart)
 2  Pick a version           stable  /  nightly       [ ] clean install
 3  Connect and install      one click, progress, verify
 4  Wi-Fi                    Improv Serial over USB, right after flashing
 5  Console                  live log, copy, save, reset, enter bootloader
```

- **One merged binary per board**, written at offset `0`. Updates over the air use a separate app-only binary.
- Chrome or Edge on desktop only (Web Serial). Other browsers see a clear notice plus a copy-paste `esptool` command.
- Binaries, manifests and OTA metadata are **copied to GitHub Pages** so the browser fetches same-origin and avoids CORS and redirect problems.
- No analytics and no server. Everything runs in the browser.

### Wi-Fi setup

1. **Improv Serial** over the USB console, straight after flashing.
2. **SoftAP captive portal** with a QR code on screen as the fallback (no credentials, repeated join failures, or a long-press gesture).
3. Domoticz URL and credentials are entered on the **device's own web page** over the LAN, never typed on the round screen and never stored by the installer.
4. BLE provisioning only if people ask for it.

### Updating

- HTTPS pull via `esp_https_ota` with the CA bundle and **app rollback**. A new image is marked valid only after health checks pass (display up, UI loop running, 30 s stable).
- The firmware refuses an image whose `project_name` does not match the running board.
- The user confirms the update. Never on low battery.
- **Signing:** none in the MVP (be honest in the docs: the hash protects against corruption, not a hostile host). Signed OTA at beta. Secure Boot v2 is an opt-in for people who sign their own builds, because the eFuse burns are irreversible and break open re-flashing.

### Recovery, in layers

1. Automatic rollback after a bad OTA.
2. Boot-loop detection: three failed boots lead to safe mode (display, Wi-Fi, OTA only).
3. Hold a button at power-up: about 5 s for safe mode, about 15 s for factory reset with on-screen confirmation.
4. Re-flash from the browser through the ROM download mode.
5. Documented offline `esptool` command.

**Hard rules:** keep the USB-Serial/JTAG console enabled, do not use TinyUSB on that port, and never burn the USB-JTAG-disable eFuses. Together they keep layer 4 always available.

### Partition layout (16 MB assumption)

Two 4 MB OTA slots, NVS, `otadata`, coredump and a LittleFS `storage` partition of about 6 MB. **Recommendation: no factory slot.** This resolves the research conflict as follows. With no factory app and an empty `otadata`, the bootloader should start `ota_0`, so the merged image only needs the app at the `ota_0` offset. Safe mode becomes a flag inside the main app, and browser re-flash is the last-resort exit. The research note proposed a separate 2 MB factory app instead. **This is the one decision that is destructive to change later, so it is gated before the first public release (ADR-006).** The boot behaviour with an empty `otadata` must be confirmed on a real board.

---

## 11. Security and privacy

- Wi-Fi credentials, tokens and private hostnames never enter Git. A safe example config ships instead.
- Secrets live in a separate NVS namespace, are used inside service callbacks only, and are masked in the UI and in exports. Logs redact URLs and never print secrets.
- HTTPS with the CA bundle by default. LAN installs often use plain HTTP or self-signed certificates, so each integration has an explicit, visible `insecure_lan` option and a warning icon.
- No always-on web server. The setup portal has a random password, shows it as a QR, and times out.
- All network input is length-capped and depth-limited. Parsers are fuzzed on the host.
- No cloud dependency and no telemetry. `docs/privacy.md` will state what stays on the device and what crosses the network.
- Until flash encryption is enabled, document plainly that **physical access means secret access**.

---

## 12. Testing without hardware

Hardware may be weeks away. Most of the OS can be built and tested before it arrives.

- **Simulator.** A plain-CMake SDL2 build of the same upper layers at 466 and 480, with a circular mask overlay so clipping is visible. The HAL is replaced by simulated devices, and services by mocks (Wi-Fi toggles, time warp, fixture-replay Domoticz server with latency and error injection). A tiny OS abstraction layer over pthreads keeps the one-UI-thread rule honest on the host.
- **Unit tests** (Unity under CTest): geometry and safe-area, freshness and formatting, event bus, settings, Domoticz parsers, Wi-Fi backoff, idle-power state machine, gesture recogniser on recorded touch traces, memory-level state machine. Run with ASAN and UBSAN. Fuzz the parsers.
- **Golden images.** Each screen is rendered at both sizes with fake data. CI asserts no lit pixel outside the circle, no essential pixel outside the safe radius, and an average-lit-pixel ceiling.
- **Build matrix.** Both boards × ESP-IDF 5.5 and 6.x, with a firmware size report that fails on unexpected growth.
- **Later:** a self-hosted runner with a real board for serial smoke tests.

A simulator proves layout and logic. It does **not** prove the display driver, touch controller or frame rate.

---

## 13. Roadmap

| Version | Name | Delivers | Exit criterion |
|---|---|---|---|
| **0.1** | Hello round | IDF project, 1.75 board profile, display and touch HAL, LVGL, touch test screen, memory stats, simulator skeleton, CI | Boots reliably, touch correct at the edges, both builds green |
| **0.2** | Runtime | Registry, lifecycle, event bus, settings, Home and Clock, theme, 480 profile, 2.16 profile compiles | 1000 app switches with no leak |
| **0.3** | Connected | Wi-Fi, provisioning, SNTP and RTC, config and secrets, HTTP/TLS, freshness values, 2.16 bring-up | Survives a router reboot, time valid offline |
| **0.4** | **Domoticz PoC (the MVP)** | Integration interface, model store, Domoticz read-only client, parser tests, Domoticz app, stale and error states | Boot, launcher, Domoticz on both boards, 8 h soak |
| **0.5** | Shell | Carousel launcher, gestures, quick settings, notifications, power states, low-memory policy, i18n (en, nl) | Gesture tests pass, idle current measured, **first public release** |
| **0.6** | Control and energy | Confirmed switch control, favourites, Energy app, MQTT, battery, IMU, audio and SD where present | No unconfirmed risky action |
| **0.7** | Integrations | Second integration, declarative dashboard apps, app switcher, API-level draft | Second integration added without touching apps |
| **0.8** | Update and recover | OTA with rollback, signed images, recovery docs, backup and restore, factory reset | Power pulled mid-OTA recovers |
| **0.9** | Hardening | Fuzzing, 7-day soak, power tuning, optional always-on face, PIN, hardware-in-loop smoke test | No leaks, budgets documented |
| **1.0** | Stable | Stable API level 1, documented boards, app template, security docs | A contributor ships an app from the template in under an hour |
| **post-1.0** | Extensible | WASM (WAMR) apps with enforced permissions, signed app packages, more boards, RTL and CJK, BLE provisioning | |

**Dynamic apps.** None before 0.8. First comes declarative JSON dashboards (cheap, safe, covers most "widgets and favourites" demand). WASM comes after 1.0. ELF loading is a non-goal because it has no sandbox and breaks on every SDK update.

---

## 14. Decisions, conflicts and open questions

### ADRs to write

| ADR | Topic | Status |
|---|---|---|
| 001 | Framework and build system | Proposed: ESP-IDF 5.5.x, `idf.py` |
| 002 | Graphics library and rendering strategy | Proposed: LVGL 9, partial rendering |
| 003 | App registration and lifecycle | Proposed: generated table, section 5 lifecycle |
| 004 | Configuration and secrets storage | Proposed: NVS, separate secret namespace |
| 005 | Integrations and Domoticz API | Proposed: section 7, endpoints to verify |
| 006 | Partition layout, OTA, signing, recovery | **Open. Must close before the first public release** |
| 007 | Licence | **Open.** Research suggests Apache-2.0 or MIT. Needs your call. |

### Where the research notes disagreed, and what this document chose

| Topic | Conflict | Choice |
|---|---|---|
| Factory partition | Flashing note: keep 2 MB factory. Architecture note: two OTA slots only. | No factory. Safe mode in the main app. Gated by ADR-006. |
| ESP-IDF version | Flashing CI sketch used 5.4.1. Others say 5.5.x. | 5.5.x. The sketch needs updating. |
| Drivers | Architecture: write our own. Hardware: use registry drivers. | Use registry drivers behind the HAL for bring-up. Replace only if needed. |
| Notification gesture | Architecture: swipe down. UX: swipe down is quick settings. | UX map wins. |
| Safe inset | Architecture: 6% of radius. UX: 8% (`Rs = 0.92R`). | 0.92R, tuned per board once measured. |
| Provisioning | Flashing: Improv Serial. UX: phone and QR. Architecture: SoftAP. | Improv Serial first, SoftAP and QR as fallback. They agree. |

### Verification ledger: do this before trusting the docs

These came from search summaries or memory. They are not confirmed.

**Hardware**
- 2.16" flash size (16 MB vs 8 MB), PSRAM mode on both boards, and whether the 2.16" panel is physically round.
- Real LCD-reset and touch-reset wiring on the 1.75" (GPIO39/40 vs expander vs GPIO1/2). GPIO1 and GPIO2 are SD pins there, so the latter look wrong.
- Whether the 2.16" has an expander, its IMU interrupt pin (GPIO17 vs GPIO13), and its speaker and PA wiring.
- Whether the CST9217 driver reads the 2.16"'s CST9220.
- Whether USB enumerates as USB-Serial/JTAG and auto-reset works without BOOT.
- Licences of Waveshare example code before anything is copied.

**Software and tooling**
- LVGL minor version, and `esp_lvgl_port` compatibility with IDF 5.5 and 6.x.
- ESP Web Tools manifest schema, Improv flag placement, erase options, and Android support.
- GitHub Pages and Release CORS headers. The plan sidesteps them by hosting on Pages.
- Improv Serial coexisting with log output on the USB-Serial/JTAG console.
- Boot behaviour with an empty `otadata` and no factory partition.
- Internal-RAM headroom for TLS OTA while LVGL is running.
- Domoticz endpoints for your installed version.

**Numbers**
- Every memory threshold, QSPI clock, frame rate, power figure, contrast ratio and chord width in the research notes.

### Still to decide

- Signing key custody (GitHub secret or offline).
- Downgrade policy between nightly and stable.
- Whether nightly builds ship at all in the first public release.

---

<div align="center">

*Boot → render → touch → launcher → one Domoticz screen.*
*Everything else earns its place after that.*

</div>
