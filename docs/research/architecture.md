# dzPanel Software Architecture Proposal

Status: proposal for review. Companion to `docs/PROJECT_DESIGN.md` (not modified).
Conventions: **[A]** = assumption to verify on hardware or docs. **[D]** = decision (recommendation). Code is sketch-level C, not final API.

---

## 1. Baseline and why

| Item | Decision | Notes |
|---|---|---|
| SDK | **ESP-IDF 5.5.x LTS-ish line for v0.x, evaluate 6.x at v0.5** [D] | Web check (Oct 2026): IDF 6.1 is latest feature release (2026-08-27), 6.0 (2026-03) and 5.5.6 (2026-10-16) are maintained. Waveshare's 1.75 examples are validated on 5.5.x and 6.0.x. 6.0 moves to MbedTLS 4 and Picolibc, which can break third-party components; so start on 5.5.x, run CI on both from v0.3, switch default when all pinned deps build clean. |
| RTOS | FreeRTOS (IDF SMP build, dual-core S3) | Nothing to decide; it is the IDF default. |
| GUI | **LVGL 9.x, pin exact minor (registry shows 9.6.0 current)** [D] | Waveshare's IDF examples already use LVGL 9. LVGL has a mature SDL2 simulator, round-friendly widgets (arc, scale, canvas), styles/themes, and an `esp_lvgl_port` glue component. Pin to one minor; LVGL minor bumps change APIs. |
| Language | C core + C++ allowed in apps/services behind `extern "C"` headers [D] | C for HAL/runtime ABI stability (matters for future dynamic apps). |
| Build | `idf.py` + CMake, ESP-IDF components, IDF Component Manager for third-party deps with a committed `dependencies.lock` | |
| Not Arduino | Arduino examples for this board are on LVGL 8.4 and mix frameworks poorly. | Arduino-as-component is possible but unnecessary. |

Vendor BSPs: Waveshare publishes managed components (e.g. `waveshare/esp32_s3_touch_amoled_1_75`). **Use them as reference/bring-up only.** [D] Copy the init sequences (CO5300 QSPI init commands, CST9217 touch) into our own `hal_*` drivers behind our interface, honoring licences. Reason: the BSP bundles its own LVGL init, pin macros and I/O expander assumptions that would leak through our HAL. Fallback: wrap the BSP inside `boards/<name>/` if a driver is not worth re-writing.

Known hardware facts (1.75, from Waveshare repo/wiki): CO5300 QSPI AMOLED 466x466, CST9217 I2C touch (2 points); the "1.75C" variant has 32MB flash/8MB PSRAM. **[A]** The 2.16 board's controller, touch IC and expander are unverified; the design assumes only "QSPI panel + I2C touch + PCA/TCA-style expander" is possible.

---

## 2. Layered architecture

```
┌───────────────────────────────────────────────────────────────┐
│ Apps (built-in, static): home clock domoticz energy settings   │
│       later: script apps (WASM/Lua) behind same app ABI        │
├───────────────────────────────────────────────────────────────┤
│ Shell: launcher │ app switcher │ gestures │ status ring │      │
│        notifications overlay │ quick settings │ lock/sleep     │
├───────────────────────────────────────────────────────────────┤
│ App Runtime: registry │ lifecycle │ permissions │ app memory   │
│              event bus │ UI thread owner (LVGL task)           │
├──────────────────────────┬────────────────────────────────────┤
│ UI Toolkit (ru_*)        │ Data layer                         │
│ round layout, cards,     │ typed models, freshness<T>, units, │
│ gauges, theme, i18n      │ integrations (domoticz, ha, mqtt)  │
├──────────────────────────┴────────────────────────────────────┤
│ Services (own tasks): config/secrets │ wifi │ time │ http+TLS  │
│  mqtt │ log │ ota │ power │ storage                            │
├───────────────────────────────────────────────────────────────┤
│ HAL (interfaces + capability flags) ◄── board profile (boards/)│
│ display touch power rtc imu audio storage board_caps           │
├───────────────────────────────────────────────────────────────┤
│ ESP-IDF drivers (esp_lcd, i2c master, sdmmc, i2s) + FreeRTOS   │
└───────────────────────────────────────────────────────────────┘
Host build: same layers 2-5 compiled for Linux; HAL + services replaced by sim/mock implementations.
```

Dependency rule (enforced by CMake `REQUIRES`, checked in CI): upward calls only via events/callbacks; apps depend on `ru_runtime`, `ru_ui`, `ru_data` public headers only, never on `hal_*`, ESP-IDF headers, or `lvgl` internals directly (LVGL public API is allowed since apps build UI with it).

---

## 3. Repo and component layout

```
dzpanel/
├── CMakeLists.txt, sdkconfig.defaults, sdkconfig.defaults.<board>
├── main/                      # app_main: board select, start runtime. tiny.
├── boards/
│   ├── board_common/          # board_profile.h, board_caps.h, registry
│   ├── amoled_175/            # profile + pins + panel init table
│   ├── amoled_216/
│   └── sim_466/ sim_480/      # host "boards"
├── components/
│   ├── hal/                   # hal_display.h hal_touch.h hal_power.h ... (interfaces only)
│   ├── hal_drivers/           # co5300, cst9217, axp2101?, pcf85063?, qmi8658?, es8311?  [A: chips]
│   ├── ru_core/               # event bus, freshness, units, result/err, log macros, pool alloc
│   ├── ru_runtime/            # app registry, lifecycle, permissions, ui thread, memory guard
│   ├── ru_ui/                 # layout/safe-area, widgets, theme, i18n, icons
│   ├── ru_shell/              # launcher, switcher, status ring, overlays, lock
│   ├── svc_config/ svc_wifi/ svc_time/ svc_http/ svc_mqtt/ svc_log/ svc_ota/ svc_power/ svc_storage/
│   ├── ru_data/               # models + integration interface
│   ├── int_domoticz/ int_mqtt/ int_ha/ (later)
│   └── ru_script/             # later: WASM/Lua host
├── apps/                      # each app is a component: app_home, app_clock, app_domoticz, app_energy, app_settings
├── sim/                       # SDL2 host build (CMake, not idf), mock services, fixtures loader
├── tests/                     # host unit tests (Unity or Catch2 via CTest); fixtures/ sanitized JSON
├── tools/                     # partition gen, asset (icon/font) pipeline, flash scripts, fixture sanitizer
├── docs/ {decisions/, hardware/, research/}
└── .github/workflows/         # host tests+sim screenshots, idf build matrix (board x idf version)
```

Rules: component names prefixed (`ru_`, `svc_`, `hal_`) to avoid collisions; every component compiles on host if its `REQUIRES` are host-capable (portable code lives in `ru_core`, `ru_data`, `int_*`, `ru_ui` layout math). Platform glue sits in `*/port_esp32.c` vs `*/port_host.c`, selected by CMake.

---

## 4. HAL

Principles: C vtable-style interfaces, one instance per board, **every optional device is nullable**; `board_caps` bitmask is the single source of truth for what exists. Init failure of an optional device downgrades the capability, never aborts boot. All HAL calls are thread-safe or documented otherwise.

