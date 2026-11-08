#include "ui_shell.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "boot_health.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ru_apps.h"
#include "ru_bus.h"
#include "ru_gesture.h"
#include "ru_hal.h"
#include "ru_heap.h"
#include "ru_nav.h"
#include "ru_prefs.h"
#include "ru_osal.h"
#include "ru_runtime.h"
#include "ru_splash.h"
#include "ru_theme.h"
#include "ru_widgets.h"
#include "svc_glue.h"

static const char *TAG = "ui";

static struct {
  dz_service_t *svc;
  ru_dzapp_t *app;
  ru_runtime_t rt;
  ru_heap_t heap;
  ru_gesture_t gest;
  ru_bus_sub_t sub;
  ru_event_t subq[64];
  ru_queue_t *wake;
  atomic_bool app_active; /* the Domoticz apps own the screen (bridge may tick them) */
  bool safe;
  lv_obj_t *setup_scr;
  ru_kv_t *kv; /* interface preferences; may be NULL */
} U;

/* ---------- display diagnostics (RU_HEAP_LOG): what a refresh costs the panel ---------- */
#if CONFIG_RU_HEAP_LOG
#include "esp_timer.h"

static struct {
  volatile uint32_t inv_n, inv_px, inv_full, render_n, render_max_us;
  volatile int64_t render_t0;
} D;

static void disp_event_cb(lv_event_t *e) {
  switch (lv_event_get_code(e)) {
    case LV_EVENT_INVALIDATE_AREA: {
      const lv_area_t *a = lv_event_get_param(e);
      if (!a) break;
      uint32_t w = (uint32_t)lv_area_get_width(a), h = (uint32_t)lv_area_get_height(a);
      D.inv_n++;
      D.inv_px += w * h;
      if (w >= (uint32_t)ru_board()->diameter && h >= (uint32_t)ru_board()->diameter) D.inv_full++;
      break;
    }
    case LV_EVENT_RENDER_START: D.render_t0 = esp_timer_get_time(); break;
    case LV_EVENT_RENDER_READY: {
      uint32_t us = (uint32_t)(esp_timer_get_time() - D.render_t0);
      D.render_n++;
      if (us > D.render_max_us) D.render_max_us = us;
      break;
    }
    default: break;
  }
}

static void disp_diag_start(void) {
  lv_display_t *d = lv_display_get_default();
  if (!d) return;
  lv_display_add_event_cb(d, disp_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
  lv_display_add_event_cb(d, disp_event_cb, LV_EVENT_RENDER_START, NULL);
  lv_display_add_event_cb(d, disp_event_cb, LV_EVENT_RENDER_READY, NULL);
}

void ui_shell_log_disp(void) {
  ru_dzapp_stats_t st = {0};
  if (U.app) ru_dzapp_stats(U.app, &st, true);
  uint32_t n = D.inv_n, px = D.inv_px, full = D.inv_full, rn = D.render_n, rmax = D.render_max_us;
  D.inv_n = D.inv_px = D.inv_full = D.render_n = D.render_max_us = 0;
  ESP_LOGI("disp", "inv_n %u inv_px %u inv_full %u render_n %u render_max_us %u | rebuilds %u inplace %u evals %u build_max_us %u sig_max_us %u model_copies %u",
           (unsigned)n, (unsigned)px, (unsigned)full, (unsigned)rn, (unsigned)rmax, (unsigned)st.rebuilds, (unsigned)st.inplace,
           (unsigned)st.evals, (unsigned)st.build_max_us, (unsigned)st.sig_max_us, (unsigned)st.copies);
}
#else
static inline void disp_diag_start(void) {}
#endif

/* ---------- screens without Domoticz ---------- */

static lv_obj_t *label(lv_obj_t *s, const char *t, const lv_font_t *f, lv_color_t c, float dy_units) {
  lv_obj_t *l = lv_label_create(s);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, ru_u(dy_units));
  return l;
}

