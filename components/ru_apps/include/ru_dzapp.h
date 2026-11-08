/* The Domoticz app: joins the model (data), the client and command layer (network) and the Halo widgets (UI).
 *
 * Generic: screens are chosen from dz_cap_t capabilities, never from device names or idx numbers.
 *
 * Threads. LVGL and every ru_dzapp_ui_* / ru_dzapp_start call belong to ONE UI thread. dz_client and dz_cmd are
 * not thread safe, so exactly one NETWORK thread may touch them: the UI never calls them, it posts requests into
 * a queue and reads results from another. The model has its own lock, so the UI reads it directly. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "dz_client.h"
#include "dz_cmd.h"
#include "dz_model.h"
#include "lvgl.h"
#include "ru_dzview.h"
#include "ru_fb.h"
#include "ru_gesture.h"
#include "ru_theme.h"
#include "ru_widgets.h"

typedef struct ru_dzapp ru_dzapp_t;

/* The pages (sections), in the order of the Domoticz web UI: Favourites (RU_SEC_FAVS), Lights, Scenes/Groups,
 * Temperature, Weather, Utility, Energy, Settings. Each one is a ru_app_t in ru_apps_table.c. Every page except Home and
 * Settings is hidden (not available) when the model has nothing for it. Membership of Lights / Temperature / Weather /
 * Utility comes from dz_device_t.tabs (dz_tabs_of), order from dz_model_order. */
typedef enum {
  RU_SEC_FAVS, RU_SEC_LIGHTS, RU_SEC_SCENES, RU_SEC_TEMP, RU_SEC_WEATHER, RU_SEC_UTILITY, RU_SEC_ENERGY, RU_SEC_SETTINGS,
  RU_SEC_COUNT
} ru_section_t;
const char *ru_section_name(ru_section_t s);

/* RU_VIEW_HOME = a section screen (any section, see ru_dzapp_section), DETAIL = a device, CONFIRM = hold to confirm,
 * SUB = a settings sub screen (connection, brightness, about). */
typedef enum { RU_VIEW_HOME, RU_VIEW_DETAIL, RU_VIEW_CONFIRM, RU_VIEW_SUB } ru_view_t;
typedef enum { RU_SUB_CONNECTION, RU_SUB_BRIGHTNESS, RU_SUB_ABOUT } ru_sub_t;

/* model and client stay owned by the caller and must outlive the app. allow_control=false (a Viewer token)
 * hides controls and refuses every command. Returns NULL on allocation failure. */
ru_dzapp_t *ru_dzapp_create(dz_model_t *model, dz_client_t *client, bool allow_control);
void ru_dzapp_destroy(ru_dzapp_t *a); /* stops the network thread if running */

/* ---- backend (device build) ----
 * On the device dz_service is the ONE owner of client, model and command layer. With a backend set, the UI sends
 * commands through submit() (thread safe, returns a request id, 0 = queue full), reads the connection state through
 * conn(), and the glue feeds outcomes back with ru_dzapp_post_cmd_event() (thread safe). ru_dzapp_net_* must then
 * not be used. The `client` passed to ru_dzapp_create is read only for diagnostics (last error, request count). */
typedef struct {
  uint32_t (*submit)(const dz_cmd_req_t *req, void *ud);
  dz_conn_state_t (*conn)(void *ud);
} ru_dzapp_backend_t;
/* Board id and panel shape, shown in Settings > About ("AMOLED_175 466 PX ROUND"). Optional; the shell sets it from ru_board(). */
void ru_dzapp_set_display_info(ru_dzapp_t *a, const char *board_id, bool round);
void ru_dzapp_set_backend(ru_dzapp_t *a, const ru_dzapp_backend_t *b, void *ud); /* b must outlive the app */
void ru_dzapp_post_cmd_event(ru_dzapp_t *a, uint32_t req_id, dz_outcome_t outcome, dz_fail_t fail, dz_cmd_kind_t kind,
                             dz_cmd_err_t reject, int level);

/* ---- network side ---- */
/* One iteration: submit queued requests, poll if due, pump command verification. Call from the network
 * thread only (or from a test, with no network thread running). */
void ru_dzapp_net_step(ru_dzapp_t *a, uint32_t now_ms);
/* Convenience: a thread that loops ru_dzapp_net_step and sleeps until the next due time. */
bool ru_dzapp_net_start(ru_dzapp_t *a);
void ru_dzapp_net_stop(ru_dzapp_t *a);

/* ---- UI side (UI thread) ---- */
/* Builds and loads the home screen and installs a 250 ms refresh timer. */
void ru_dzapp_start(ru_dzapp_t *a);
/* Refresh what changed. The timer calls this; tests call it directly with a virtual clock. */
void ru_dzapp_ui_tick(ru_dzapp_t *a, uint32_t now_ms);

/* Refresh counters for the `disp` diagnostics line (collected only when CONFIG_RU_HEAP_LOG is on, otherwise zeros):
 * screen rebuilds, in-place updates, signature evaluations, longest build / signature in microseconds, and dz_device_t
 * copies taken from the model. Counts and maxima since the last call that asked for a reset. */