```c
// board_caps.h
typedef enum {
  RU_CAP_TOUCH       = 1u<<0,  RU_CAP_BATTERY   = 1u<<1,  RU_CAP_RTC     = 1u<<2,
  RU_CAP_IMU         = 1u<<3,  RU_CAP_AUDIO_OUT = 1u<<4,  RU_CAP_MIC     = 1u<<5,
  RU_CAP_SDCARD      = 1u<<6,  RU_CAP_PSRAM     = 1u<<7,  RU_CAP_HAPTIC  = 1u<<8,
  RU_CAP_BUTTONS     = 1u<<9,  RU_CAP_DISPLAY_BRIGHTNESS = 1u<<10,
  RU_CAP_WIFI        = 1u<<11, RU_CAP_BLE = 1u<<12,
} ru_cap_t;

typedef enum { RU_OK=0, RU_ERR_UNSUPPORTED, RU_ERR_NOT_INIT, RU_ERR_IO, RU_ERR_TIMEOUT,
               RU_ERR_NO_MEM, RU_ERR_INVALID, RU_ERR_DENIED, RU_ERR_BUSY } ru_err_t;

// board_profile.h
typedef struct {
  const char *id;                 // "amoled_175"
  const char *name;
  uint32_t    caps;               // static caps; runtime caps may be reduced after init
  struct { uint16_t w, h; uint8_t bpp; uint16_t diameter_mm_x10; // [A] physical size for dpi
           uint16_t safe_inset_px;     // inset of safe circle from edge (corner+bezel)
           uint8_t  rotation;          // 0/90/180/270 native->logical
           uint16_t dpi; } display;
  struct { uint32_t max_hz; bool tearing_pin; bool dma_internal_only;
           uint8_t  fb_strategy; /* RU_FB_* §11 */ uint16_t draw_buf_lines; } display_hw;
  struct { uint8_t swap_xy, mirror_x, mirror_y; uint8_t max_points; } touch;
  const struct ru_pin_map *pins;  // opaque board-private
  ru_err_t (*init_hw)(const struct ru_hal **out);   // fills HAL, sets runtime caps
  const char *hw_revision_probe;  // [A] I2C probe / strap to tell revisions apart
} ru_board_profile_t;

// hal_display.h
typedef struct ru_hal_display {
  ru_err_t (*flush)(struct ru_hal_display*, int x0,int y0,int x1,int y1, const uint16_t *px);
  void     (*set_flush_done_cb)(struct ru_hal_display*, void (*cb)(void*), void *ctx); // ISR-safe signal
  ru_err_t (*set_brightness)(struct ru_hal_display*, uint8_t pct);   // 0..100
  ru_err_t (*set_power)(struct ru_hal_display*, bool on);            // AMOLED sleep/display-off
  ru_err_t (*get_info)(struct ru_hal_display*, ru_display_info_t *out);
} ru_hal_display_t;

// hal_touch.h
typedef struct { int16_t x, y; uint8_t id; bool pressed; } ru_touch_pt_t;   // logical coords
typedef struct ru_hal_touch {
  ru_err_t (*read)(struct ru_hal_touch*, ru_touch_pt_t *pts, uint8_t *n);
  ru_err_t (*set_int_cb)(struct ru_hal_touch*, void (*isr_cb)(void*), void *ctx);
  ru_err_t (*set_wake_enabled)(struct ru_hal_touch*, bool en);   // touch-to-wake from light sleep
} ru_hal_touch_t;

// hal_power.h  (PMIC/fuel gauge optional)
typedef struct { uint16_t mv; int8_t pct; bool charging, usb_present; int16_t ma; } ru_batt_t;
typedef struct ru_hal_power {
  ru_err_t (*battery)(struct ru_hal_power*, ru_batt_t *out);     // UNSUPPORTED if no gauge
  ru_err_t (*set_rail)(struct ru_hal_power*, ru_rail_t r, bool on); // audio amp, SD, etc.
  ru_err_t (*poweroff)(struct ru_hal_power*);
  uint32_t (*wake_reason)(struct ru_hal_power*);
} ru_hal_power_t;

// hal_rtc.h / hal_imu.h
typedef struct ru_hal_rtc { ru_err_t (*get)(struct ru_hal_rtc*, struct tm*);
                            ru_err_t (*set)(struct ru_hal_rtc*, const struct tm*); } ru_hal_rtc_t;
typedef struct { float ax, ay, az, gx, gy, gz; int64_t t_us; } ru_imu_sample_t;
typedef struct ru_hal_imu { ru_err_t (*read)(struct ru_hal_imu*, ru_imu_sample_t*);
  ru_err_t (*set_odr)(struct ru_hal_imu*, uint16_t hz);
  ru_err_t (*set_motion_irq)(struct ru_hal_imu*, void (*cb)(void*), void*); } ru_hal_imu_t; // wake-on-motion

// hal_audio.h
typedef struct ru_hal_audio {
  ru_err_t (*play_pcm)(struct ru_hal_audio*, const int16_t *s, size_t n, uint32_t rate, uint8_t vol_pct);
  ru_err_t (*stop)(struct ru_hal_audio*);
  ru_err_t (*mic_start)(struct ru_hal_audio*, uint32_t rate, ru_pcm_cb_t cb, void*);  // NULL if no mic
  ru_err_t (*mic_stop)(struct ru_hal_audio*);
} ru_hal_audio_t;

// hal_storage.h : SD card mount (FATFS) + internal flash partition (LittleFS) behind one VFS
typedef struct ru_hal_storage {
  ru_err_t (*mount_sd)(struct ru_hal_storage*, const char *mp);   // UNSUPPORTED/IO if absent
  bool     (*sd_present)(struct ru_hal_storage*);
  ru_err_t (*sd_info)(struct ru_hal_storage*, uint64_t *total, uint64_t *free);
} ru_hal_storage_t;

// aggregate
typedef struct ru_hal {
  const ru_board_profile_t *board;
  ru_hal_display_t *display;  ru_hal_touch_t *touch;  ru_hal_power_t *power;
  ru_hal_rtc_t *rtc;  ru_hal_imu_t *imu;  ru_hal_audio_t *audio;  ru_hal_storage_t *storage;
  uint32_t caps_runtime;      // caps actually working
} ru_hal_t;
const ru_hal_t *ru_hal(void);
static inline bool ru_has(uint32_t cap){ return ru_hal()->caps_runtime & cap; }
```

I2C bus: single owner (`hal_i2c_bus` with mutex) shared by touch, RTC, IMU, PMIC, codec, expander. Many such boards route reset/enable lines via an I/O expander **[A]**; the board profile `init_hw` owns that, so the rest of the HAL calls `set_rail`, not GPIOs.

Brightness/AMOLED notes: expose brightness only through HAL; the power service implements dimming policy. Burn-in mitigation (§8 power, §15 theming): dark theme default, shell periodic pixel shift of static chrome, screen-off timeout default.

---

## 5. App model

### 5.1 Manifest and lifecycle

```c
typedef struct {
  const char *id;               // reverse-dns "org.dzpanel.domoticz", [a-z0-9.]+ ≤ 40
  const char *name_key;         // i18n key
  const ru_icon_t *icon;        // 1 bit-alpha mask or RGB565A8; recolored by theme
  uint16_t    version;          // semver packed
  uint16_t    api_level;        // runtime ABI level it was built for
  uint32_t    needs_caps;       // RU_CAP_*  hardware required (hide app if missing)
  uint32_t    wants_caps;       // optional hardware (app copes)
  uint32_t    perms;            // RU_PERM_* below
  uint32_t    ram_budget_int;   // bytes of internal RAM it may hold
  uint32_t    ram_budget_psram; // bytes of PSRAM
  uint16_t    ui_stack_hint;    // for event handler stack accounting (static apps share LVGL task)
  uint8_t     flags;            // KEEP_ALIVE_BG, NO_SLEEP_WHILE_FOREGROUND, FULLSCREEN, ROUND_SAFE_NONE
} ru_app_manifest_t;

typedef struct ru_app_ops {
  ru_err_t (*init)   (ru_app_ctx_t*);                  // once, after registration; no UI; cheap. may return UNSUPPORTED.
  ru_err_t (*enter)  (ru_app_ctx_t*, lv_obj_t *screen);// build UI on app-owned screen; subscribe to events
  void     (*event)  (ru_app_ctx_t*, const ru_event_t*);// runs on UI thread
  void     (*tick)   (ru_app_ctx_t*, uint32_t now_ms); // optional 1 Hz while foreground
  void     (*leave)  (ru_app_ctx_t*);                  // going to background/suspend; persist state; stop animations
  void     (*resume) (ru_app_ctx_t*);                  // optional, from background (if screen kept)
  void     (*release)(ru_app_ctx_t*);                  // free everything; screen is deleted by runtime after
  void     (*on_low_memory)(ru_app_ctx_t*, ru_mem_level_t lvl); // optional; drop caches
} ru_app_ops_t;

typedef struct { const ru_app_manifest_t *m; const ru_app_ops_t *ops; } ru_app_desc_t;
#define RU_APP_REGISTER(desc)  /* places pointer in linker section ".ru_apps" */ \
  __attribute__((used, section(".ru_apps"))) static const ru_app_desc_t *_ru_##desc = &desc
```