static lv_obj_t *build_setup_screen(void) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *s = ru_screen_new();
  lv_obj_t *rim = ru_status_rim_create(s);
  ru_status_rim_set_state(rim, RU_RIM_OFFLINE);
  label(s, "NOT SET UP", ru_font_title(), t->warn, -22);
  label(s, "OPEN THE DZPANEL INSTALLER", ru_font_body(), t->text, -8);
  label(s, "PAGE IN CHROME OR EDGE,", ru_font_body(), t->text, -1);
  label(s, "CONNECT THIS DEVICE BY USB", ru_font_body(), t->text, 6);
  label(s, "AND ENTER WI-FI AND DOMOTICZ", ru_font_body(), t->text, 13);
  label(s, svc_cfg_set() ? "WI-FI MISSING" : "NO DOMOTICZ SETTINGS YET", ru_font_small(), t->text_dim, 24);
  return s;
}

static void on_erase(lv_obj_t *w, ru_hold_event_t ev, void *ud) {
  (void)w;
  (void)ud;
  if (ev != RU_HOLD_CONFIRMED) return;
  svc_factory_reset();
  ru_sleep_ms(300);
  esp_restart();
}

static lv_obj_t *build_safe_screen(unsigned fails) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *s = ru_screen_new();
  lv_obj_t *rim = ru_status_rim_create(s);
  ru_status_rim_set_state(rim, RU_RIM_ERROR);
  label(s, "SAFE MODE", ru_font_title(), t->error, -30);
  char b[40];
  snprintf(b, sizeof b, "%u FAILED STARTS IN A ROW", fails);
  label(s, b, ru_font_body(), t->text, -18);
  label(s, "APPS ARE OFF. USB SETUP STILL WORKS.", ru_font_small(), t->text_dim, -10);
  label(s, "RE-FLASH WITH THE INSTALLER,", ru_font_small(), t->text_dim, -4);
  label(s, "OR HOLD TO ERASE SETTINGS", ru_font_small(), t->text_dim, 2);
  lv_obj_t *h = ru_hold_confirm_create(s, ru_u(24), "ERASE", 3000);
  lv_obj_align(h, LV_ALIGN_CENTER, 0, ru_u(20));
  ru_hold_confirm_on_event(h, on_erase, NULL);
  return s;
}

/* ---------- gestures: ONE recogniser; the shell owns the rim, ru_dzapp_swipe decides the content swipes ---------- */

static void shell_gesture(const ru_gesture_event_t *g) {
  if (!atomic_load(&U.app_active)) return;
  if (g->zone != RU_ZONE_RIM) {
    /* sections (finger left/right) and list pages (up/down); LVGL has no gesture handler left, so no double fire */
    if (g->type == RU_G_SWIPE) ru_dzapp_swipe(U.app, g->dir, (int)g->x0, (int)g->y0);
    return;
  }
  ru_nav_input_t in;
  if (!ru_nav_input_from_gesture(g, &in)) return;
  switch (in.type) {
    case RU_NAV_IN_BACK:
      ru_dzapp_back(U.app); /* false on Home: nothing to do */
      break;
    case RU_NAV_IN_GO_HOME:
    case RU_NAV_IN_HOME_ACTION:
      ru_runtime_go_home(&U.rt);
      ru_dzapp_home(U.app);
      break;
    default: break; /* quick controls, launcher, context: not built yet */
  }
}

static void on_touch(const ru_touch_t *s, void *ud) {
  (void)ud;
  ru_gesture_event_t ev[RU_GESTURE_MAX_EVENTS];
  int n = ru_gesture_feed(&U.gest, s, ev, RU_GESTURE_MAX_EVENTS);
  for (int i = 0; i < n; i++) shell_gesture(&ev[i]);
}

static void gesture_tick_cb(lv_timer_t *t) {
  (void)t;
  ru_gesture_event_t ev[RU_GESTURE_MAX_EVENTS];
  int n = ru_gesture_tick(&U.gest, ru_now_ms(), ev, RU_GESTURE_MAX_EVENTS);
  for (int i = 0; i < n; i++) shell_gesture(&ev[i]);
}

/* ---------- control timer: heartbeat, setup -> apps transition ---------- */