typedef struct { uint32_t rebuilds, inplace, evals, build_max_us, sig_max_us, copies; } ru_dzapp_stats_t;
void ru_dzapp_stats(ru_dzapp_t *a, ru_dzapp_stats_t *out, bool reset);
/* Navigation, also used by the gesture layer later. */
void ru_dzapp_open(ru_dzapp_t *a, const char *idx, bool is_scene);
void ru_dzapp_home(ru_dzapp_t *a);
/* One step back: confirm -> where it came from (detail or settings sub), detail -> its section, sub -> Settings,
 * any other section -> Home. false on Home (nothing to go back to). Used by the on-screen back controls and the shell. */
bool ru_dzapp_back(ru_dzapp_t *a);
/* Square panels only: the corner back chevron on a detail, sub or confirm screen, in screen pixels. false on a round
 * panel or on a section screen (nothing is drawn there). */
bool ru_dzapp_back_rect(const ru_dzapp_t *a, lv_area_t *r);

/* ---- introspection (tests, screenshots) ---- */
ru_view_t ru_dzapp_view(const ru_dzapp_t *a);
const char *ru_dzapp_current_idx(const ru_dzapp_t *a);
const char *ru_dzapp_banner(const ru_dzapp_t *a); /* connection banner text shown on home, "" when fine */
lv_obj_t *ru_dzapp_screen(const ru_dzapp_t *a);
/* the ru_light widget of the open light detail page, NULL on every other screen (tests) */
lv_obj_t *ru_dzapp_light(const ru_dzapp_t *a);
size_t ru_dzapp_home_rows(const ru_dzapp_t *a); /* tiles / rows of the current section screen (the current page) */
const char *ru_dzapp_toast(const ru_dzapp_t *a); /* the visible result message, "" when none */
/* Tap the n-th home row exactly as a finger would. */
bool ru_dzapp_tap_row(ru_dzapp_t *a, size_t n);

/* ---- sections ---- */
bool ru_dzapp_section_available(ru_dzapp_t *a, ru_section_t s);
void ru_dzapp_show_section(ru_dzapp_t *a, ru_section_t s); /* unavailable section: falls back to Home */
ru_section_t ru_dzapp_section(const ru_dzapp_t *a);
/* +1 / -1 to the next / previous available section. No wrap: false (and no change) at the first / last. */
bool ru_dzapp_step_section(ru_dzapp_t *a, int dir);
/* THE decision point for a swipe in the CONTENT zone (the shell forwards RU_G_SWIPE with zone CONTENT; the rim
 * recogniser keeps the rest). finger left/right = next/previous section, finger up/down = next/previous page of a
 * list. (x0, y0) is where the finger went down (screen px). Refused (false, nothing changes) when it started in the
 * rim (r >= 0.90 R), while a slider is held (interact window), and on any screen that is not a section (detail,
 * confirm, sub) or at the end of the sections / pages. true = it moved. */
bool ru_dzapp_swipe(ru_dzapp_t *a, ru_dir_t dir, int x0, int y0);
/* Current page (0-based) and number of pages of the current list screen (1 for screens that are not paged). */
int ru_dzapp_page(const ru_dzapp_t *a);
int ru_dzapp_pages(const ru_dzapp_t *a);
void ru_dzapp_open_sub(ru_dzapp_t *a, ru_sub_t sub);

/* ---- feedback, platform hooks, test hooks ---- */
const ru_fb_t *ru_dzapp_feedback(const ru_dzapp_t *a);
void ru_dzapp_set_conn(ru_dzapp_t *a, dz_conn_state_t s);  /* simulator driver / tests, when no network thread runs */
void ru_dzapp_set_brightness_hook(ru_dzapp_t *a, void (*fn)(int percent, void *ud), void *ud);
int ru_dzapp_brightness(const ru_dzapp_t *a);
/* Called on the UI thread with every request the UI submits, right before it is queued (tests). */
void ru_dzapp_set_cmd_spy(ru_dzapp_t *a, void (*fn)(const dz_cmd_req_t *req, void *ud), void *ud);
/* Feed a command outcome as if dz_cmd had reported it (tests, simulator driver). */
void ru_dzapp_inject_result(ru_dzapp_t *a, const dz_cmd_result_t *r);
/* What the n-th tile or row of the current section shows. false when n is out of range. */
typedef struct { char idx[12]; bool scene; ru_dot_state_t dot; char sub[24]; } ru_dzapp_row_info_t;
bool ru_dzapp_row_info(const ru_dzapp_t *a, size_t n, ru_dzapp_row_info_t *out);
/* The confirm screen in progress: the verb ("ACTIVATE", "SWITCH OFF"...) and the command it will send. */
const char *ru_dzapp_confirm_verb(const ru_dzapp_t *a);
dz_cmd_kind_t ru_dzapp_confirm_kind(const ru_dzapp_t *a);
/* Energy shows one meter at a time; a tap on the gauge cycles. This picks the n-th meter (tests, driver). Other sections
 * ignore it. */
void ru_dzapp_select(ru_dzapp_t *a, ru_section_t s, int n);
/* Test hook: choose the slot-th mode in the open mode device's selector exactly as the wheel would. The thermostat mode has no
 * MCP tool, so this ends in a refusal (view only), never a hold. */
void ru_dzapp_pick_mode(ru_dzapp_t *a, int slot);
/* Test hook: set the open thermostat's dial to v and release it (opens the hold, or a refusal when read-only). */
void ru_dzapp_pick_setpoint(ru_dzapp_t *a, float v);