Registration: linker-section array (static, no init-order surprises) [D]. The ESP-IDF note: components are linked only if referenced; use `WHOLE_ARCHIVE` on the app components (`target_link_libraries(... -u)` or `idf_component_register(... WHOLE_ARCHIVE)`). Simpler fallback: generated `apps_table.c` from a Kconfig/`apps.yml` list (also gives a per-board app enable list). Prefer the generated table for portability to host and clear build config.

State machine: `REGISTERED → INITED → (ENTERED ↔ SUSPENDED) → RELEASED`. Exactly one foreground app. A background app (flag `KEEP_ALIVE_BG`, e.g. Notifications/Domoticz poller) has no UI, only service-side work, which should really live in an integration (§9), not an app. Default: leaving an app = `leave` then `release` after a grace period (default 30 s, or immediately on memory pressure), so RAM is returned. Home/launcher is never released.

### 5.2 Context, memory, settings

```c
struct ru_app_ctx {
  const ru_app_manifest_t *m;
  void *user;                         // app state, allocated from app pool
  ru_pool_t *pool;                    // app memory pool (§5.4)
  ru_settings_t *settings;            // NVS namespace "a.<hash8(id)>", see below
  const ru_services_t *svc;           // permission-filtered service table
  ru_bus_sub_t subs[RU_APP_MAX_SUBS]; // auto-unsubscribed on leave
  ru_ui_env_t ui;                     // screen, safe-area, theme, scale (§13)
};
```

- **Per-app settings:** `ru_settings_get/set_{i32,str,blob}(ctx->settings, key, ...)`; NVS namespace limit is 15 chars, so use `"a_" + 8-hex crc32(id)` [D]; keys ≤ 15 chars. Runtime keeps a registry id→namespace to detect collisions at register time. Quota: maintain a per-app key/byte count in the wrapper and refuse above manifest-agnostic default (e.g. 4 KB) to protect the shared NVS partition. Uninstall/reset app = `nvs_erase_all` on the namespace. Larger data (graph history) goes to a file under `/data/apps/<id>/` on LittleFS, not NVS.
- Settings are *app-owned*; shared secrets (Wi-Fi password, Domoticz token) belong to services and are never visible to apps (§8).
- Apps cannot call NVS, HTTP or sockets directly; they use `ctx->svc` which is a vtable filtered by `perms`. Static apps are trusted code, so this is *API hygiene and future-proofing, not a security boundary* (be honest in docs).

### 5.3 Permissions / capabilities

```c
RU_PERM_NET_HTTP=1<<0, RU_PERM_NET_MQTT=1<<1, RU_PERM_STORAGE_APP=1<<2, RU_PERM_STORAGE_SD=1<<3,
RU_PERM_AUDIO_OUT=1<<4, RU_PERM_MIC=1<<5, RU_PERM_IMU=1<<6, RU_PERM_NOTIFY_POST=1<<7,
RU_PERM_BRIGHTNESS=1<<8, RU_PERM_INTEGRATION_READ=1<<9, RU_PERM_INTEGRATION_CONTROL=1<<10,
RU_PERM_KEEP_AWAKE=1<<11, RU_PERM_SYSTEM=1<<31  /* settings, OTA: first-party only */
```
Hardware needs (`needs_caps`) are separate from permissions: an app with unmet `needs_caps` is hidden in the launcher; unmet `wants_caps` returns `RU_ERR_UNSUPPORTED` from the call. `INTEGRATION_CONTROL` calls (switch a light) additionally require the UI-level confirm policy (§9.4).

### 5.4 Screen and memory