static void show_apps(void) {
  ru_dzapp_start(U.app); /* builds Home, installs its own 250 ms refresh timer */
  ru_runtime_go_home(&U.rt);
  atomic_store(&U.app_active, true);
  if (U.setup_scr) {
    lv_obj_delete(U.setup_scr);
    U.setup_scr = NULL;
  }
  ESP_LOGI(TAG, "apps on screen");
}

static void control_cb(lv_timer_t *t) {
  (void)t;
  bh_heartbeat();
  if (!U.safe && U.app && !atomic_load(&U.app_active) && svc_provisioned()) show_apps();
}

static void brightness_hook(int percent, void *ud) {
  (void)ud;
  ru_hal_set_brightness(percent);
}

/* Settings > START PAGE / ROTATION. The start page is applied by the app at once, here it is only stored. A rotation is stored and
 * applied by the panel driver at the next start (ru_hal_set_rotation before ru_hal_init), so it answers "restart"; the restart itself
 * comes when the user confirms the hold. A rotation the board cannot do, or a store that failed, is refused (nothing pending). */
static ru_pref_result_t pref_hook(ru_pref_t p, int value, void *ud) {
  (void)ud;
  switch (p) {
    case RU_PREF_START:
      return U.kv && ru_prefs_save_start(U.kv, value ? RU_START_FAVS : RU_START_CLOCK) == RU_KV_OK ? RU_PREF_APPLIED : RU_PREF_UNSUPPORTED;
    case RU_PREF_ROTATION:
      if (!U.kv || !((ru_board()->rotations | 1u) >> (value / 90) & 1u) || ru_prefs_save_rotation(U.kv, value) != RU_KV_OK)
        return RU_PREF_UNSUPPORTED;
      return value == ru_hal_rotation() ? RU_PREF_APPLIED : RU_PREF_NEEDS_RESTART;
    case RU_PREF_RESTART:
      ESP_LOGW(TAG, "restart to apply the rotation");
      ru_sleep_ms(200);
      esp_restart();
      break;
  }
  return RU_PREF_UNSUPPORTED;
}

static void on_fatal(void *ctx) {
  (void)ctx;
  ESP_LOGE(TAG, "memory FATAL: controlled restart");
  esp_restart();
}

/* ---------- bridge: dz_service bus events -> UI (own task, no LVGL calls without the lock) ---------- */

static void bus_wake(void *ctx) {
  (void)ctx;
  uint8_t b = 1;
  ru_queue_send(U.wake, &b, 0); /* a full queue just means "already pending" */
}

static void bridge_task(void *arg) {
  (void)arg;
  for (;;) {
    uint8_t b;
    ru_queue_recv(U.wake, &b, 2000);
    bool dirty = false;
    ru_event_t ev;
    while (ru_bus_poll(&U.sub, &ev)) {
      if (ev.type == DZ_EV_CMD_RESULT && ev.len >= sizeof(dz_ev_cmd_t)) {
        dz_ev_cmd_t r;
        memcpy(&r, ev.payload, sizeof r);
        ru_dzapp_post_cmd_event(U.app, r.req_id, (dz_outcome_t)r.outcome, (dz_fail_t)r.fail, (dz_cmd_kind_t)r.kind,
                                (dz_cmd_err_t)r.reject, r.level);
      }
      dirty = true;
    }
    if (dirty && atomic_load(&U.app_active)) {
      if (ru_hal_lock(200)) {
        ru_dzapp_ui_tick(U.app, ru_now_ms());
        ru_hal_unlock();
      }
      ru_sleep_ms(100); /* events keep coalescing on the bus meanwhile */
    }
  }
}

/* ---------- Domoticz backend: dz_service is the only owner ---------- */

static uint32_t be_submit(const dz_cmd_req_t *req, void *ud) { return dz_service_submit((dz_service_t *)ud, req); }
static dz_conn_state_t be_conn(void *ud) { return dz_service_conn_state((dz_service_t *)ud); }
static const ru_dzapp_backend_t k_backend = { be_submit, be_conn };

/* Boot splash: an overlay on the top layer, so the real first screen is built and loaded underneath it exactly as
 * before. It plays its draw-and-fade once, then fades out. Compiled, not run on a board. */
static lv_obj_t *s_splash;

static void splash_end_cb(lv_timer_t *t) {
  (void)t;
  if (s_splash) {
    ru_splash_dismiss(s_splash, 250);
    s_splash = NULL;
  }
}

static void splash_begin(void) {
  static char ver[24];
  const esp_app_desc_t *d = esp_app_get_description();
  snprintf(ver, sizeof ver, "v%.20s", d && d->version[0] ? d->version : "dev");
  ru_splash_cfg_t cfg = { "dzPanel", "for Domoticz", ver };
  s_splash = ru_splash_overlay_create(&cfg);
  if (!s_splash) return; /* no memory for the splash is not a reason to fail the boot */
  ru_splash_start(s_splash);
  lv_timer_t *t = lv_timer_create(splash_end_cb, RU_SPLASH_TOTAL_MS + 100, NULL);
  if (t) lv_timer_set_repeat_count(t, 1); /* one shot, then LVGL deletes the timer */
}

void ui_shell_start(dz_service_t *svc, bool safe_mode, unsigned failed_boots, ru_kv_t *kv) {
  const ru_board_t *b = ru_board();
  ru_theme_init(b->diameter);
  disp_diag_start();
  splash_begin();
  U.safe = safe_mode;
  U.svc = svc;
  U.kv = kv;
  lv_timer_create(control_cb, 250, NULL);
  if (safe_mode || !svc) {
    U.safe = true;
    lv_screen_load(build_safe_screen(failed_boots));
    return;
  }
  ru_gesture_cfg_t gcfg;
  if (b->round) ru_gesture_cfg_default(&gcfg, b->diameter);
  else ru_gesture_cfg_square(&gcfg, b->diameter); /* corners of a square panel are valid touches (RIM) */
  ru_gesture_init(&U.gest, b->diameter, &gcfg);
  ru_hal_set_touch_sink(on_touch, NULL);
  lv_timer_create(gesture_tick_cb, 20, NULL);

  ru_heap_init(&U.heap, 512 * 1024); /* accounting only: what apps allocate through the runtime */
  ru_runtime_init(&U.rt, &U.heap, NULL, NULL);
  ru_runtime_set_fatal(&U.rt, on_fatal, NULL);

  /* `client` is passed for read-only diagnostics (last error, request count); commands and state go via the backend. */
  U.app = ru_dzapp_create(dz_service_model(svc), &svc->client, true);
  if (!U.app) {
    ESP_LOGE(TAG, "app create failed");
    U.safe = true;
    lv_screen_load(build_safe_screen(failed_boots));
    return;
  }
  ru_dzapp_set_display_info(U.app, b->id, b->round);
  ru_dzapp_set_backend(U.app, &k_backend, svc);
  ru_dzapp_set_brightness_hook(U.app, brightness_hook, NULL);
  {
    ru_prefs_t prefs;
    ru_prefs_load(U.kv, &prefs);
    ru_dzapp_set_home_section(U.app, prefs.start == RU_START_FAVS ? RU_SEC_FAVS : RU_SEC_CLOCK);
    ru_dzapp_set_rotation_info(U.app, ru_hal_rotation(), b->rotations); /* the rotation in force, not the stored one: that one waits for a restart */
    ru_dzapp_set_pref_hook(U.app, pref_hook, NULL);
  }
  if (ru_apps_register(&U.rt, U.app) != RU_OK) ESP_LOGE(TAG, "app registration failed");

  U.wake = ru_queue_create(1, 1);
  uint16_t topic = DZ_SERVICE_TOPIC_DEFAULT;
  if (!U.wake || ru_bus_subscribe(svc->cfg.bus, &U.sub, U.subq, 64, 1u << topic, bus_wake, NULL) != RU_OK ||
      !ru_task_create("ui_bridge", bridge_task, NULL))
    ESP_LOGE(TAG, "bridge not started: the UI will refresh on its 250 ms timer only");

  if (svc_provisioned()) show_apps();
  else {
    U.setup_scr = build_setup_screen();
    lv_screen_load(U.setup_scr);
  }
}