- Each app gets its **own `lv_obj_t` screen** created by the runtime (`lv_obj_create(NULL)`), loaded with `lv_screen_load_anim`. On leave+release the runtime calls `lv_obj_delete(screen)`, which frees all child widgets and their styles/anims attached via the object. Apps must not leak global `lv_style_t` that outlive release (document; use `ctx->pool`-allocated styles and `lv_style_reset` in release).
- **LVGL heap:** configure LVGL to use `LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM` mapped to `heap_caps_malloc(…, MALLOC_CAP_SPIRAM)` with fallback to internal, **not** the built-in TLSF pool [D]. Reason: IDF's heap already offers capability-aware allocation and tracking, and a fixed pool wastes PSRAM. Cost: no per-app accounting at the LVGL level, but screen deletion frees correctly. Per-app accounting is done on `ctx->pool`/`ru_alloc(ctx, ...)` wrapper (counter + cap vs `ram_budget_*`) for app-owned data only; LVGL allocations are measured as a delta of heap free before/after `enter` and logged in debug builds [A: good-enough accounting].
- Image/font caches: LVGL image cache size set explicitly in Kconfig (e.g. 512 KB PSRAM); icons are RGB565A8 in flash (XIP, mmap'd), not decoded to RAM.
- **Low memory policy** (`ru_mem_monitor` task, 1 Hz; thresholds as **[A]**, tune on hardware):

| Level | Trigger (free internal 8-bit heap / largest PSRAM block) | Action |
|---|---|---|
| NORMAL | int > 40 KB and psram_largest > 1 MB | none |
| LOW | int < 40 KB or psram_largest < 1 MB | broadcast `on_low_memory(LOW)`; flush image cache; release suspended apps |
| CRITICAL | int < 20 KB or psram_largest < 256 KB | force-leave+release foreground app, return to Home, toast "Closed X: low memory"; refuse new `enter` unless it fits `ram_budget` |
| FATAL | alloc fails in runtime | log to RTC-noinit crash ring, controlled `esp_restart()` (not a panic) |

Entering an app first checks `heap_caps_get_free_size >= manifest budgets + margin`; failure shows an error screen, not a crash. Allocation failure inside LVGL objects must be treated as possible (`LV_ASSERT_MALLOC` → handler that routes to the FATAL path).

---

## 6. Event bus and threading

### 6.1 Threads

| Task | Core | Prio | Stack | Owns |
|---|---|---|---|---|
| `lvgl` (UI) | 1 [A] | 5 | 8 KB (internal) | **all LVGL calls**, app callbacks, shell, runtime state machine |
| `display_dma`/flush is callback + ISR | - | - | - | esp_lcd transfer-done ISR → `lv_display_flush_ready` |
| `touch` (interrupt → task or LVGL indev poll) | 1 | 6 | 3 KB | read touch, push to LVGL indev via `lv_indev` read cb (no task: LVGL polls, reading from ISR-filled buffer) [D] |
| `net` (svc_wifi + http worker pool of 2) | 0 | 4 | 6-10 KB (TLS: 10+ KB, mbedTLS buffers in PSRAM) | sockets, TLS |
| `integration` workers (one per integration) | 0 | 3 | 6 KB | polling, parsing (cJSON/yyjson), model updates |
| `svc` misc (config flush, log sink, power, ota) | 0 | 2-3 | 4 KB each | |
| `mem_monitor`, `idle hooks` | any | 1 | 2 KB | |

Rules (enforced by review and a debug-only `lv_lock` assertion, `RU_ASSERT_UI_THREAD()`):
1. **Only the `lvgl` task touches LVGL objects.** Others may call `ru_ui_post(fn, arg)` or publish events; `ru_ui_post` queues a closure (freed after run). `lvgl_port_lock` is available but **banned in app/service code**, reserved for the runtime [D] (simplicity, avoids priority inversion with 466² draw).
2. UI handlers must not block >~10 ms: no HTTP, no NVS commit, no SD I/O. Use `ru_job_submit(fn, done_cb)` (worker pool → posts completion to UI).
3. Services never call apps. They publish events or update models; apps subscribe.
4. Events carry small POD payloads (≤ 32 B inline) or a refcounted immutable blob pointer; never raw pointers to stack/mutable data.
5. No callbacks run in ISR except HAL "notify" shims that post to a queue with `FromISR`.

### 6.2 Bus

```c
typedef uint16_t ru_topic_t;   // enum, ranges per owner: 0x01xx SYS, 0x02xx NET, 0x03xx TIME, 0x04xx POWER,
                               // 0x05xx INPUT/GESTURE, 0x06xx NOTIFY, 0x10xx+ integrations/models
typedef struct { ru_topic_t topic; uint16_t len; int64_t t_us;
                 union { uint8_t b[24]; uint32_t u32[6]; const void *blob; } d; } ru_event_t;

ru_err_t ru_bus_publish(ru_topic_t, const void *payload, size_t len);        // any task; copies ≤24 B
ru_err_t ru_bus_publish_blob(ru_topic_t, ru_blob_t *refcounted);             // any task
ru_bus_sub_t ru_bus_subscribe(ru_topic_t first, ru_topic_t last_incl, ru_delivery_t how);
// how: RU_ON_UI (default: marshalled into the lvgl task queue, coalescing optional), RU_ON_CALLER_TASK (service-internal)
```
Design: one central dispatcher queue per delivery context (`ui_q` 32 entries, `svc_q` 32), subscribers stored in a static table (≤ 64). Publishing to a full queue drops the oldest of the same topic if flagged `COALESCE` (e.g. `NET_RSSI`, model-updated), else returns `RU_ERR_BUSY` and bumps a counter shown in diagnostics. Model updates publish only `{model_id, generation}`; UI then reads current value via a getter holding a short mutex (§10). The UI queue is drained inside the LVGL timer handler loop each iteration.

Deferred: topics as strings (rejected, cost and typos), full pub/sub libraries (esp_event is fine for system events; we wrap it **only** for IDF events (WIFI/IP) at the service boundary and do not expose it to apps).

---

## 7. Shell and navigation

Round-first: no rectangular nav bar. Shell objects live on LVGL's **top layer** (`lv_layer_top`) and **sys layer**, above the app screen, so apps are unaware of them.

| Element | Spec |
|---|---|
| Launcher | Home screen = watchface/clock with glance widgets. Swipe up (or tap center) → launcher: **radial/circular carousel** of icons on a ring (6 visible), snap scrolling by arc angle; page dots as an arc. Icons ≥ 25% of display diameter hit area → ~115px on 466 [A: finger-comfortable min 48px] |
| Gestures (global, in shell gesture recognizer on `lv_indev`) | edge-swipe from **left bezel** (angle 160-200°) = Back; swipe-down from top arc = notifications; swipe-up from bottom = launcher/home; swipe-up from bottom, hold = app switcher; two-finger tap (if touch ≥ 2 pts) = screenshot [debug]; long-press centre in Home = widget edit later. Apps may mark `FULLSCREEN`/claim `RU_GESTURE_HORIZONTAL` locally (e.g. a chart pan) and shell yields except for edge zones. Edge zones sized in **angle** and **radius band** (outer 12% radius), not pixels |
| Status ring | A thin arc (3 px) hugging the bezel inside the safe area: Wi-Fi/battery/sync state as colored segments (not color alone: also segment *position* and short icon on tap). Battery = arc length proportion on right side; connection state = dot at 12 o'clock; stale data = dashed segment. Hidden in fullscreen apps. Pixel-shifted ±2 px hourly (AMOLED) |
| Notifications overlay | Pull-down sheet clipped to circle; cards (title ≤ 24 ch, body ≤ 80 ch, priority, ts). Banner (toast) for NORMAL, full-screen takeover only for CRITICAL. Dismiss = swipe sideways. History ring buffer in RAM (32), optional SD/LittleFS persistence |
| Quick settings | Pull-up from Home: brightness arc (centre dial), Wi-Fi toggle, DND, sleep-now, lock. Large 3-4 tiles |
| App switcher | stack of the last 4 screens as shrunken cards, since apps are released the cards are `lv_snapshot` thumbnails at 1/4 res (RGB565, PSRAM, ~13 KB each); optional, v0.6 |
| Lock / sleep | Optional PIN (4-6 digit, salted hash in NVS) guarding *controls* and Settings, not display. Idle → dim (30 s) → screen off (60 s) → light sleep (§8). Wake: touch, button, IMU raise [A] |
| Always-on | Optional low-power watchface (dim, 1 Hz update, mostly black) - AMOLED makes this attractive, but must be measured; default **off** |

Navigation model: a stack with Home as root. `ru_nav_push(app_id, args)`, `ru_nav_back()`. Deep links `ru://domoticz/device/12` used by notifications. Screen transition animations ≤ 250 ms, disabled under CRITICAL memory or when frame time budget exceeded.

Gesture-vs-widget arbitration: shell gets first look at pointer-down events in edge zones only (via an invisible full-screen object on top layer with `LV_OBJ_FLAG_EVENT_BUBBLE`/`ADV_HITTEST` ring-shaped hit test). Everything else passes to the app untouched.

---

## 8. Services

Each service has: `start()/stop()`, a state topic, `status` struct, and host mock. Services own their task; apps see only async APIs.

| Service | Design decision |
|---|---|
| **config (`svc_config`)** | NVS (default partition, encrypted `nvs_keys` partition when flash encryption is enabled, §17) for small typed values; namespaces: `sys`, `net`, `sec`, `a_<hash>`. Typed schema table `{key, type, default, min, max, secret}` compiled in; unknown keys rejected. Writes are debounced (2 s) and batched. **Factory reset** = erase NVS + LittleFS; **backup/restore** = export non-secret config as JSON (secrets excluded unless user explicitly includes, encrypted with passphrase). Config can also be seeded from `sdkconfig` (dev) or provisioning (below). |
| **secrets** | Stored in a separate NVS namespace `sec`, API `ru_secret_use(id, cb)` that exposes the value only to the owning service (wifi, domoticz integration) inside a callback; never logged; masked in UI/JSON export (`****`). Not accessible to apps by permission design. With NVS encryption on, at rest protected [A: needs flash encryption/secure boot decisions]. |
| **provisioning** | First boot, no creds: show on-screen QR + SoftAP captive portal (IDF `wifi_provisioning` SoftAP, or tiny HTTP form) for SSID/pass/Domoticz URL/token. Settings app can later edit on-device with a round on-screen keyboard (ugly but necessary; also BLE provisioning later). Never hard-code in source; `config.example.json` + `sdkconfig` dev override file git-ignored. |
| **wifi (`svc_wifi`)** | State machine `OFF→CONNECTING→CONNECTED(ip)→DISCONNECTED(reason)→BACKOFF`; exponential backoff 1 s → 60 s with jitter; multiple saved networks (≤ 3); publishes `NET_STATE`, `NET_RSSI` (coalesced, 5 s). Power-save `WIFI_PS_MIN_MODEM` default. Exposes `ru_net_wait_up(timeout)` to workers. |
| **time (`svc_time`)** | SNTP (`esp_sntp`) after IP; configurable server (default `pool.ntp.org`, LAN override); POSIX TZ string (IANA→POSIX table for UI pick-list, ~30 zones initially, subset in flash); RTC (if cap) read at boot to give valid time offline, written after each SNTP sync. Publishes `TIME_SYNCED`, per-minute `TIME_TICK` (aligned to minute boundary; used by clock apps so no 1 Hz timers when not needed). `ru_time_valid()` flag; clock shows "--:--" or "no sync" tag until valid; freshness uses monotonic `esp_timer`, not wall clock, so a time jump cannot corrupt stale detection (§10). |
| **http (`svc_http`)** | Wrapper over `esp_http_client` with: request struct `{method,url,headers,body,timeout_ms,max_resp,tls_policy,cb}`, async via 2-worker pool, response streamed into a caller buffer or a streaming JSON parse callback, **hard caps** on body size (default 64 KB). TLS: `esp_crt_bundle` (Mozilla bundle) default; per-integration options `pin_sha256`, `ca_pem`, or `insecure_lan` (explicit, logged, shown in settings; many Domoticz installs use self-signed or plain HTTP on LAN). Keep-alive connection reuse per host (saves TLS handshake ~1-2 s and heap). Redirects limited to same host. No cloud dependency. |
| **mqtt (`svc_mqtt`)** | `esp-mqtt` wrapper, v0.6+, subscriptions by topic filter → integration, LWT, auto-reconnect aligned with wifi state. "Maybe" in the brief; recommended because it makes HA and Domoticz-MQTT work and gives push (no polling). Optional component (Kconfig). |
| **log (`svc_log`)** | `RU_LOGx(tag, ...)` macros wrap `ESP_LOGx` on target and printf-with-colors on host. Ring buffer 8 KB in PSRAM viewable in Settings > Diagnostics and exportable; optional sink to SD; optional UDP/TCP remote syslog (opt-in, off). Redaction helpers `ru_log_url()` strips credentials/query. Crash info: `esp_core_dump` to flash partition + a boot-count/last-reset-reason record in RTC noinit. |
| **ota (`svc_ota`)** | Partition scheme: `factory`? **No**: two OTA slots (`ota_0`, `ota_1`) + `otadata` + small `nvs`, `nvs_keys`, `coredump`, `storage` (LittleFS), flash ≥ 16 MB [A: 1.75 has 16 MB flash, 1.75C 32 MB; verify, and verify the 2.16]. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; new image must call `esp_ota_mark_app_valid_cancel_rollback()` only after: display up, wifi optional, runtime healthy for 30 s. Sources: manual HTTPS URL then GitHub-releases JSON; **signed images** (secure boot v2 RSA/ECDSA, or at minimum app-signature check `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`) from v0.8. USB-flash recovery doc always maintained. Never OTA on battery < 30% or while < 80 KB internal free. |
| **power (`svc_power`)** | Idle policy FSM: ACTIVE → DIM → SCREEN_OFF → LIGHT_SLEEP. DIM brightness 15%; AMOLED power draw scales with lit pixels, so dark theme matters. Touch IRQ/IMU/button as wake sources (`esp_sleep_enable_gpio_wakeup`), Wi-Fi in modem-sleep keeps connection under light sleep (DTIM listen interval 3, `CONFIG_PM_ENABLE` + `esp_pm_configure` with `light_sleep_enable`). Wake locks: `ru_power_lock(RU_LOCK_NO_SLEEP)` by apps with `KEEP_AWAKE`, OTA, in-flight HTTP. Battery % via HAL, smoothing, low-battery events at 20/10/5%, critical → save state, poweroff. Deep sleep not used by default (loses RAM state, 400+ ms re-init) except at <5% battery. CPU freq scaling 80/160/240 MHz via PM locks. [A: LVGL timers + I2C touch polling must be IRQ-driven for light sleep to engage.] |
| **storage (`svc_storage`)** | LittleFS on internal `storage` partition (`/data`: app files, history cache, icons overrides); FATFS on SD (`/sd`, hot-plug detect through HAL, optional). Path-sandboxed API: `ru_fs_open(ctx, "file")` maps to `/data/apps/<id>/file`. All I/O via worker (never UI thread). SD never required; features that use it declare `wants_caps`. |

---

## 9. Data model and integrations

### 9.1 Typed models, freshness, units

```c
typedef enum { RU_UNIT_NONE, RU_UNIT_C, RU_UNIT_F, RU_UNIT_PCT, RU_UNIT_W, RU_UNIT_KW, RU_UNIT_WH, RU_UNIT_KWH,
               RU_UNIT_V, RU_UNIT_A, RU_UNIT_HPA, RU_UNIT_LUX, RU_UNIT_EUR_KWH, RU_UNIT_M3, RU_UNIT_L, RU_UNIT_PPM } ru_unit_t;

typedef enum { RU_FRESH_UNKNOWN,   // never received
               RU_FRESH_OK,        // age ≤ ttl
               RU_FRESH_STALE,     // age > ttl  (value shown dimmed + age label)
               RU_FRESH_DEAD } ru_fresh_t; // age > 3*ttl or source disconnected long

typedef struct {                // "freshness-tagged value"; unavailable ≠ zero by construction
  float    v;
  ru_unit_t unit;
  bool     valid;               // false until first good sample, or source says N/A
  uint32_t ts_mono_ms;          // esp_timer at receipt (monotonic)
  uint32_t ts_wall;             // unix s at source update if known, else 0
  uint32_t ttl_ms;              // freshness window (poll interval × 3 default)
} ru_val_t;

static inline ru_fresh_t ru_val_fresh(const ru_val_t *x, uint32_t now_ms);
const char *ru_val_fmt(const ru_val_t*, char *buf, size_t n, ru_locale_t*); // "--" if !valid, never "0"
```
Rule: UI widgets that show data take `ru_val_t*` (not float), and render `OK / STALE (dim + clock glyph + "5m") / UNKNOWN ("--")` identically across apps; the `ru_ui_value_tile` widget does this once, so apps can't forget the empty/stale states (a design-doc requirement). Unit conversion (C↔F, W↔kW) happens in `ru_fmt` at display via locale/user prefs, models always store SI-ish canonical units.

Domain models (plain structs, no JSON inside): `ru_sensor_t {id, name, kind, ru_val_t value, ru_val_t secondary}`, `ru_switch_t {id, name, state enum {OFF,ON,UNKNOWN,PENDING}, bool dimmable, uint8 level, ru_fresh_t…}`, `ru_energy_t {grid_w, solar_w, batt_pct, batt_w, price_eur_kwh, day_import_kwh,… all ru_val_t}`, `ru_series_t` ring buffer of `{ts, int16 v}` (downsampled; 1 per 5 min, 288 pts/day, in PSRAM). Models live in a **model store** (`ru_store`): fixed-capacity tables, mutex-protected, `generation` counter; writers = integration workers; readers copy out snapshot (`ru_store_get_sensor(id,&out)`) in the UI thread. Updates publish `MODEL_CHANGED{kind, id}` (coalesced).

### 9.2 Integration abstraction

```c
typedef struct ru_integration_ops {
  const char *kind;                                          // "domoticz", "mqtt", "ha"
  ru_err_t (*configure)(ru_integration_t*, const ru_cfg_view_t*);   // validate config (URL, creds ref)
  ru_err_t (*start)(ru_integration_t*);                      // spawn poll/subscribe on worker
  void     (*stop)(ru_integration_t*);
  ru_err_t (*list_entities)(ru_integration_t*, ru_entity_info_t *out, size_t *n); // for pick-lists in settings
  ru_err_t (*subscribe)(ru_integration_t*, ru_entity_ref_t ref);   // add entity to the watched set
  ru_err_t (*command)(ru_integration_t*, ru_entity_ref_t, const ru_cmd_t*, ru_cmd_done_cb, void*);
  ru_conn_state_t (*conn_state)(ru_integration_t*);          // DISCONNECTED/CONNECTING/OK/AUTH_FAILED/ERROR
} ru_integration_ops_t;

typedef struct { uint8_t integ;  uint32_t id; } ru_entity_ref_t;   // stable ref, persisted by apps in settings
```
Integrations write models into the store with the *same typed schema*; apps and shell don't know the source. Mapping tables (integration-specific "idx 12 → ru_sensor kind TEMP") live in config as the **entity watch list** (user picks via `list_entities` UI): no hard-coded household indexes (design doc requirement). Connection state publishes `INTEG_STATE{integ, state, last_ok}` which feeds the status ring, and every model gets stale tracking even if the integration dies silently.

### 9.3 Domoticz client

- API: JSON over HTTP(S) `GET /json.htm?type=command&param=getdevices&rid=<idx>` (single-device) and `...&filter=all&used=true&favorite=1` for lists; **verify against the user's Domoticz version** (2023+ switched toward `type=command&param=getdevices`; `/json.htm?type=devices` is deprecated) [A]. Auth: Basic auth or API token header per Domoticz config; secrets via §8.
- Polling: one batch request per cycle for favorites / watch-list (filter by `rid` list is not supported in a single call in all versions, so use `favorite=1` or `used=true&order=...` and filter locally; cap response 64 KB, parse **streaming** with a SAX-like parser (cJSON would need the whole tree, ~3-5× body size in RAM; `yyjson` read-only in PSRAM is an alternative); [D] start with cJSON on ≤ 32 KB, switch to a streaming extractor if bodies exceed).
- Intervals: foreground app = 10 s, home glance = 30 s, background/dim = 120 s, screen off = suspended; energy = 10 s while visible. Exponential backoff on errors 2→60 s; on `401/403` stop and raise `AUTH_FAILED` notification (no retry storms).
- Commands: `param=switchlight&idx=N&switchcmd=On|Off|Toggle` / `setcolbrightnessvalue`. State shows `PENDING` until next poll confirms (or 5 s timeout → revert + error toast). Command safety (§9.4).
- Energy: map to Domoticz P1/Usage/"Electric (Instant+Counter)" devices via user-selected watch roles (`grid`, `solar`, `battery`, `price`); parse the "Data"/"Usage" strings (`"1234 Watt"`) with unit-aware parser, not `atof`; fixtures cover variations. Missing role → `valid=false`, not 0.
- Fixtures: `tests/fixtures/domoticz/*.json` sanitised via `tools/sanitize_fixture.py`; parser tested on host; `sim` mock server can replay them with induced latency/errors.
- Push later: Domoticz MQTT plugin / webhooks via `svc_mqtt` (no polling), same models.

### 9.4 Control safety

`ru_cmd_t` carries `risk` (SAFE, CONFIRM, LOCKED). Policy table in config by entity (default: lights SAFE, locks/garage/heating-setpoint/scene CONFIRM, alarm LOCKED requiring PIN). CONFIRM uses shell-owned dialog with press-and-hold (800 ms) control, not a second tap, to resist accidental touch; commands are rejected if the model is not `OK` fresh (no acting on stale state), if screen was woken < 500 ms ago (wake-touch swallowed), or if display is dimmed (first touch wakes only).

---

## 10. Memory budget and framebuffer strategy

Baseline **[A]** (verify on the real boards): ESP32-S3, 512 KB SRAM (~300-340 KB usable internal heap after Wi-Fi/BT + IDF), 8 MB octal PSRAM (1.75 reportedly 8 MB) , 16 MB flash.

Framebuffer math at RGB565 (16 bpp): 466² × 2 = **434 KB**; 480² × 2 = **461 KB**. RGB888 (the CO5300 supports 18/24 bpp over QSPI, but 16 bpp is the practical choice): 652 KB / 691 KB.

| Strategy | Description | Verdict |
|---|---|---|
| **Partial rendering, 2 draw buffers in internal DMA RAM** | LVGL `LV_DISPLAY_RENDER_MODE_PARTIAL`, buffers = N lines × width × 2 B, e.g. 40 lines → 37 KB each ×2 = 75 KB internal | **Default** [D]. Internal RAM for DMA to QSPI is the most reliable on S3 + QSPI (PSRAM→QSPI DMA contends with PSRAM bus, and cache). Works with the panel's own GRAM, so no full framebuffer is needed (AMOLED controllers have internal GRAM and take partial window writes via CASET/RASET). |
| Full-screen FB in PSRAM, `FULL`/`DIRECT` mode | 434-461 KB PSRAM ×1-2 | Fallback if partial rendering shows tearing on animation or clip artefacts; costs ~1 MB PSRAM and PSRAM bandwidth (S3 octal 80 MHz ≈ up to ~ 60-80 MB/s measured typical, shared with Wi-Fi + flash-cache) |
| Hybrid | Partial buffers in internal RAM, snapshot FB in PSRAM only for transitions/thumbnail | Used for switcher thumbnails (§7) |

Make strategy a board-profile field (`fb_strategy`, `draw_buf_lines`) and measure on hardware: target ≥ 30 fps for scrolling, ≥ 60 for small arcs; QSPI at 40 MHz = 5 MB/s×... → full-frame 434 KB takes ~87 ms at 40 MHz QSPI (4-bit @ 40 MHz = 20 MB/s ⇒ ~22 ms; **[A]** check actual panel max clock, frequently 40-80 MHz). Practical: **partial updates & dirty-rect only**; avoid full-screen animated gradients, prefer arcs/lines and opacity-free fills. Enable `LV_USE_DRAW_SW` with ESP32-S3 SIMD assembly accelerations (LVGL 9 has S3 PIE optimizations for blending, [A] verify in the pinned version) and Draw unit threading only if CPU-bound.

Budget table [A]:

| Pool | Item | Budget |
|---|---|---|
| Internal (≈320 KB) | IDF + Wi-Fi/LWIP (with `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) | ~100 KB |
| | LVGL draw buffers 2× | 75 KB |
| | Task stacks (~10 tasks) | 50 KB |
| | DMA/driver bufs, mbedTLS (TLS in-out buffers in PSRAM via `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`; handshake still wants ~40 KB internal contiguous) | 30-50 KB |
| | **Headroom floor** | ≥ 40 KB |
| PSRAM (8 MB) | LVGL heap (screens, styles, images cache) | 1.5-2 MB |
| | Fonts loaded (compressed) / image cache | 0.5 MB |
| | HTTP/JSON buffers, series, log ring, notification history | 0.5 MB |
| | Thumbnails, spare | 0.5 MB |
| | Script runtime later | 1-2 MB reserved |
| | Free | > 3 MB |

Fonts: use LVGL font converter, 2-4 bpp, subset glyphs per language, stored in flash `.rodata` (XIP). Large CJK would need a file-backed font on LittleFS (`lv_binfont`). Icons/images as `RGB565A8` or `A8` masks (recolorable). Never decode PNG at runtime on UI thread (use pre-converted bins; PNG/JPG only for user content via worker).

Flash cache vs PSRAM: enable `CONFIG_SPIRAM_FETCH_INSTRUCTIONS`/`RODATA` only if flash-cache contention measured, needing PSRAM (no). Put LVGL blend hot paths in IRAM only if profiling says so (`CONFIG_LV_ATTRIBUTE_FAST_MEM`).

---

## 11. Display-profile scaling and round layout

- Everything in layout is expressed relative to `ru_ui_env_t`: `{w,h, cx,cy, radius, safe_radius, dpi, scale}`. Units: `RU_PCT(x)` = x% of diameter (as integer 0..1000 permille), `RU_RPX(r,deg)` polar placement helpers, `RU_DP(n)` density-independent px (`n × dpi/160` against a 466 reference so 466 ⇒ 1.0, 480 ⇒ ≈1.03 [A: 2.16 is bigger physically, so dpi differs ~ 1.75" →466 = 376 ppi vs 2.16" → 480 ≈ 311 ppi: text in dp renders physically larger on the 2.16]).
- Safe area: circular, `safe_radius = radius − safe_inset_px` (corner radius is infinite for circles; the inset represents bezel masking + finger reach, default 6% of radius); helpers `ru_safe_inscribed_rect(aspect)` (largest rect inside the circle, 0.707×d for a square), `ru_safe_chord_width(y)` = `2*sqrt(R² − (y−cy)²)` so list rows get their allowed width at each y (rows near top/bottom are narrower: the list widget shrinks/curves row width), `ru_polar_to_xy`.
- Layout archetypes in toolkit (apps pick one rather than hand-place): `Glance` (centre value + ring gauge), `Ring list` (curved list, centre-focused item enlarged), `Grid of 2×2/3 tiles` inside inscribed square, `Pager` (horizontal pages with arc dots), `Chart` (inscribed rect with chord-clipped axes), `Dial` (arc input).
- Fonts by semantic role (`RU_FONT_DISPLAY/H1/BODY/CAPTION`), size chosen per profile table (e.g. body 20 px @466, 21 px @480), min caption 14 px [A: tune on device]. Min touch target 48 dp, preferably 56.
- Layout math is pure, unit-testable on host (`ru_ui/geom.c`), golden values at 466 and 480.
- Rotation: native display rotated in HAL (swap/mirror in profile); for round displays only 0° is needed, the profile field is mainly for mounting differences. Touch coordinate transform is in HAL so LVGL sees logical coordinates.
- Visual regression: simulator screenshots of each app at both sizes saved in CI (artefact compare with tolerance, §12).

---

## 12. Host simulator and tests

- **Sim:** separate CMake project `sim/` (plain CMake, not idf.py) building LVGL 9 with SDL2 driver, window 466x466 or 480x480 (`-DRU_SIM_PROFILE=466|480`, runtime flag `--size`), round mask drawn by an overlay (circle clip + bezel image) so clipping errors are visible. Compiles `ru_core ru_runtime ru_ui ru_shell ru_data int_* apps/*` **unchanged**. Replaces HAL by `sim_hal` (SDL display/mouse-as-touch, fake battery slider, fake RTC, keyboard shortcuts for gestures) and services by mocks: `svc_wifi_mock` (UI buttons to toggle states), `svc_time_host` (real clock + warp factor), `svc_http_host` (libcurl or in-process fixture server with latency/error injection), `svc_config_host` (JSON file), `svc_log_host`. Same event bus implementation (FreeRTOS replaced by a tiny `osal` over pthreads; **`ru_osal.h`: queue, mutex, task, timer, tick** with ESP32 port = FreeRTOS, host port = pthreads) [D]. This also keeps the single-LVGL-thread rule honest in the sim: LVGL runs in its own thread, services in others, so threading bugs show up on host (add TSAN builds in CI).
- Simulated constraints: allocator wrapper capping LVGL heap to the board budget so memory-hungry screens fail in sim; frame-time reporting (not representative of ESP32 speed, document).
- Headless mode: `--headless --script scenario.json` runs input + clock scenarios and dumps PNG screenshots (LVGL snapshot or SDL readback) → CI visual regression and docs images.
- **Unit tests** (CTest + Unity or Catch2 [D: Unity for C components, runs under `ctest`; ESP-IDF's own Unity target tests only for on-device smoke]): geometry/safe-area, freshness/units/formatting, event bus (ordering, coalescing, drop counters), settings quota/namespace hashing, Domoticz parsers with fixtures, wifi backoff FSM, power idle FSM with fake clock, gesture recogniser with recorded touch traces (CSV traces from device), notification queue, memory-level state machine. Target: logic coverage ≥ 70% for `ru_core/ru_data/int_*`.
- CI: (1) host build+tests+ASAN/UBSAN; (2) sim screenshots at both sizes; (3) IDF build matrix `{amoled_175, amoled_216} × {idf 5.5, 6.x}` with size report (fail on > N% growth); (4) clang-format/clang-tidy/cppcheck; (5) optional hardware-in-loop later (self-hosted runner with a board, serial smoke test).

---

## 13. Theming and i18n

- **Theme:** semantic tokens struct `ru_theme_t { bg, surface, text, text_dim, accent, ok, warn, err, stale, ring_track, radii, font roles, anim_ms }`; applied via LVGL styles/themes (`lv_theme_t` custom or `lv_style` sets), switched at runtime (`ru_theme_set`) which refreshes only active screens. Ships `dark` (true-black bg for AMOLED; default) and `dim`/`high-contrast`; light theme optional (power-hungry on AMOLED, warn user). State never by color alone: ok/stale/err also use icon glyph or pattern/label. Accent color user-selectable (6 presets). Themes are data (const structs; later JSON on LittleFS).
- **i18n:** string keys → table per language (`const char *tbl[KEY_COUNT]`), generated from `locales/*.po`/YAML by `tools/i18n_gen.py` into C arrays (compile-time key check; missing → English fallback); `ru_tr(KEY_X)`; plurals via small rules table (en, nl, de, fr first, matching likely community). Locale also carries date/number format, 12/24h, first weekday, units system. Fonts subset per language at build time. RTL & CJK deferred (flag `[A]`: needs file-backed fonts, shaping: not before v1.0). Apps provide their own key tables registered at init.

---

## 14. Future: scripting and dynamic apps

| Option | Pros | Cons / cost on ESP32-S3 |
|---|---|---|
| **WASM (WAMR interpreter or wasm3)** | language-neutral (C, Rust, AssemblyScript), real sandbox (linear memory, import-only API), small ABI, stable binary format | Needs a host API for LVGL (hundreds of functions or a reduced binding), 10-50× slower than native (interp; OK for UI logic, not for drawing), 100-300 KB runtime + 64-256 KB per instance in PSRAM, wasm3 is in low-maintenance state [A: check], WAMR has fast-interp/AOT but AOT on Xtensa is immature [A] |
| **Lua (eLua/Lua 5.4) / LVGL bindings** | tiny (~100-200 KB), quick iteration, mature, LVGL has community Lua bindings (`lvgl_lua`, lua_lvgl) | no sandbox by default (must strip libs), GC pauses/PSRAM fragmentation, weaker typing; fine for tinkerers |
| **MicroPython / JS (Elk, QuickJS, Moddable XS)** | big community (MicroPython has lv_micropython), JS is familiar | MicroPython is a whole second runtime; QuickJS ~ 400+ KB + heap hungry; Moddable XS is excellent but a whole ecosystem incl. its own UI stack (not LVGL) |
| **ELF/native dynamic loading (IDF `esp_elf`, esp-idf `elf_loader`)** | native speed, full API | **no sandbox** (any app can crash/own the device), relocation/ABI fragility across IDF/LVGL updates, code must run from IRAM/PSRAM executable memory (S3 PSRAM exec is limited: cache-mapped instructions typically flash-only unless special config) [A], signature/trust problem, version skew nightmare |
| **Declarative UI (JSON/LVGL XML) + tiny expression logic** | safe by construction, very small, no VM; LVGL 9.x has an XML UI feature in development [A: verify maturity in 9.6] | limited logic, but covers 80% of "dashboard" use cases and fits the product |

Recommendation: **do not build dynamic apps before v0.8.** Order: (1) v0.7-0.8 **declarative "dashboard apps"** (JSON/XML layout + data bindings to the model store, e.g. user-defined Domoticz tiles) — this fits the brief's "configurable widgets/favourites" and is the cheapest 'third-party app' story; (2) v1.0+ **WASM via WAMR** as the sandboxed scripting runtime (exposing the same `ru_app_ops` ABI + reduced `ru_ui` API + `ctx->svc` permission-filtered services, so the permission model of §5.3 becomes real), keep Lua as a potential lightweight alternative; (3) **no ELF loading** (explicit non-goal; revisit only for signed first-party plug-ins). Prepare now at near-zero cost: keep the app ABI plain-C with `api_level`, avoid exposing LVGL internals, keep manifests as data, keep `RU_APP_REGISTER` indirection, route all service access via `ctx->svc`.

---

## 15. Security

Threat model **[A]**: home LAN device, physical access possible (shelf/wall), trusted first-party code, OTA is the main remote attack surface, no cloud.

- Secrets (Wi-Fi/API tokens): NVS encrypted partition; enable **flash encryption** (release mode) + **secure boot v2** before v1.0 (irreversible eFuse steps; ship dev builds unencrypted and document the migration; **ADR-006**). Until then document that physical access = secret access.
- Transport: HTTPS with CA bundle by default; per-integration pinned cert/CA option; explicit `insecure_lan` toggle labelled in UI; HTTP Basic over plain HTTP discouraged with a visible warning icon.
- Local provisioning portal: SoftAP with random WPA2 password shown on screen (QR), timeout 5 min, disabled after config; no always-on web server. Optional local web UI (v0.9) protected by a user-set password and CSRF-safe, LAN-only, no remote exposure; no UPnP.
- OTA: signed images, rollback, HTTPS only, version anti-rollback optional, size/rate limits.
- Input validation: all network data parsed with length caps; JSON depth limit; no `sprintf` into fixed buffers (use `snprintf`); cppcheck/clang-tidy in CI; fuzz Domoticz/JSON parsers on host (libFuzzer) [D: cheap and valuable].
- Controls: confirm/hold/PIN policy (§9.4); stale-state gating; rate-limit commands (≤ 2/s).
- Privacy: no telemetry; log redaction; document data flows in `docs/privacy.md`.
- Debug interfaces: JTAG/USB-serial-JTAG and UART download disabled in release eFuses [A]; coredumps may contain secrets in RAM, so encrypted partition or disabled in release.
- Supply chain: pin component versions via lock file, record licences (SBOM generation in CI, `tools/sbom`).

---

## 16. Staged roadmap

| Ver | Theme | OS capabilities delivered | Exit criteria |
|---|---|---|---|
| **v0.1 "Hello round"** | Bring-up | IDF project, board profile for 1.75, HAL display+touch+caps, LVGL partial rendering, touch test screen, log, heap/PSRAM stats; host sim skeleton (466) with the same UI test screen; CI host build + IDF build | boots reliably, 30+ fps simple UI, touch correct at edges; both IDF + sim build in CI |
| **v0.2 "Runtime"** | App model | `ru_runtime`: registry, lifecycle, app screens, event bus, osal, thread rules, settings (NVS) wrapper; Home + Clock apps; theme v1; 480 profile in sim, 2.16 board profile compiling | switching between 2 apps 1000× with no leak (heap delta ≈ 0); unit tests for bus/lifecycle |
| **v0.3 "Connected"** | Net services | wifi manager + provisioning, SNTP + RTC, config/secrets, status ring v1; http service with TLS; time/stale infrastructure (`ru_val_t`); 2.16 hardware bring-up | reconnect after router reboot; time valid offline w/ RTC; IDF 5.5+6.x matrix green |
| **v0.4 "Domoticz PoC"** | Data layer | integration interface, model store, Domoticz client (read-only), fixtures + parser tests, watch-list config, Domoticz app (sensors, switches), stale/empty/error states in `value_tile` | **MVP of the design doc**: boot→launcher→Domoticz screen on both boards, 8 h soak |
| **v0.5 "Shell"** | UX | launcher carousel, gestures (back/home/notifications), quick settings, notifications service + overlay, power service (dim/off/light sleep), low-memory policy, i18n framework (en+nl), settings app | gesture recogniser tested with traces; idle current measured & documented; release 0.5 public (design-doc "Phase 5") |
| **v0.6 "Control & energy"** | Features | switch control + confirm/hold policy, favourites editor, energy app (gauges/history series), MQTT service, battery/IMU/audio/SD HAL implementations as present (`caps`), diagnostics screen | no unconfirmed risky actions; energy view with missing data correct |
| **v0.7 "Integrations"** | Platform | Integration plugins finalised (Home Assistant REST/WebSocket or MQTT discovery as 2nd integration proving the abstraction), declarative dashboard apps (JSON), theme/accent options, app switcher + snapshots, API-level freeze draft (`api_level=1`) | second integration added without touching apps; docs for app developers |
| **v0.8 "Update & recover"** | Robustness | OTA with rollback + signed images, USB recovery docs, config backup/restore, crash dump viewer, factory reset flow, secure boot/flash encryption dry-run (dev eFuse board) | pulled-power during OTA recovers; rollback on failed boot demonstrated |
| **v0.9 "Hardening"** | Quality | fuzzing, soak 7 days, power tuning, always-on watchface (optional), PIN lock, local web config UI (optional), perf/regression budgets in CI, HIL smoke test | no leaks; documented RAM/flash budgets; accessibility pass (contrast, touch target) |
| **v1.0** | Stable platform | API level 1 stable for built-in/compile-time apps, 3 supported boards or revisions documented, security docs, contributor guide, app template repo, secure boot recommended-config | external contributor builds a new app from template in < 1 h |
| **post-1.0** | Extensibility | WASM (WAMR) scripted apps with permission enforcement, app packaging (`.rapp`: manifest+wasm+assets, signed), more boards, RTL/CJK i18n, BLE provisioning, voice (mic) experiments | |

Cross-cutting from v0.1: ADRs 001-006 written alongside, size/heap CI reports, conventional commits, licence audit (LVGL MIT, ESP-IDF Apache-2.0, project licence recommendation: **Apache-2.0 or MIT** for easy reuse; avoid copying GPL code from third-party BSPs; Waveshare example code is typically permissive but **check per file** [A]).

---

## 17. Key risks and open items (to verify first on hardware)

1. Actual free internal RAM after Wi-Fi + LVGL + TLS; whether partial buffers fit in internal DMA RAM at chosen line counts (§10).
2. QSPI max clock and tearing behaviour of the CO5300-class panel; need for TE pin sync; 2.16 controller identity and whether it shares the 1.75 driver.
3. I/O expander/PMIC/codec/IMU/RTC chip identity and I2C addresses per board revision; `hw_revision_probe`.
4. Light sleep + Wi-Fi + touch-wake achievable current; AXP-style PMIC rail control.
5. Domoticz API version differences in the user's instance (§9.3).
6. LVGL 9.6 + IDF 5.5 vs 6.x compatibility (esp_lvgl_port version) — unverified in this research.
7. Flash size/partition layout on the 2.16 (OTA needs two slots).
