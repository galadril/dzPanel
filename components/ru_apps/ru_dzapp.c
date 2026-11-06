#include "ru_dzapp.h"

#if __has_include("sdkconfig.h")
#include "sdkconfig.h" /* CONFIG_RU_HEAP_LOG: refresh counters (device builds only) */
#endif

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dz_color.h"
#include "ru_climate.h"
#include "ru_climate_ui.h"
#include "ru_wx.h"
#include "ru_osal.h"

#define MAX_ROWS 8 /* LVGL objects per screen build: one page of tiles or rows (6 or 4), or the 5 settings rows */
#define GRID_COLS 3
#define GRID_ROWS 2
#define PAGE_TILES (GRID_COLS * GRID_ROWS)
#define PAGE_ROWS 4
#define MAX_NAMES 12
#define NAME_LEN 20
#define INTERACT_MS 2500
#define STALE_EPOCH_MS 1500 /* stale states are re-evaluated at most this often */
#define REBUILD_MIN_MS 2000 /* background rebuilds (list changed by the server) at most this often */

typedef struct { dz_cmd_req_t req; char idx[12]; bool scene; } req_msg_t;
typedef struct {
  bool refused;
  dz_cmd_err_t err;
  dz_cmd_result_t r;
} res_msg_t;
typedef struct { struct ru_dzapp *app; char idx[12]; bool scene; int setting; } row_t;
/* one entry of the index list of a screen: sort key (group, server order) and the dz_model entry index */
typedef struct { uint32_t key; uint16_t i; uint8_t st; } ix_t; /* st: ru_val_state_t at collection time */

struct ru_dzapp {
  dz_model_t *model;
  dz_client_t *client;
  dz_cmd_t cmd;
  bool allow_control;
  char board[24]; /* board id for Settings > About; empty = unknown */
  bool square;    /* panel shape: false = round glass */

  ru_queue_t *req_q, *res_q;
  ru_task_t *net;
  atomic_bool net_stop;
  atomic_int conn; /* dz_conn_state_t published by the network thread */

  ru_view_t view;
  ru_section_t section;
  ru_sub_t sub;
  char cur_idx[12];
  bool cur_scene;
  /* confirmation in progress, and where to go back to */
  req_msg_t pend;
  char pend_title[40], pend_state[16], pend_verb[24];
  ru_view_t ret_view;

  ru_fb_t fb;
  lv_obj_t *scr, *list;
  lv_timer_t *timer;
  uint64_t sig; /* signature of the STRUCTURE on screen (what a refresh cannot change in place); 0 = rebuild now */
  uint64_t ctl; /* cheap control signature (controls, model generation, units) at the last evaluation */
  uint32_t epoch; /* stale epoch (time / STALE_EPOCH_MS) at the last evaluation */
  uint64_t stale_sig;
  uint64_t imm; /* view, section, page, connection, feedback at the last build: a change is never rate limited */
  uint32_t last_build;
  lv_obj_t *rim; /* status rim of the grid / rows screen (and of the light detail), updated in place */
  lv_obj_t *light;      /* the ru_light widget of the open detail page, NULL on every other screen: it is updated in place */
  uint32_t newest_id;   /* backend: the id of the newest light command; results of older ones are ignored */
  bool detail_thermo;
  dz_cap_t detail_cap;  /* capability of the open detail page (swipe zones) */
  uint32_t copies_base;
#if CONFIG_RU_HEAP_LOG
  ru_dzapp_stats_t stats;
#endif
  uint32_t now, interact_until;
  char banner[56];

  int energy_sel;
  int page, pages;      /* the paged list on screen: current page (kept over detail and back) and page count */
  ix_t *ix;             /* index list of the current section, rebuilt for each screen build (one pass over the model) */
  size_t ix_n, ix_cap;
  int press_x, press_y; /* where the finger went down on a tile: a drag that ends on it is a swipe, not a tap */
  int brightness;
  void (*bright_fn)(int, void *);
  void *bright_ud;
  void (*spy_fn)(const dz_cmd_req_t *, void *);
  void *spy_ud;
  const ru_dzapp_backend_t *backend; /* device: dz_service owns client, model and commands; NULL = own net step */
  void *backend_ud;

  row_t rows[MAX_ROWS];
  lv_obj_t *row_tiles[MAX_ROWS];
  size_t nrows;

  char names[MAX_NAMES][NAME_LEN];
  const char *name_ptrs[MAX_NAMES];
};

static const char *const k_sec_names[RU_SEC_COUNT] = { "FAVOURITES", "LIGHTS", "SCENES", "TEMPERATURE", "WEATHER", "UTILITY",
                                                       "ENERGY", "SETTINGS" };
const char *ru_section_name(ru_section_t s) { return s < RU_SEC_COUNT ? k_sec_names[s] : "?"; }

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
  const unsigned char *b = p;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
  return h;
}

/* ------------------------------------------------------------------ small helpers */

static lv_obj_t *label_u(lv_obj_t *scr, const char *txt, const lv_font_t *f, lv_color_t c, float dx_u, float dy_u) {
  int D = ru_theme_geom()->diameter;
  return ru_label_new(scr, txt, f, c, D / 2 + ru_u(dx_u), D / 2 + ru_u(dy_u));
}

static dz_conn_state_t conn(const ru_dzapp_t *a) {
  if (a->backend && a->backend->conn) return a->backend->conn(a->backend_ud);
  return (dz_conn_state_t)atomic_load(&((ru_dzapp_t *)a)->conn);
}

static ru_rim_state_t rim_for(dz_conn_state_t c, bool any_stale) {
  switch (c) {
    case DZ_CS_OK: return any_stale ? RU_RIM_STALE : RU_RIM_OK;
    case DZ_CS_CONNECTING: return RU_RIM_STALE;
    case DZ_CS_OFFLINE: return RU_RIM_OFFLINE;
    default: return RU_RIM_ERROR;
  }
}

static const char *banner_for(dz_conn_state_t c) {
  switch (c) {
    case DZ_CS_OK: return "";
    case DZ_CS_CONNECTING: return "CONNECTING...";
    case DZ_CS_OFFLINE: return "OFFLINE";
    case DZ_CS_TOKEN_REJECTED: return "TOKEN REJECTED";
    case DZ_CS_SERVER_ERROR: return "SERVER ERROR";
    case DZ_CS_BAD_RESPONSE: return "NOT DOMOTICZ?";
  }
  return "";
}

static bool get_entry(ru_dzapp_t *a, const char *idx, bool scene, dz_device_t *d, ru_val_t *v, ru_val_state_t *st) {
  return dz_model_get(a->model, idx, scene, d, v, st, a->now);
}

static void get_units(ru_dzapp_t *a, dz_units_t *u) { dz_model_units(a->model, u); }

/* Home group order = the Domoticz dashboard: scenes and groups, lights, temperature, weather, utility, the rest. */
static unsigned home_group(const dz_device_t *d, bool scene) {
  if (scene) return 0;
  if (d->tabs & DZ_TAB_LIGHT) return 1;
  if (d->tabs & DZ_TAB_TEMP) return 2;
  if (d->tabs & DZ_TAB_WEATHER) return 3;
  if (d->tabs & DZ_TAB_UTILITY) return 4;
  return 5;
}

static bool sec_match(ru_section_t s, const dz_device_t *d, bool scene) {
  switch (s) {
    case RU_SEC_HOME: return d->favorite;
    case RU_SEC_LIGHTS: return !scene && (d->tabs & DZ_TAB_LIGHT);
    case RU_SEC_SCENES: return scene;
    case RU_SEC_TEMP: return !scene && (d->tabs & DZ_TAB_TEMP);
    case RU_SEC_WEATHER: return !scene && (d->tabs & DZ_TAB_WEATHER);
    case RU_SEC_UTILITY: return !scene && (d->tabs & DZ_TAB_UTILITY);
    case RU_SEC_ENERGY: return !scene && d->cap == DZ_CAP_ENERGY && (d->has_usage || d->has_usage_deliv || d->has_today);
    default: return false;
  }
}

static int ix_cmp(const void *x, const void *y) {
  const ix_t *p = x, *q = y;
  if (p->key != q->key) return p->key < q->key ? -1 : 1;
  return p->i < q->i ? -1 : p->i > q->i; /* equal server order (new by delta, 0xFFFF): model order */
}

/* ONE pass over the model under ONE lock, no copies (dz_model_foreach): the entries of a section in a->ix with their
 * display key and state. */
typedef struct { ru_dzapp_t *a; ru_section_t s; bool full; } collect_t;

static bool collect_cb(size_t i, const dz_device_t *d, bool scene, ru_val_state_t st, uint16_t order, void *ud) {
  collect_t *c = ud;
  ru_dzapp_t *a = c->a;
  if (i >= 0xFFFF || !sec_match(c->s, d, scene)) return true;
  if (a->ix_n >= a->ix_cap) { c->full = true; return false; }
  a->ix[a->ix_n].key = c->s == RU_SEC_HOME ? (home_group(d, scene) << 16) | order : order;
  a->ix[a->ix_n].i = (uint16_t)i;
  a->ix[a->ix_n].st = (uint8_t)st;
  a->ix_n++;
  return true;
}

/* The entries of a section in display order (a->ix, a->ix_n). Returns the count. */
static size_t build_ix(ru_dzapp_t *a, ru_section_t s) {
  for (int tries = 0; tries < 3; tries++) {
    size_t total = dz_model_count(a->model) + 8 * (size_t)tries;
    a->ix_n = 0;
    if (total > a->ix_cap) {
      ix_t *n = realloc(a->ix, total * sizeof *n);
      if (!n) return 0;
      a->ix = n;
      a->ix_cap = total;
    }
    collect_t c = { a, s, false };
    dz_model_foreach(a->model, a->now, collect_cb, &c);
    if (!c.full) break; /* the model grew between count and pass: once more with room */
  }
  if (a->ix_n > 1) qsort(a->ix, a->ix_n, sizeof *a->ix, ix_cmp);
  return a->ix_n;
}

/* the k-th entry of the index list */
static bool ix_get(ru_dzapp_t *a, size_t k, dz_device_t *d, ru_val_t *v, ru_val_state_t *st, bool *scene) {
  ru_val_t vv;
  ru_val_state_t ss;
  bool sc;
  return k < a->ix_n && dz_model_at(a->model, a->ix[k].i, d, v ? v : &vv, st ? st : &ss, scene ? scene : &sc, a->now);
}

static bool ix_any_stale(const ru_dzapp_t *a) {
  for (size_t k = 0; k < a->ix_n; k++) if (a->ix[k].st == RU_VAL_STALE) return true;
  return false;
}

/* Which sections have something to show: one pass, no copies, one bit each. Home and Settings are always there. */
static bool avail_cb(size_t i, const dz_device_t *d, bool scene, ru_val_state_t st, uint16_t order, void *ud) {
  (void)i; (void)st; (void)order;
  unsigned *m = ud;
  for (int s = RU_SEC_LIGHTS; s < RU_SEC_SETTINGS; s++) if (sec_match((ru_section_t)s, d, scene)) *m |= 1u << s;
  return true;
}

static unsigned avail_mask(ru_dzapp_t *a) {
  unsigned m = (1u << RU_SEC_HOME) | (1u << RU_SEC_SETTINGS);
  dz_model_foreach(a->model, a->now, avail_cb, &m);
  return m;
}

bool ru_dzapp_section_available(ru_dzapp_t *a, ru_section_t s) {
  if (s == RU_SEC_HOME || s == RU_SEC_SETTINGS) return true;
  return s < RU_SEC_COUNT && (avail_mask(a) >> s & 1u);
}

/* ------------------------------------------------------------------ commands (UI side: queue only) */

static void build_and_show(ru_dzapp_t *a);
static uint64_t screen_sig(ru_dzapp_t *a);

static void post_cmd(ru_dzapp_t *a, const req_msg_t *m) {
  if (a->spy_fn) {
    dz_cmd_req_t r = m->req;
    r.idx = m->idx;
    a->spy_fn(&r, a->spy_ud);
  }
  if (a->backend) {
    dz_cmd_req_t r = m->req;
    r.idx = m->idx;
    uint32_t id = a->backend->submit(&r, a->backend_ud);
    if (!id) { ru_fb_on_refused(&a->fb, DZ_CMDE_QUEUE_FULL, a->now); return; }
    if (r.kind == DZ_CMD_SET_LEVEL || dz_cmd_is_colour(r.kind)) a->newest_id = id;
  } else if (ru_queue_send(a->req_q, m, 0) != RU_OK) { ru_fb_on_refused(&a->fb, DZ_CMDE_QUEUE_FULL, a->now); return; }
  ru_fb_begin(&a->fb, m->idx, m->scene, a->now);
}

static const char *verb_for(dz_cmd_kind_t kind) {
  static const struct { dz_cmd_kind_t k; const char *t; } names[] = {
    { DZ_CMD_ON, "ON" }, { DZ_CMD_OFF, "OFF" }, { DZ_CMD_TOGGLE, "TOGGLE" }, { DZ_CMD_SCENE_ON, "SWITCH ON" },
    { DZ_CMD_SCENE_OFF, "SWITCH OFF" }, { DZ_CMD_SETPOINT, "SET" }, { DZ_CMD_SET_LEVEL, "SET" }, { DZ_CMD_SELECTOR, "SET" },
    { DZ_CMD_BLINDS_OPEN, "OPEN" }, { DZ_CMD_BLINDS_CLOSE, "CLOSE" }, { DZ_CMD_BLINDS_STOP, "STOP" }, { DZ_CMD_PUSH, "PUSH" },
    { DZ_CMD_MODE, "SET MODE" } };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (names[i].k == kind) return names[i].t;
  return "CONFIRM";
}

static void start_confirm(ru_dzapp_t *a, const dz_device_t *d, const req_msg_t *m, const char *verb, const char *state) {
  a->pend = *m;
  ru_dzv_clamp(a->pend_title, sizeof a->pend_title, d->name, 18);
  ru_dzv_upper(a->pend_title, sizeof a->pend_title, a->pend_title);
  snprintf(a->pend_state, sizeof a->pend_state, "%s", state);
  snprintf(a->pend_verb, sizeof a->pend_verb, "%s", verb);
  if (a->view != RU_VIEW_CONFIRM) a->ret_view = a->view;
  a->view = RU_VIEW_CONFIRM;
  build_and_show(a);
}

/* Shared gate for every command: control allowed, device known, not locked out, data fresh. Then SAFE commands go
 * out at once and CONFIRM commands lead to the hold screen. verb/state are only for the hold screen. */
static void refused_for(ru_dzapp_t *a, const char *idx, bool scene, dz_cmd_err_t e) {
  ru_fb_on_refused(&a->fb, e, a->now);
  snprintf(a->fb.idx, sizeof a->fb.idx, "%s", idx); /* so a detail page shows it only on the device it is about */
  a->fb.scene = scene;
}

static void submit_or_confirm_h(ru_dzapp_t *a, const char *idx, bool scene, dz_cmd_kind_t kind, int level, float sp, int hue,
                                const char *verb, bool show_state) {
  dz_device_t d;
  ru_val_t v;
  ru_val_state_t st;
  if (!get_entry(a, idx, scene, &d, &v, &st)) { refused_for(a, idx, scene, DZ_CMDE_UNKNOWN_DEVICE); return; }
  if (!a->allow_control) { refused_for(a, idx, scene, DZ_CMDE_CONTROL_DISABLED); return; }
  dz_units_t un;
  get_units(a, &un);
  dz_risk_t risk = dz_cmd_risk(&d, kind, &un);
  if (risk == DZ_RISK_LOCKED) {
    bool nomcp = kind == DZ_CMD_MODE || (kind == DZ_CMD_SETPOINT && d.cap == DZ_CAP_THERMOSTAT && !dz_cmd_supported(&d, kind, &un));
    refused_for(a, idx, scene, d.is_protected ? DZ_CMDE_PROTECTED : nomcp ? DZ_CMDE_UNSUPPORTED : DZ_CMDE_NOT_CONTROLLABLE);
    return;
  }
  if (st != RU_VAL_OK) { refused_for(a, idx, scene, DZ_CMDE_STALE); return; } /* do not make the user hold for nothing */
  req_msg_t m;
  memset(&m, 0, sizeof m);
  m.req.kind = kind;
  m.req.level = level;
  m.req.setpoint = sp;
  m.req.hue = (int16_t)hue;
  m.scene = scene;
  snprintf(m.idx, sizeof m.idx, "%s", idx);
  if (risk == DZ_RISK_CONFIRM) {
    char state[16] = "";
    if (show_state && d.cap == DZ_CAP_GROUP) snprintf(state, sizeof state, "%s", ru_dzv_tile_label(ru_dzv_tile_state(&d))); /* a Scene has no state to show */
    start_confirm(a, &d, &m, verb ? verb : verb_for(kind), state);
    if (scene) a->ret_view = RU_VIEW_HOME; /* scenes and groups live on a section screen, never on a detail */
    return;
  }
  post_cmd(a, &m);
}

static void submit_or_confirm(ru_dzapp_t *a, const char *idx, bool scene, dz_cmd_kind_t kind, int level, float sp,
                              const char *verb, bool show_state) {
  submit_or_confirm_h(a, idx, scene, kind, level, sp, 0, verb, show_state);
}

static void request_cmd(ru_dzapp_t *a, dz_cmd_kind_t kind, int level, float sp) {
  submit_or_confirm(a, a->cur_idx, a->cur_scene, kind, level, sp, NULL, false);
  a->sig = 0; /* redraw from the model, never from what the widget was asked to show */
}

/* A tap on a scene or group tile: Scene = push (never Off). Group = Off when fully On, else On. */
static void scene_tap(ru_dzapp_t *a, const char *idx) {
  dz_device_t d;
  ru_val_t v;
  ru_val_state_t st;
  dz_cmd_kind_t kind;
  const char *verb;
  if (!get_entry(a, idx, true, &d, &v, &st) || !ru_dzv_scene_tap(&d, &kind, &verb)) {
    ru_fb_on_refused(&a->fb, DZ_CMDE_UNKNOWN_DEVICE, a->now);
    return;
  }
  submit_or_confirm(a, idx, true, kind, 0, 0, verb, true);
}

/* ------------------------------------------------------------------ result handling */

static void drain_results(ru_dzapp_t *a) {
  res_msg_t r;
  while (ru_queue_recv(a->res_q, &r, 0) == RU_OK) {
    if (r.refused) ru_fb_on_refused(&a->fb, r.err, a->now);
    else if (a->newest_id && (r.r.kind == DZ_CMD_SET_LEVEL || dz_cmd_is_colour(r.r.kind)) && (int32_t)(r.r.id - a->newest_id) < 0) {
      /* superseded by a newer light command: the UI follows the newest id only */
    } else {
      /* Backend events carry no device state: for an UNCONFIRMED outcome read it from the model (dz_cmd stored the
       * re-read there), so a Group that stayed Mixed still says "STILL MIXED". */
      if (a->backend && r.r.outcome == DZ_OUT_UNCONFIRMED && !r.r.have_after && a->fb.idx[0]) {
        ru_val_t v;
        ru_val_state_t st;
        if (dz_model_get(a->model, a->fb.idx, a->fb.scene, &r.r.after, &v, &st, a->now)) r.r.have_after = true;
      }
      ru_fb_on_result(&a->fb, &r.r, a->now);
    }
  }
}

void ru_dzapp_set_display_info(ru_dzapp_t *a, const char *board_id, bool round) {
  snprintf(a->board, sizeof a->board, "%s", board_id ? board_id : "");
  a->square = !round;
}
void ru_dzapp_set_backend(ru_dzapp_t *a, const ru_dzapp_backend_t *b, void *ud) { a->backend = b; a->backend_ud = ud; }

void ru_dzapp_post_cmd_event(ru_dzapp_t *a, uint32_t req_id, dz_outcome_t outcome, dz_fail_t fail, dz_cmd_kind_t kind,
                             dz_cmd_err_t reject, int level) {
  res_msg_t m;
  memset(&m, 0, sizeof m);
  if (reject != DZ_CMD_ACCEPTED) {
    m.refused = true;
    m.err = reject;
  } else {
    m.r.id = req_id;
    m.r.kind = kind;
    m.r.outcome = outcome;
    m.r.fail = fail;
    m.r.level_sent = level;
  }
  ru_queue_send(a->res_q, &m, 0);
}

void ru_dzapp_inject_result(ru_dzapp_t *a, const dz_cmd_result_t *r) { ru_fb_on_result(&a->fb, r, a->now); a->sig = 0; }

static lv_color_t fb_color(const ru_fb_t *f) {
  const ru_theme_t *t = ru_theme();
  switch (f->st) {
    case RU_FB_CONFIRMED: return t->ok;
    case RU_FB_UNCONFIRMED: return t->warn;
    case RU_FB_FAILED: return t->error;
    default: return t->text_dim;
  }
}

/* The shared feedback line: pending (dim), confirmed (ok), unconfirmed (warn), failed (error). */
static void fb_label(ru_dzapp_t *a, lv_obj_t *scr, float dy_u) {
  const char *txt = ru_fb_text(&a->fb);
  if (!txt[0]) return;
  if (a->view == RU_VIEW_DETAIL && strcmp(a->fb.idx, a->cur_idx) != 0) return; /* feedback of another device */
  label_u(scr, txt, ru_font_small(), fb_color(&a->fb), 0, dy_u);
}

/* ------------------------------------------------------------------ navigation chrome */

/* The one way back, for the on-screen controls, the confirm hold and the shell's BACK gesture. false = nowhere to go
 * (already on Home). Confirm returns to where it came from; a detail or sub screen to its section (a sub screen to
 * Settings); a section to Home. */
bool ru_dzapp_back(ru_dzapp_t *a) {
  switch (a->view) {
    case RU_VIEW_CONFIRM:
      if (a->ret_view == RU_VIEW_DETAIL || a->ret_view == RU_VIEW_SUB) { a->view = a->ret_view; build_and_show(a); }
      else ru_dzapp_show_section(a, a->section);
      return true;
    case RU_VIEW_DETAIL: ru_dzapp_show_section(a, a->section); return true;
    case RU_VIEW_SUB: ru_dzapp_show_section(a, RU_SEC_SETTINGS); return true;
    default: break;
  }
  if (a->section == RU_SEC_HOME) return false;
  ru_dzapp_home(a);
  return true;
}

/* A swipe that starts on the corner chevron can fire the shell's BACK (rim sector) and still end inside the chevron:
 * LVGL then delivers CLICKED to the OLD screen's chevron, which show() frees only asynchronously. Taking that click
 * would step back a second time (detail -> section -> Home). So a click only counts when its target lives on the screen
 * that is shown now (a stale screen is not a->scr). */
static void cb_back(lv_event_t *e) {
  ru_dzapp_t *a = lv_event_get_user_data(e);
  if (lv_obj_get_screen((lv_obj_t *)lv_event_get_target(e)) != a->scr) return;
  ru_dzapp_back(a);
}

#define BACK_CORNER_PX 56
#define BACK_CORNER_POS 8

bool ru_dzapp_back_rect(const ru_dzapp_t *a, lv_area_t *r) {
  if (!a->square || a->view == RU_VIEW_HOME) return false;
  if (r) {
    r->x1 = BACK_CORNER_POS;
    r->y1 = BACK_CORNER_POS;
    r->x2 = BACK_CORNER_POS + BACK_CORNER_PX - 1;
    r->y2 = BACK_CORNER_POS + BACK_CORNER_PX - 1;
  }
  return true;
}

/* A '<' drawn with lv_line (no glyph: the font subset need not have one). The points live with the object. */
static void chev_free(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }
static lv_obj_t *chevron(lv_obj_t *parent, int cw, int ch, lv_color_t c, int x, int y) {
  lv_point_precise_t *pt = lv_malloc(3 * sizeof *pt);
  pt[0] = (lv_point_precise_t){ (lv_value_precise_t)cw, 0 };
  pt[1] = (lv_point_precise_t){ 0, (lv_value_precise_t)(ch / 2) };
  pt[2] = (lv_point_precise_t){ (lv_value_precise_t)cw, (lv_value_precise_t)ch };
  lv_obj_t *l = lv_line_create(parent);
  lv_line_set_points(l, pt, 3);
  lv_obj_add_event_cb(l, chev_free, LV_EVENT_DELETE, pt);
  lv_obj_set_style_line_width(l, 2, 0);
  lv_obj_set_style_line_color(l, c, 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(l, x, y);
  return l;
}

static lv_obj_t *outlined(lv_obj_t *scr, int w, int h, int radius) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *b = lv_obj_create(scr);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, w, h);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(b, radius, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, ru_dim(t->text_dim, 150), 0);
  lv_obj_set_style_border_color(b, t->accent, LV_STATE_PRESSED);
  return b;
}

/* The visible back control of every detail, sub and confirm screen: an outlined pill "< LABEL" dy pixels from the
 * centre. On a square panel there is also a corner chevron at the top left, outside the inscribed circle. */
static lv_obj_t *nav_back_f(ru_dzapp_t *a, lv_obj_t *scr, const char *label, int dy, const lv_font_t *f) {
  const ru_theme_t *t = ru_theme();
  int cw = ru_u(1.5f), ch = ru_u(3.0f), pad = ru_u(2.6f), gap = ru_u(1.6f);
  int h = ru_u(9);
  lv_obj_t *b = outlined(scr, LV_SIZE_CONTENT, h, LV_RADIUS_CIRCLE);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(b, pad, 0);
  lv_obj_set_style_pad_column(b, gap, 0);
  lv_obj_align(b, LV_ALIGN_CENTER, 0, dy);
  chevron(b, cw, ch, t->text_dim, 0, 0);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_max_width(l, ru_u(34), 0);
  lv_obj_set_height(l, lv_font_get_line_height(f)); /* one line: a name that is too long ends in dots */
  lv_label_set_text(l, label);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, t->text_dim, 0);
  lv_obj_add_event_cb(b, cb_back, LV_EVENT_CLICKED, a);
  lv_area_t r;
  if (ru_dzapp_back_rect(a, &r)) {
    lv_obj_t *c = outlined(scr, BACK_CORNER_PX, BACK_CORNER_PX, ru_u(3));
    lv_obj_set_pos(c, r.x1, r.y1);
    lv_obj_set_ext_click_area(c, 4);
    chevron(c, ru_u(2.4f), ru_u(5.0f), t->text_dim, (BACK_CORNER_PX - ru_u(2.4f)) / 2 - 1, (BACK_CORNER_PX - ru_u(5.0f)) / 2 - 1);
    lv_obj_add_event_cb(c, cb_back, LV_EVENT_CLICKED, a);
  }
  return b;
}
static lv_obj_t *nav_back(ru_dzapp_t *a, lv_obj_t *scr, const char *label, int dy) { return nav_back_f(a, scr, label, dy, ru_font_small()); }

static void back_button(ru_dzapp_t *a, lv_obj_t *scr, const char *text) { nav_back(a, scr, text, ru_u(42)); }

/* Dial screens use the whole face: the pill reads "< DEVICE NAME" where the name sits (the shell's edge swipe is the other way back). */
static void name_back(ru_dzapp_t *a, lv_obj_t *scr, const char *text, float dy_u) { nav_back_f(a, scr, text, ru_u(dy_u), ru_font_body()); }

/* One dot per available section, the current one lit. Not colour alone: the current dot is also larger. */
static void pager_dots(ru_dzapp_t *a, lv_obj_t *scr) {
  const ru_theme_t *t = ru_theme();
  int n = 0;
  ru_section_t avail[RU_SEC_COUNT];
  unsigned mask = avail_mask(a);
  for (int s = 0; s < RU_SEC_COUNT; s++) if (mask >> s & 1u) avail[n++] = (ru_section_t)s;
  float step = 3.0f, x0 = -(float)(n - 1) * step / 2;
  for (int i = 0; i < n; i++) {
    bool cur = avail[i] == a->section;
    lv_obj_t *d = ru_icon_dot(scr, ru_u(cur ? 1.6f : 1.1f), cur ? t->accent : ru_dim(t->text_dim, 120));
    lv_obj_align(d, LV_ALIGN_CENTER, ru_u(x0 + (float)i * step), ru_u(43.5f));
  }
}

static lv_obj_t *section_screen(ru_dzapp_t *a, bool any_stale) {
  lv_obj_t *s = ru_screen_new();
  lv_obj_t *rim = ru_status_rim_create(s);
  ru_status_rim_set_state(rim, rim_for(conn(a), any_stale));
  a->rim = rim;
  a->list = NULL;
  a->nrows = 0;
  a->pages = 1;
  return s;
}

static void title(lv_obj_t *s, const char *txt) { label_u(s, txt, ru_font_title(), ru_theme()->text_dim, 0, -40); }

/* ------------------------------------------------------------------ tiles */

static const char *cap_label(dz_cap_t c) {
  switch (c) {
    case DZ_CAP_TEMP: return "TEMP";
    case DZ_CAP_WEATHER: return "WEATHER";
    case DZ_CAP_ENERGY: return "ENERGY";
    case DZ_CAP_THERMOSTAT: return "SETPOINT";
    case DZ_CAP_BLINDS: return "BLINDS";
    case DZ_CAP_SELECTOR: return "SELECTOR";
    case DZ_CAP_STATUS: return "STATUS";
    case DZ_CAP_LOCK: return "LOCK";
    case DZ_CAP_SENSOR: return "SENSOR";
    case DZ_CAP_MODE: return "MODE";
    case DZ_CAP_READONLY:
    case DZ_CAP_ONOFF:
    case DZ_CAP_DIMMER:
    case DZ_CAP_PUSH:
    case DZ_CAP_SCENE:
    case DZ_CAP_GROUP: return "INFO";
  }
  return "INFO";
}

static ru_dot_state_t dot_of(ru_tile_state_t s) {
  switch (s) {
    case RU_TILE_ON: return RU_DOT_ON;
    case RU_TILE_OFF: return RU_DOT_OFF;
    case RU_TILE_MIXED: return RU_DOT_MIXED;
    case RU_TILE_PUSH: return RU_DOT_PUSH;
    default: return RU_DOT_UNKNOWN;
  }
}

/* ---- Temperature and Weather tiles (NSPanel style): icon, big value, unit and trend, one small line. What a device shows comes
 * from ITS OWN readings only (ru_wx.c: one case per kind: temperature/humidity, wind, rain, UV, air, barometer). The widget
 * is ru_wx_tile_*; a band colours the icon, the value stays white. */
/* every tile of the Temperature and Weather pages, and a temperature or weather device wherever else it is a tile (favourites) */
static bool is_wx(const ru_dzapp_t *a, const dz_device_t *d) {
  if (d->cap == DZ_CAP_SCENE || d->cap == DZ_CAP_GROUP) return false;
  return d->cap == DZ_CAP_TEMP || d->cap == DZ_CAP_WEATHER || (a->view == RU_VIEW_HOME && (a->section == RU_SEC_TEMP || a->section == RU_SEC_WEATHER));
}

static void wx_content(const dz_device_t *d, const dz_units_t *un, ru_wx_tile_t *t) {
  ru_cl_units_t u;
  ru_cl_units(un, &u);
  ru_wx_tile_build(d, &u, t);
  if (d->n_rd == 0 && !(d->has_baro && isfinite(d->baro))) { /* a hand-made device: the text of its Data field, no icon */
    char v[40];
    bool known;
    ru_dzv_value_text_u(d, un, v, sizeof v, &known);
    ru_dzv_clamp(t->value, sizeof t->value, v, sizeof t->value - 3);
    t->known = known;
  }
}

static lv_color_t wx_color(ru_wx_color_t c, lv_color_t none) { return c == RU_WX_NONE ? none : lv_color_hex(ru_wx_color_rgb(c)); }

static void wx_tile_update(ru_dzapp_t *a, lv_obj_t *tile, const dz_device_t *d, ru_val_state_t st) {
  const ru_theme_t *th = ru_theme();
  dz_units_t un;
  get_units(a, &un);
  ru_wx_tile_t t;
  wx_content(d, &un, &t);
  char nm[40];
  ru_dzv_upper(nm, sizeof nm, t.name); /* the whole app is upper case; the widget cuts a long name with dots */
  char icon[4];
  ru_wx_icon_utf8(t.icon, icon);
  ru_wxtile_data_t w = { nm, icon, wx_color(t.icon_color, th->text), t.value, t.unit, t.second, wx_color(t.second_color, th->text_dim),
                         t.known ? st : RU_VAL_UNKNOWN };
  ru_wx_tile_set(tile, &w);
}

/* The tile content of any entry: an indicator for things with a state, a value for sensors. */
static void tile_content(const ru_dzapp_t *a, const dz_device_t *d, bool scene, ru_dot_state_t *dot, char *sub, size_t n,
                         char *val, size_t vn) {
  bool known;
  dz_units_t un;
  get_units((ru_dzapp_t *)a, &un);
  (void)scene;
  val[0] = 0;
  if (is_wx(a, d)) { /* for row_info (tests, driver): the tile's value text and small line */
    ru_wx_tile_t t;
    *dot = RU_DOT_NONE;
    wx_content(d, &un, &t);
    snprintf(val, vn, "%s%s", t.value, t.unit);
    snprintf(sub, n, "%s", t.second);
    return;
  }
  switch (d->cap) {
    case DZ_CAP_SCENE: *dot = RU_DOT_PUSH; snprintf(sub, n, "SCENE"); return;
    case DZ_CAP_GROUP: *dot = dot_of(ru_dzv_tile_state(d)); snprintf(sub, n, "%s", ru_dzv_tile_label(ru_dzv_tile_state(d))); return;
    case DZ_CAP_PUSH: *dot = RU_DOT_PUSH; snprintf(sub, n, "PUSH"); return;
    case DZ_CAP_ONOFF:
    case DZ_CAP_DIMMER:
    case DZ_CAP_LOCK:
      *dot = dot_of(ru_dzv_tile_state(d));
      ru_dzv_value_text_u(d, &un, sub, n, &known);
      return;
    case DZ_CAP_TEMP:
    case DZ_CAP_WEATHER: /* handled above */
    case DZ_CAP_READONLY:
    case DZ_CAP_BLINDS:
    case DZ_CAP_SELECTOR:
    case DZ_CAP_STATUS:
    case DZ_CAP_THERMOSTAT:
    case DZ_CAP_ENERGY:
    case DZ_CAP_SENSOR:
    case DZ_CAP_MODE:
      *dot = RU_DOT_NONE;
      ru_dzv_value_text_u(d, &un, val, vn, &known);
      snprintf(sub, n, "%s", cap_label(d->cap));
      return;
  }
}

/* A tap counts only on the screen that is shown (a swipe that changed section can still end as a click on the old
 * one, which is freed asynchronously) and only if the finger stayed put: a drag that happens to end on the tile it
 * started on is a swipe, handled by ru_dzapp_swipe. */
static bool tap_valid(ru_dzapp_t *a, lv_obj_t *w) {
  if (lv_obj_get_screen(w) != a->scr) return false;
  lv_indev_t *in = lv_indev_active();
  if (!in) return true; /* a test hook, not a finger */
  lv_point_t p;
  lv_indev_get_point(in, &p);
  int dx = p.x - a->press_x, dy = p.y - a->press_y, slop = ru_u(2.2f);
  return dx * dx + dy * dy <= slop * slop;
}

static void cb_press(lv_event_t *e) {
  ru_dzapp_t *a = lv_event_get_user_data(e);
  lv_indev_t *in = lv_indev_active();
  lv_point_t p = { 0, 0 };
  if (in) lv_indev_get_point(in, &p);
  a->press_x = p.x;
  a->press_y = p.y;
}

static void cb_row(lv_obj_t *w, void *ud) {
  row_t *r = ud;
  ru_dzapp_t *a = r->app;
  if (!tap_valid(a, w)) return;
  ru_dzapp_open(a, r->idx, r->scene); /* a scene or group goes to its hold screen, a device to its detail */
}

/* The content of a state tile from the model entry: the one place that sets it, on creation and on every refresh. The
 * tile setters compare with what they show and touch nothing that did not change. */
static void entry_tile_update(ru_dzapp_t *a, lv_obj_t *tile, const dz_device_t *d, bool scene, ru_val_state_t st) {
  if (is_wx(a, d)) { wx_tile_update(a, tile, d, st); return; }
  char sub[24], val[24];
  ru_dot_state_t dot;
  tile_content(a, d, scene, &dot, sub, sizeof sub, val, sizeof val);
  /* "activated" feedback on a Scene tile: shown only while its command is CONFIRMED (a Scene keeps no state) */
  if (d->cap == DZ_CAP_SCENE && a->fb.st == RU_FB_CONFIRMED && a->fb.scene && !strcmp(a->fb.idx, d->idx)) snprintf(sub, sizeof sub, "ACTIVATED");
  ru_state_tile_set(tile, dot, sub, dot == RU_DOT_NONE && !val[0] ? RU_VAL_UNKNOWN : st);
  if (dot == RU_DOT_NONE) ru_state_tile_set_value(tile, val);
}

/* Add one state tile for an entry to `parent` and register it as row n. */
static lv_obj_t *add_entry_tile(ru_dzapp_t *a, lv_obj_t *parent, int w, int h, const dz_device_t *d, bool scene, ru_val_state_t st) {
  if (a->nrows >= MAX_ROWS) return NULL;
  row_t *r = &a->rows[a->nrows];
  r->app = a;
  r->scene = scene;
  r->setting = -1;
  snprintf(r->idx, sizeof r->idx, "%s", d->idx);
  char nm[40];
  ru_dzv_upper(nm, sizeof nm, d->name);
  ru_dzv_clamp(nm, sizeof nm, nm, 16);
  lv_obj_t *tile = is_wx(a, d) ? ru_wx_tile_create(parent, w, h) : ru_state_tile_create(parent, w, h, nm);
  entry_tile_update(a, tile, d, scene, st);
  if (is_wx(a, d)) ru_wx_tile_on_tap(tile, cb_row, r);
  else ru_state_tile_on_tap(tile, cb_row, r);
  lv_obj_add_event_cb(tile, cb_press, LV_EVENT_PRESSED, a);
  a->row_tiles[a->nrows++] = tile;
  return tile;
}

/* ------------------------------------------------------------------ section: home */

static float nice_max(float v) {
  if (!(v > 0.0f)) return 1.0f;
  float e = powf(10.0f, floorf(log10f(v)));
  float m = v / e;
  return (m <= 1.0f ? 1.0f : m <= 2.0f ? 2.0f : m <= 5.0f ? 5.0f : 10.0f) * e;
}

/* A gauge value for a number the view model parsed. Freshness comes from the model's verdict for the entry (OK, STALE,
 * UNKNOWN), never from whether the model chose to keep a reading of its own for that capability. */
static ru_val_t make_val(float x, ru_unit_t unit, ru_val_state_t st, uint32_t now) {
  if (st == RU_VAL_UNKNOWN) return ru_val_unknown();
  if (st == RU_VAL_STALE) return ru_val_make(x, unit, now - 7200000u, 60000u); /* two hours old, 1 min ttl: STALE */
  return ru_val_make(x, unit, now, 600000u);
}

/* A power value as a gauge value: W below 1000, kW from there, with the range a 1-2-5 step above it. */
static ru_val_t power_val(ru_val_state_t st, uint32_t now, float watts, float *max_out) {
  float aw = fabsf(watts), mx = nice_max(aw);
  if (mx < 1000.0f) mx = 1000.0f;
  if (aw >= 1000.0f) { *max_out = mx / 1000.0f; return make_val(watts / 1000.0f, RU_UNIT_KW, st, now); }
  *max_out = mx;
  return make_val(watts, RU_UNIT_W, st, now);
}

static lv_obj_t *list_box(lv_obj_t *s, float top_u, float bot_u, int *w_out) {
  const ru_geom_t *g = ru_theme_geom();
  float top = (float)g->diameter / 2 + top_u * g->u, bot = (float)g->diameter / 2 + bot_u * g->u;
  int w = (int)ru_geom_row_width(g, top, bot) - ru_u(2);
  lv_obj_t *list = lv_obj_create(s);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, w, (int)(bot - top));
  if (w_out) *w_out = w;
  lv_obj_align(list, LV_ALIGN_CENTER, 0, ru_u((top_u + bot_u) / 2));
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  return list;
}

static void conn_banner(ru_dzapp_t *a, lv_obj_t *s, dz_conn_state_t cs, float dy_u) {
  snprintf(a->banner, sizeof a->banner, "%s", banner_for(cs));
  if (a->banner[0]) label_u(s, a->banner, ru_font_small(), cs == DZ_CS_CONNECTING ? ru_theme()->text_dim : cs == DZ_CS_OFFLINE ? ru_theme()->warn : ru_theme()->error, 0, dy_u);
}

/* ---- paged lists ---- */

static void page_go(ru_dzapp_t *a, int p) {
  a->page = p;
  build_and_show(a);
  a->sig = screen_sig(a);
}

static void cb_pager(lv_event_t *e) {
  ru_dzapp_t *a = lv_event_get_user_data(e);
  if (lv_obj_get_screen((lv_obj_t *)lv_event_get_target(e)) != a->scr || a->pages < 2) return;
  page_go(a, (a->page + 1) % a->pages); /* the indicator is also a button: the next page, round to the first */
}

/* the 'n/N' page indicator under a list; a tap on it steps to the next page. Nothing for a single page. */
static void page_indicator(ru_dzapp_t *a, lv_obj_t *s, float dy_u) {
  if (a->pages < 2) return;
  char c[16];
  snprintf(c, sizeof c, "%d/%d", a->page + 1, a->pages);
  lv_obj_t *l = label_u(s, c, ru_font_small(), ru_theme()->text_dim, 0, dy_u);
  lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(l, ru_u(3));
  lv_obj_add_event_cb(l, cb_pager, LV_EVENT_CLICKED, a);
}

/* Fix the page for a list of n entries with per_page on a page. Returns the first entry of the page. */
static size_t page_setup(ru_dzapp_t *a, size_t n, int per_page) {
  a->pages = n ? (int)((n + (size_t)per_page - 1) / (size_t)per_page) : 1;
  if (a->page >= a->pages) a->page = a->pages - 1;
  if (a->page < 0) a->page = 0;
  return (size_t)a->page * (size_t)per_page;
}

/* The grid of state tiles: 3 columns x 2 rows. Chord at +-23u is 79u, the grid is 78u wide. */
static lv_obj_t *tile_grid(lv_obj_t *s, float cy_u) {
  int tw = ru_u(25), th = ru_u(21), gap = ru_u(1.5f);
  lv_obj_t *g = lv_obj_create(s);
  lv_obj_remove_style_all(g);
  lv_obj_set_size(g, GRID_COLS * tw + (GRID_COLS - 1) * gap, GRID_ROWS * th + (GRID_ROWS - 1) * gap);
  lv_obj_align(g, LV_ALIGN_CENTER, 0, ru_u(cy_u));
  lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(g, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START); /* a short last row is centred */
  lv_obj_set_style_pad_row(g, gap, 0);
  lv_obj_set_style_pad_column(g, gap, 0);
  return g;
}

static void grid_page(ru_dzapp_t *a, lv_obj_t *s) {
  size_t first = page_setup(a, a->ix_n, PAGE_TILES);
  if (a->ix_n == 0) return;
  lv_obj_t *g = tile_grid(s, -1.0f);
  a->list = g;
  for (size_t k = first; k < a->ix_n && k < first + PAGE_TILES; k++) {
    dz_device_t d;
    ru_val_state_t st;
    bool sc;
    if (!ix_get(a, k, &d, NULL, &st, &sc)) continue;
    add_entry_tile(a, g, ru_u(25), ru_u(21), &d, sc, st);
  }
  page_indicator(a, s, 26.5f);
}

/* ---- section: favourites (home) ---- */

static lv_obj_t *build_home(ru_dzapp_t *a) {
  const ru_theme_t *t = ru_theme();
  dz_conn_state_t cs = conn(a);
  build_ix(a, RU_SEC_HOME);
  bool any_stale = ix_any_stale(a);
  lv_obj_t *s = section_screen(a, any_stale);
  title(s, "FAVOURITES");
  conn_banner(a, s, cs, -34); /* the connection problem line; stale data shows on the rim dot only */
  grid_page(a, s);
  if (a->nrows == 0) {
    if (cs == DZ_CS_OK) {
      label_u(s, "NO FAVOURITES", ru_font_body(), t->text_dim, 0, -2);
      label_u(s, "MARK THEM IN DOMOTICZ", ru_font_small(), t->text_dim, 0, 5);
    } else {
      label_u(s, cs == DZ_CS_TOKEN_REJECTED ? "CHECK THE TOKEN IN SETTINGS" : "WAITING FOR DOMOTICZ", ru_font_small(), t->text_dim, 0, 0);
    }
  }
  fb_label(a, s, 33.5f);
  pager_dots(a, s);
  return s;
}

/* ---- lights, scenes, temperature, weather: tiles; utility: value rows ---- */

static const char *empty_text(ru_section_t sec) {
  switch (sec) {
    case RU_SEC_LIGHTS: return "NO LIGHTS";
    case RU_SEC_SCENES: return "NO SCENES";
    case RU_SEC_TEMP: return "NO TEMPERATURE";
    case RU_SEC_WEATHER: return "NO WEATHER";
    default: return "NO DEVICES";
  }
}

static lv_obj_t *build_tiles_section(ru_dzapp_t *a, ru_section_t sec) {
  dz_conn_state_t cs = conn(a);
  build_ix(a, sec);
  lv_obj_t *s = section_screen(a, ix_any_stale(a));
  title(s, ru_section_name(sec));
  conn_banner(a, s, cs, -34);
  grid_page(a, s);
  if (a->nrows == 0) label_u(s, cs == DZ_CS_OK ? empty_text(sec) : "WAITING FOR DOMOTICZ", ru_font_body(), ru_theme()->text_dim, 0, 0);
  fb_label(a, s, 33.5f);
  pager_dots(a, s);
  return s;
}

static void row_tile_update(lv_obj_t *tile, const dz_device_t *d, const dz_units_t *un, ru_val_state_t st) {
  const ru_theme_t *t = ru_theme();
  char txt[32];
  bool known;
  ru_dzv_value_text_u(d, un, txt, sizeof txt, &known);
  bool on = (d->cap == DZ_CAP_ONOFF || d->cap == DZ_CAP_DIMMER) && dz_device_state(d) == DZ_STATE_ON;
  ru_value_tile_set_color(tile, on ? t->accent : t->text);
  ru_value_tile_set_text(tile, txt, known ? st : RU_VAL_UNKNOWN);
}

static lv_obj_t *build_rows_section(ru_dzapp_t *a, ru_section_t sec) {
  const ru_theme_t *t = ru_theme();
  dz_conn_state_t cs = conn(a);
  build_ix(a, sec);
  lv_obj_t *s = section_screen(a, ix_any_stale(a));
  title(s, ru_section_name(sec));
  conn_banner(a, s, cs, -34);
  size_t first = page_setup(a, a->ix_n, PAGE_ROWS);
  int w;
  const float rh = 13.0f, pad = 1.5f;
  lv_obj_t *list = list_box(s, -29, -29 + PAGE_ROWS * rh + (PAGE_ROWS - 1) * pad, &w);
  lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(list, ru_u(pad), 0);
  a->list = list;
  dz_units_t un;
  get_units(a, &un);
  for (size_t k = first; k < a->ix_n && k < first + PAGE_ROWS && a->nrows < MAX_ROWS; k++) {
    dz_device_t d;
    ru_val_state_t st;
    bool sc;
    if (!ix_get(a, k, &d, NULL, &st, &sc)) continue;
    row_t *r = &a->rows[a->nrows];
    r->app = a; r->scene = false; r->setting = -1;
    snprintf(r->idx, sizeof r->idx, "%s", d.idx);
    char nm[40];
    ru_dzv_upper(nm, sizeof nm, d.name);
    ru_dzv_clamp(nm, sizeof nm, nm, 22);
    lv_obj_t *tile = ru_value_tile_create(list, w, ru_u(rh), nm);
    row_tile_update(tile, &d, &un, st);
    ru_value_tile_on_tap(tile, cb_row, r);
    lv_obj_add_event_cb(tile, cb_press, LV_EVENT_PRESSED, a);
    a->row_tiles[a->nrows++] = tile;
  }
  if (a->nrows == 0) label_u(s, cs == DZ_CS_OK ? empty_text(sec) : "WAITING FOR DOMOTICZ", ru_font_body(), t->text_dim, 0, 0);
  page_indicator(a, s, 31.5f);
  fb_label(a, s, 37.0f);
  pager_dots(a, s);
  return s;
}

/* ------------------------------------------------------------------ section: energy */

static void cb_cycle_energy(lv_event_t *e) { ru_dzapp_t *a = lv_event_get_user_data(e); a->energy_sel++; a->sig = 0; }

static const char *stale_text(const ru_val_t *v, uint32_t now) {
  static char buf[40];
  char age[24];
  ru_age_format(ru_val_age_ms(v, now), age, sizeof age);
  snprintf(buf, sizeof buf, "UPDATED %s", age);
  return buf;
}

static lv_obj_t *tap_zone(lv_obj_t *s, lv_event_cb_t cb, ru_dzapp_t *a) {
  lv_obj_t *z = ru_cont_new(s, 0, 0, ru_u(46), ru_u(46));
  lv_obj_align(z, LV_ALIGN_CENTER, 0, ru_u(-4));
  lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(z, cb, LV_EVENT_CLICKED, a);
  return z;
}

static lv_obj_t *build_energy(ru_dzapp_t *a) {
  const ru_theme_t *t = ru_theme();
  int n = (int)build_ix(a, RU_SEC_ENERGY);
  dz_device_t d;
  ru_val_t v;
  ru_val_state_t st = RU_VAL_UNKNOWN;
  bool have = n > 0;
  if (have) {
    a->energy_sel %= n;
    bool sc;
    if (!ix_get(a, (size_t)a->energy_sel, &d, &v, &st, &sc)) have = false;
  }
  lv_obj_t *s = section_screen(a, have && st == RU_VAL_STALE);
  if (!have) {
    title(s, "ENERGY");
    label_u(s, "NO ENERGY METER", ru_font_body(), t->text_dim, 0, 0);
    pager_dots(a, s);
    return s;
  }
  char nm[40];
  ru_dzv_upper(nm, sizeof nm, d.name);
  ru_dzv_clamp(nm, sizeof nm, nm, 18);
  label_u(s, nm, ru_font_body(), t->text_dim, 0, -40.5f);
  if (n > 1) { char c[24]; snprintf(c, sizeof c, "%d/%d", a->energy_sel + 1, n); label_u(s, c, ru_font_small(), t->text_dim, 0, -35.5f); }
  ru_energy_view_t ev;
  ru_dzv_energy(&d, &ev);
  if (ev.known || !ev.has_today) {
    lv_obj_t *g = ru_ring_gauge_create(s, ru_u(42), ev.flow == RU_FLOW_EXPORT ? "EXPORT" : "POWER");
    lv_obj_align(g, LV_ALIGN_CENTER, 0, ru_u(-6));
    ru_val_t pv = ru_val_unknown();
    float mx = 1000.0f;
    if (ev.known) pv = power_val(st, a->now, ev.hero_w, &mx);
    ru_ring_gauge_set_color(g, ev.flow == RU_FLOW_EXPORT ? t->ok : t->accent);
    ru_ring_gauge_set(g, &pv, 0.0f, mx, a->now);
  } else {
    /* no power reading (a gas or water meter): the day total is the hero, with its own unit */
    char num[16];
    snprintf(num, sizeof num, "%.1f", (double)d.today);
    label_u(s, num, ru_font_hero(), st == RU_VAL_STALE ? t->stale : t->text, 0, -8);
    {
      char cap[16];
      snprintf(cap, sizeof cap, d.today_unit[0] ? "TODAY  %s" : "TODAY", d.today_unit);
      label_u(s, cap, ru_font_title(), t->text_dim, 0, 6);
    }
  }
  if (ev.known && ev.has_today) label_u(s, ev.today, ru_font_body(), t->text_dim, 0, 21.5f);
  else if (ev.known) label_u(s, "TODAY --", ru_font_body(), t->text_dim, 0, 21.5f);
  if (ev.flow_text[0]) label_u(s, ev.flow_text, ru_font_small(), ev.flow == RU_FLOW_EXPORT ? t->ok : t->text_dim, 0, 27.5f);
  if (st == RU_VAL_STALE) label_u(s, v.valid ? stale_text(&v, a->now) : "STALE", ru_font_small(), t->warn, 0, 33);
  if (n > 1) tap_zone(s, cb_cycle_energy, a);
  fb_label(a, s, 38);
  pager_dots(a, s);
  return s;
}

/* ------------------------------------------------------------------ section: settings */

static void cb_setting(lv_obj_t *w, void *ud) {
  (void)w;
  row_t *r = ud;
  ru_dzapp_open_sub(r->app, (ru_sub_t)r->setting);
}

static const char *state_word(dz_conn_state_t c) {
  switch (c) {
    case DZ_CS_OK: return "CONNECTED";
    case DZ_CS_CONNECTING: return "CONNECTING";
    case DZ_CS_OFFLINE: return "OFFLINE";
    case DZ_CS_TOKEN_REJECTED: return "TOKEN REJECTED";
    case DZ_CS_SERVER_ERROR: return "SERVER ERROR";
    case DZ_CS_BAD_RESPONSE: return "NOT DOMOTICZ?";
  }
  return "?";
}

static lv_obj_t *build_settings(ru_dzapp_t *a) {
  const ru_theme_t *t = ru_theme();
  dz_conn_state_t cs = conn(a);
  lv_obj_t *s = section_screen(a, false);
  title(s, "SETTINGS");
  int w;
  lv_obj_t *list = list_box(s, -32, 35, &w);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(list, ru_u(1.2f), 0);
  a->list = list;
  char bright[16];
  snprintf(bright, sizeof bright, "%d%%", a->brightness);
  /* the token is never shown, not even in part: only whether one is configured */
  bool has_token = a->client && a->client->token[0];
  const struct { const char *name, *val; int sub; lv_color_t col; } rows[] = {
    { "STATUS", state_word(cs), RU_SUB_CONNECTION, cs == DZ_CS_OK ? t->ok : cs == DZ_CS_CONNECTING ? t->text_dim : cs == DZ_CS_OFFLINE ? t->warn : t->error },
    { "TOKEN", has_token ? "SET  ****" : "NOT SET", -1, has_token ? t->text : t->warn },
    { "ACCESS", a->allow_control ? "CONTROL" : "VIEW ONLY", -1, t->text },
    { "BRIGHTNESS", bright, RU_SUB_BRIGHTNESS, t->text },
    { "ABOUT", ">", RU_SUB_ABOUT, t->text_dim },
  };
  for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
    row_t *r = &a->rows[a->nrows];
    r->app = a; r->scene = false; r->setting = rows[i].sub;
    r->idx[0] = 0;
    lv_obj_t *tile = ru_value_tile_create(list, w, ru_u(11), rows[i].name);
    ru_value_tile_set_color(tile, rows[i].col);
    ru_value_tile_set_text(tile, rows[i].val, RU_VAL_OK);
    if (rows[i].sub >= 0) ru_value_tile_on_tap(tile, cb_setting, r);
    a->row_tiles[a->nrows++] = tile;
  }
  pager_dots(a, s);
  return s;
}

static void cb_bright(lv_obj_t *w, int v, bool fin, void *ud) {
  (void)w;
  ru_dzapp_t *a = ud;
  a->interact_until = a->now + INTERACT_MS;
  if (v < 5) v = 5;
  a->brightness = v;
  if (fin && a->bright_fn) a->bright_fn(v, a->bright_ud);
}

static lv_obj_t *build_sub(ru_dzapp_t *a) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *s = ru_screen_new();
  lv_obj_t *rim = ru_status_rim_create(s);
  dz_conn_state_t cs = conn(a);
  ru_status_rim_set_state(rim, rim_for(cs, false));
  a->list = NULL;
  a->nrows = 0;
  char line[64];
  switch (a->sub) {
    case RU_SUB_CONNECTION: {
      title(s, "CONNECTION");
      label_u(s, state_word(cs), ru_font_title(), cs == DZ_CS_OK ? t->ok : cs == DZ_CS_CONNECTING ? t->text : cs == DZ_CS_OFFLINE ? t->warn : t->error, 0, -26);
      const dz_client_t *c = a->client;
      snprintf(line, sizeof line, "LAST ERROR  %s", c ? dz_client_err_name(c->last_err) : "--");
      ru_dzv_upper(line, sizeof line, line);
      label_u(s, line, ru_font_body(), t->text_dim, 0, -12);
      if (c && c->last_http_status) { snprintf(line, sizeof line, "HTTP  %d", c->last_http_status); label_u(s, line, ru_font_body(), t->text_dim, 0, -4); }
      snprintf(line, sizeof line, "REQUESTS  %u", c ? (unsigned)c->requests : 0u);
      label_u(s, line, ru_font_body(), t->text_dim, 0, 4);
      snprintf(line, sizeof line, "ITEMS  %u", (unsigned)dz_model_count(a->model));
      label_u(s, line, ru_font_body(), t->text_dim, 0, 12);
      if (cs == DZ_CS_TOKEN_REJECTED) label_u(s, "TOKEN WAS REJECTED BY THE SERVER", ru_font_small(), t->error, 0, 22);
      else if (cs != DZ_CS_OK) label_u(s, "SHOWING LAST KNOWN DATA", ru_font_small(), t->text_dim, 0, 22);
      break;
    }
    case RU_SUB_BRIGHTNESS: {
      lv_obj_t *w = ru_dimmer_create(s, ru_u(80), 100, "BRIGHTNESS");
      ru_dimmer_set(w, a->brightness);
      ru_dimmer_on_change(w, cb_bright, a);
      break;
    }
    case RU_SUB_ABOUT:
    default:
      title(s, "ABOUT");
      label_u(s, "dzPanel", ru_font_title(), t->text, 0, -24);
      snprintf(line, sizeof line, "LVGL %d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
      label_u(s, line, ru_font_body(), t->text_dim, 0, -12);
      if (a->client && a->client->version.valid) snprintf(line, sizeof line, "DOMOTICZ %d.%d", a->client->version.year, a->client->version.minor);
      else snprintf(line, sizeof line, "DOMOTICZ --");
      label_u(s, line, ru_font_body(), t->text_dim, 0, -4);
      {
        char id[24];
        size_t k = 0;
        for (; a->board[k] && k < sizeof id - 1; k++) id[k] = (char)toupper((unsigned char)a->board[k]);
        id[k] = 0;
        snprintf(line, sizeof line, "%s %d PX %s", k ? id : "DISPLAY", ru_theme_geom()->diameter, a->square ? "SQUARE" : "ROUND");
      }
      label_u(s, line, ru_font_body(), t->text_dim, 0, 4);
      label_u(s, a->allow_control ? "CONTROL ENABLED" : "VIEW ONLY", ru_font_body(), t->text_dim, 0, 12);
      break;
  }
  back_button(a, s, "BACK");
  return s;
}

/* ------------------------------------------------------------------ detail and confirm */

static void touch_guard(ru_dzapp_t *a) { a->interact_until = a->now + INTERACT_MS; }

static void cb_toggle(lv_obj_t *w, bool on, void *ud) { (void)w; request_cmd(ud, on ? DZ_CMD_ON : DZ_CMD_OFF, 0, 0); }
static void lock_cmd(ru_dzapp_t *a, dz_cmd_kind_t k, const char *verb) {
  submit_or_confirm(a, a->cur_idx, a->cur_scene, k, 0, 0, verb, false);
  a->sig = 0;
}
static void cb_lock_on(lv_obj_t *w, void *ud) { (void)w; lock_cmd(ud, DZ_CMD_ON, "LOCK"); }      /* Domoticz: On = lock */
static void cb_lock_off(lv_obj_t *w, void *ud) { (void)w; lock_cmd(ud, DZ_CMD_OFF, "UNLOCK"); }  /* Off = unlock */
static void cb_selector(lv_obj_t *w, int i, bool fin, void *ud) {
  (void)w;
  ru_dzapp_t *a = ud;
  touch_guard(a);
  if (fin) request_cmd(a, DZ_CMD_SELECTOR, i * 10, 0);
}
static void cb_blinds(lv_obj_t *w, ru_blinds_cmd_t c, void *ud) {
  (void)w;
  request_cmd(ud, c == RU_BLINDS_UP ? DZ_CMD_BLINDS_OPEN : c == RU_BLINDS_DOWN ? DZ_CMD_BLINDS_CLOSE : DZ_CMD_BLINDS_STOP, 0, 0);
}
static void cb_thermo(lv_obj_t *w, float v, bool fin, void *ud) {
  (void)w;
  ru_dzapp_t *a = ud;
  touch_guard(a);
  if (!fin) return;
  /* the hold screen says WHAT will be set, in the device's own unit: "SET 21.5\xC2\xB0" + TempSign */
  dz_device_t d;
  ru_val_t val;
  ru_val_state_t st;
  char verb[24] = "SET";
  if (get_entry(a, a->cur_idx, a->cur_scene, &d, &val, &st)) {
    dz_units_t un;
    get_units(a, &un);
    ru_cl_thermo_t th;
    char num[20];
    ru_cl_thermo(&d, &un, NULL, a->allow_control, &th);
    float c = d.has_setpoint_range ? (v < d.setpoint_min ? d.setpoint_min : v > d.setpoint_max ? d.setpoint_max : v) : v;
    ru_cl_thermo_text(&th, c, num, sizeof num);
    snprintf(verb, sizeof verb, "SET %s", num);
  }
  submit_or_confirm(a, a->cur_idx, a->cur_scene, DZ_CMD_SETPOINT, 0, v, verb, false);
  a->sig = 0;
}
static void cb_mode(lv_obj_t *w, int i, bool fin, void *ud) {
  (void)w;
  ru_dzapp_t *a = ud;
  touch_guard(a);
  if (!fin) return;
  dz_device_t d;
  ru_val_t val;
  ru_val_state_t st;
  if (!get_entry(a, a->cur_idx, a->cur_scene, &d, &val, &st) || i < 0 || i >= d.n_modes || i >= DZ_MAX_MODES) {
    ru_fb_on_refused(&a->fb, DZ_CMDE_BAD_ARG, a->now);
    return;
  }
  char nm[24], verb[24];
  ru_cl_mode_name(&d, i, nm, sizeof nm);
  snprintf(verb, sizeof verb, "MODE %.14s", nm);
  submit_or_confirm(a, a->cur_idx, a->cur_scene, DZ_CMD_MODE, d.mode_val[i], 0, verb, false);
  a->sig = 0;
}
static void cb_push(lv_obj_t *w, void *ud) { (void)w; request_cmd(ud, DZ_CMD_PUSH, 0, 0); }

static void cb_hold(lv_obj_t *w, ru_hold_event_t ev, void *ud) {
  (void)w;
  ru_dzapp_t *a = ud;
  /* RU_HOLD_CANCELLED fires on ANY early release: it must not leave the screen (a slightly short hold would dump
   * the user out). Leaving is the CANCEL button's job only. */
  if (ev != RU_HOLD_CONFIRMED) return;
  a->pend.req.confirmed = true;
  post_cmd(a, &a->pend);
  /* back to where the user came from: the result line appears there */
  ru_dzapp_back(a);
}

static lv_obj_t *build_confirm(ru_dzapp_t *a) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *s = ru_screen_new();
  ru_status_rim_create(s);
  label_u(s, a->pend_title, ru_font_title(), t->text_dim, 0, -33);
  if (a->pend_state[0]) {
    char st[40];
    snprintf(st, sizeof st, "NOW  %s", a->pend_state);
    label_u(s, st, ru_font_body(), !strcmp(a->pend_state, "MIXED") ? t->warn : t->text_dim, 0, -26);
  }
  label_u(s, a->pend_verb, strlen(a->pend_verb) > 8 ? ru_font_title() : ru_font_hero(), t->text, 0, -14.5f);
  lv_obj_t *h = ru_hold_confirm_create(s, ru_u(36), "HOLD", 1200);
  lv_obj_align(h, LV_ALIGN_CENTER, 0, ru_u(13));
  ru_hold_confirm_on_event(h, cb_hold, a);
  back_button(a, s, "CANCEL");
  a->list = NULL;
  return s;
}

static void split_names(ru_dzapp_t *a, const dz_device_t *d, int *n) {
  *n = 0;
  while (*n < MAX_NAMES && ru_dzv_selector_name(d, *n, a->names[*n], NAME_LEN)) { a->name_ptrs[*n] = a->names[*n]; (*n)++; }
}



/* ---- next / previous device on a detail page: the list the user came from (a->section), scenes skipped ---- */
static bool detail_pos(ru_dzapp_t *a, int *pos, int *total, char *prev, char *next) {
  build_ix(a, a->section);
  int n = 0, at = -1;
  dz_device_t d;
  bool sc;
  for (size_t k = 0; k < a->ix_n; k++) {
    if (!ix_get(a, k, &d, NULL, NULL, &sc) || sc) continue;
    if (!strcmp(d.idx, a->cur_idx)) at = n;
    n++;
  }
  *pos = at;
  *total = n;
  if (prev) prev[0] = 0;
  if (next) next[0] = 0;
  if (at < 0) return false;
  int i = 0;
  for (size_t k = 0; k < a->ix_n; k++) {
    if (!ix_get(a, k, &d, NULL, NULL, &sc) || sc) continue;
    if (i == at - 1 && prev) snprintf(prev, 12, "%s", d.idx);
    if (i == at + 1 && next) snprintf(next, 12, "%s", d.idx);
    i++;
  }
  return true;
}

/* ---- light control page: the ru_light widget, updated in place (no rebuild on a poll or a command result) ---- */
static uint32_t hue_rgb_fn(float p) {
  uint8_t r, g, b;
  dz_hsb2rgb(p * 360.0f, &r, &g, &b);
  return (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}
static uint32_t temp_rgb_fn(float p) { return dz_temp_rgb(p); }
static const ru_light_hooks_t k_light_hooks = { hue_rgb_fn, temp_rgb_fn, dz_hue_word, dz_warmth_word };

static void light_state_of(const dz_device_t *d, bool known, ru_light_state_t *o) {
  memset(o, 0, sizeof *o);
  dz_state_t ds = dz_device_state(d);
  o->known = known && ds != DZ_STATE_UNKNOWN;
  o->on = ds == DZ_STATE_ON;
  int max = d->max_level > 0 ? d->max_level : 100;
  o->level = d->has_level ? (d->level < 0 ? 0 : d->level > max ? max : d->level) : (o->on ? max : 0);
  dz_light_mode_t m = d->col_caps ? dz_light_mode(d) : DZ_LM_NONE;
  o->mode = (int)m;
  if (m == DZ_LM_COLOR) {
    float h, sat;
    if (dz_rgb_to_hue(d->col_r, d->col_g, d->col_b, &h, &sat)) {
      o->has_hue = true;
      o->hue = (int)lroundf(h) % 360;
      uint8_t r, g, b;
      dz_hsb2rgb((float)o->hue, &r, &g, &b);
      o->color = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
    } else {
      o->color = 0xFFE3BF; /* a white-ish RGB: no hue, a neutral lamp colour */
    }
  } else if (m == DZ_LM_TEMP) {
    o->warmth = dz_t_to_warmth(d->col_t);
    o->color = dz_warmth_rgb(o->warmth);
  } else if (m == DZ_LM_WHITE) {
    o->color = 0xFFE3BF;
  }
}

/* what the light page shows for the state the model has: the setters compare, so an unchanged call touches nothing */
static void light_apply(ru_dzapp_t *a, const dz_device_t *d, ru_val_state_t st) {
  if (!a->light) return;
  bool known = st != RU_VAL_UNKNOWN;
  ru_light_state_t ls;
  light_state_of(d, known, &ls);
  ru_light_set(a->light, &ls);
  bool ctl = a->allow_control && d->controllable;
  const char *why = d->is_protected ? "PROTECTED" : !ctl ? "VIEW ONLY" : st == RU_VAL_STALE ? "STALE" : "";
  ru_light_set_readonly(a->light, why[0] != 0, why);
  ru_light_fb_t fs = RU_LIGHT_FB_NONE;
  const char *ft = "";
  if (!strcmp(a->fb.idx, d->idx) && !a->fb.scene) {
    switch (a->fb.st) {
      case RU_FB_PENDING: fs = RU_LIGHT_FB_SENDING; ft = "SENDING"; break;
      case RU_FB_CONFIRMED: fs = RU_LIGHT_FB_OK; ft = "SET"; break;
      case RU_FB_UNCONFIRMED: fs = RU_LIGHT_FB_WARN; ft = ru_fb_text(&a->fb); break;
      case RU_FB_FAILED: fs = RU_LIGHT_FB_ERROR; ft = ru_fb_text(&a->fb); break;
      default: break;
    }
  }
  ru_light_set_feedback(a->light, fs, ft);
  if (a->rim) ru_status_rim_set_state(a->rim, rim_for(conn(a), st == RU_VAL_STALE));
}

/* the structure of the light page: what only a rebuild can change */
static uint64_t light_struct_sig(const ru_dzapp_t *a, const dz_device_t *d) {
  uint64_t h = 1469598103934665603ULL;
  h = fnv(h, d->idx, strlen(d->idx));
  h = fnv(h, d->name, strlen(d->name));
  h = fnv(h, &d->cap, sizeof d->cap);
  h = fnv(h, &d->col_caps, sizeof d->col_caps);
  h = fnv(h, &d->max_level, sizeof d->max_level);
  h = fnv(h, &d->controllable, sizeof d->controllable);
  h = fnv(h, &d->is_protected, sizeof d->is_protected);
  h = fnv(h, &a->allow_control, sizeof a->allow_control);
  h = fnv(h, &a->square, sizeof a->square);
  int pos, total;
  detail_pos((ru_dzapp_t *)a, &pos, &total, NULL, NULL); /* the N/M indicator is part of the structure */
  h = fnv(h, &pos, sizeof pos);
  h = fnv(h, &total, sizeof total);
  return h ? h : 1;
}

static void light_refresh(ru_dzapp_t *a) {
  dz_device_t d;
  ru_val_t v;
  ru_val_state_t st;
  if (a->light && get_entry(a, a->cur_idx, a->cur_scene, &d, &v, &st)) light_apply(a, &d, st);
}

/* the brightness a colour command carries: the lamp's Level as a percentage; an Off lamp uses the stored Level, 100 when unknown */
static int light_brightness(const dz_device_t *d) {
  int max = d->max_level > 0 ? d->max_level : 100;
  int pct = d->has_level ? (int)lroundf(100.0f * (float)d->level / (float)max) : 0;
  return pct < 1 ? 100 : pct > 100 ? 100 : pct;
}

static void cb_light(lv_obj_t *w, ru_light_cmd_t c, int v, bool fin, void *ud) {
  (void)w;
  ru_dzapp_t *a = ud;
  touch_guard(a);
  if (!fin) return;
  a->interact_until = 0; /* the page is updated in place, and the knob waits for the model by itself */
  dz_device_t d;
  ru_val_t val;
  ru_val_state_t st;
  if (!get_entry(a, a->cur_idx, a->cur_scene, &d, &val, &st)) { ru_fb_on_refused(&a->fb, DZ_CMDE_UNKNOWN_DEVICE, a->now); return; }
  bool off = dz_device_state(&d) != DZ_STATE_ON;
  switch (c) {
    case RU_LIGHT_CMD_LEVEL: submit_or_confirm(a, a->cur_idx, false, DZ_CMD_SET_LEVEL, v, 0, NULL, false); break;
    case RU_LIGHT_CMD_POWER: submit_or_confirm(a, a->cur_idx, false, v ? DZ_CMD_ON : DZ_CMD_OFF, 0, 0, NULL, false); break;
    case RU_LIGHT_CMD_HUE: submit_or_confirm_h(a, a->cur_idx, false, DZ_CMD_COLOR_HUE, light_brightness(&d), 0, v, NULL, false); break;
    case RU_LIGHT_CMD_TEMP: submit_or_confirm(a, a->cur_idx, false, DZ_CMD_COLOR_TEMP, v, 0, NULL, false); break;
    case RU_LIGHT_CMD_WHITE: submit_or_confirm(a, a->cur_idx, false, DZ_CMD_COLOR_WHITE, light_brightness(&d), 0, NULL, false); break;
  }
  /* whether a colour or temperature command switches an Off lamp on is unverified: DZ_COLOR_WHEN_OFF_SENDS_ON decides (default: no) */
  if (DZ_COLOR_WHEN_OFF_SENDS_ON && off && (c == RU_LIGHT_CMD_HUE || c == RU_LIGHT_CMD_TEMP || c == RU_LIGHT_CMD_WHITE))
    submit_or_confirm(a, a->cur_idx, false, DZ_CMD_ON, 0, 0, NULL, false);
  light_refresh(a);
}

static lv_obj_t *build_detail(ru_dzapp_t *a) {
  const ru_theme_t *t = ru_theme();
  dz_device_t d;
  ru_val_t v;
  ru_val_state_t st;
  lv_obj_t *s = ru_screen_new();
  a->list = NULL;
  a->light = NULL;
  a->nrows = 0;
  if (!get_entry(a, a->cur_idx, a->cur_scene, &d, &v, &st)) {
    ru_status_rim_create(s);
    label_u(s, "DEVICE NOT FOUND", ru_font_title(), t->text_dim, 0, 0);
    back_button(a, s, "BACK");
    return s;
  }
  a->detail_cap = d.cap;
  a->detail_thermo = d.cap == DZ_CAP_THERMOSTAT; /* its screen may show the room's temperature sensor */
  lv_obj_t *rim = ru_status_rim_create(s);
  ru_status_rim_set_state(rim, rim_for(conn(a), st == RU_VAL_STALE));
  char name[40];
  ru_dzv_upper(name, sizeof name, d.name);
  ru_dzv_clamp(name, sizeof name, name, 20);
  bool known;
  char vt[32];
  dz_units_t vun;
  get_units(a, &vun);
  ru_dzv_value_text_u(&d, &vun, vt, sizeof vt, &known);
  bool unknown = !known || st == RU_VAL_UNKNOWN;
  bool ctl = a->allow_control && d.controllable;
  dz_state_t ds = dz_device_state(&d);
  bool dial = false, climate = false;

  switch (d.cap) {
    case DZ_CAP_LOCK: {
      /* the state in words, and one button for the OTHER state (Domoticz: On = lock, Off = unlock; a hold confirms) */
      label_u(s, name, ru_font_title(), t->text_dim, 0, -40);
      bool locked = !strcmp(d.status, "Locked"), open = !strcmp(d.status, "Unlocked");
      bool known_state = !unknown && (locked || open);
      label_u(s, known_state ? (locked ? "LOCKED" : "UNLOCKED") : "--", ru_font_hero(),
              !known_state ? t->text_dim : locked ? t->ok : t->warn, 0, -14);
      if (known_state && ctl) {
        lv_obj_t *b = ru_scene_button_create(s, ru_u(36), ru_u(20), locked ? "UNLOCK" : "LOCK", "HOLD TO CONFIRM", locked ? t->warn : t->ok);
        lv_obj_align(b, LV_ALIGN_CENTER, 0, ru_u(14));
        ru_scene_button_on_tap(b, locked ? cb_lock_off : cb_lock_on, a);
      } else if (known_state) {
        label_u(s, "VIEW ONLY", ru_font_body(), t->text_dim, 0, 14);
      }
      break;
    }
    case DZ_CAP_ONOFF: {
      label_u(s, name, ru_font_title(), t->text_dim, 0, -40);
      lv_obj_t *w = ru_toggle_create(s, ru_u(54));
      ru_toggle_set(w, unknown || ds == DZ_STATE_UNKNOWN ? RU_ONOFF_UNKNOWN : ds == DZ_STATE_ON ? RU_ONOFF_ON : RU_ONOFF_OFF);
      if (ctl) ru_toggle_on_change(w, cb_toggle, a);
      label_u(s, ctl ? (ds == DZ_STATE_ON ? "TAP TO SWITCH OFF" : "TAP TO SWITCH ON") : "VIEW ONLY", ru_font_body(), t->text_dim, 0, 33);
      break;
    }
    case DZ_CAP_DIMMER: {
      /* every dimmer and colour light: rings + centre power disc (+ chips); the page is updated in place, see light_tick */
      ru_light_cfg_t cfg = { .caps = d.col_caps, .two_rings = a->square, .max = d.max_level > 0 ? d.max_level : 100, .hooks = &k_light_hooks };
      lv_obj_t *w = ru_light_create(s, ru_u(86), &cfg);
      a->light = w;
      a->rim = rim;
      ru_light_on_change(w, cb_light, a);
      light_apply(a, &d, st);
      name_back(a, s, name, 40);
      int pos, total;
      if (detail_pos(a, &pos, &total, NULL, NULL) && total > 1) {
        char nm[28];
        snprintf(nm, sizeof nm, "%d/%d", pos + 1, total);
        label_u(s, nm, ru_font_small(), ru_dim(t->text_dim, 170), 0, 33.4f);
      }
      dial = true;
      break;
    }
    case DZ_CAP_SELECTOR: {
      int n;
      split_names(a, &d, &n);
      if (n < 2) goto info;
      lv_obj_t *w = ru_selector_create(s, ru_u(86), a->name_ptrs, n);
      int idx = d.level_int / 10;
      ru_selector_set(w, idx >= 0 && idx < n ? idx : 0);
      if (ctl) ru_selector_on_change(w, cb_selector, a);
      name_back(a, s, name, 40);
      dial = true;
      break;
    }
    case DZ_CAP_BLINDS: {
      name_back(a, s, name, -40);
      dial = true; /* the bottom row is up / stop / down: the name at the top is the back control */
      lv_obj_t *b = ru_blinds_create(s, ru_u(60), ru_u(76));
      lv_obj_align(b, LV_ALIGN_CENTER, 0, ru_u(5.2f));
      ru_val_t cl = d.has_level && (d.switch_type == 13 || d.switch_type == 21)
                        ? ru_val_make((float)(d.level), RU_UNIT_PCT, a->now, 600000) : ru_val_unknown();
      ru_blinds_set(b, &cl, a->now);
      if (ctl) ru_blinds_on_cmd(b, cb_blinds, a);
      break;
    }
    case DZ_CAP_THERMOSTAT: {
      /* range, step and unit come from the device and getconfig; no sane range = no control, shown read-only */
      dz_units_t un;
      get_units(a, &un);
      dz_device_t ps;
      ru_val_t pv;
      ru_val_state_t pst = RU_VAL_UNKNOWN;
      bool plan = !dz_dev_room_temp(&d) && dz_model_plan_temp(a->model, &d, &ps, &pv, &pst, a->now);
      ru_cl_thermo_t th;
      ru_cl_thermo(&d, &un, plan ? &ps : NULL, a->allow_control, &th);
      float lo = th.control ? th.lo : d.has_setpoint_range ? d.setpoint_min : floorf(th.setpoint) - 10.0f;
      float hi = th.control ? th.hi : d.has_setpoint_range ? d.setpoint_max : floorf(th.setpoint) + 10.0f;
      float step = th.control ? th.step : d.has_setpoint_range ? d.setpoint_step : 0.5f;
      if (!(hi > lo) || !(step > 0.0f)) { lo = floorf(th.setpoint) - 10.0f; hi = lo + 20.0f; step = 0.5f; }
      lv_obj_t *w = ru_thermostat_create(s, ru_u(86), lo, hi, step);
      ru_thermostat_set_unit(w, th.unit);
      if (d.has_setpoint) ru_thermostat_set_setpoint(w, d.setpoint);
      ru_val_t now = th.has_now ? make_val(th.now, RU_UNIT_NONE, plan ? pst : st, a->now) : ru_val_unknown();
      ru_thermostat_set_current(w, &now, a->now); /* its own temperature, else the single sensor of its plan, else "--" */
      if (th.control) ru_thermostat_on_change(w, cb_thermo, a);
      else {
        ru_thermostat_set_readonly(w, true);
        char ro[40];
        snprintf(ro, sizeof ro, "READ ONLY  %s", th.reason);
        label_u(s, ro, ru_font_small(), t->warn, 0, 25);
      }
      name_back(a, s, name, 40);
      dial = true;
      break;
    }
    case DZ_CAP_MODE: {
      /* a selector over Modes, shown read-only: the MCP endpoint has no thermostat-mode tool */
      int n = d.n_modes < MAX_NAMES ? d.n_modes : MAX_NAMES;
      for (int i = 0; i < n; i++) { if (!ru_cl_mode_name(&d, i, a->names[i], NAME_LEN)) snprintf(a->names[i], NAME_LEN, "?"); a->name_ptrs[i] = a->names[i]; }
      if (n < 2) goto info;
      lv_obj_t *w = ru_selector_create(s, ru_u(86), a->name_ptrs, n);
      int slot = ru_cl_mode_slot(&d);
      ru_selector_set(w, slot >= 0 ? slot : 0);
      /* no MCP tool for the thermostat mode: the modes are shown, not set */
      if (ctl && dz_cmd_supported(&d, DZ_CMD_MODE, NULL)) ru_selector_on_change(w, cb_mode, a);
      else label_u(s, "MODE: VIEW ONLY", ru_font_small(), t->warn, 0, 25);
      if (slot < 0) label_u(s, "MODE UNKNOWN", ru_font_small(), t->warn, 0, 20);
      name_back(a, s, name, 40);
      dial = true;
      break;
    }
    case DZ_CAP_PUSH: {
      lv_obj_t *b = ru_scene_button_create(s, ru_u(34), ru_u(26), "PUSH", "BUTTON", t->accent);
      lv_obj_align(b, LV_ALIGN_CENTER, 0, 0);
      if (ctl) ru_scene_button_on_tap(b, cb_push, a);
      name_back(a, s, name, 40);
      dial = true;
      break;
    }
    case DZ_CAP_TEMP:
    case DZ_CAP_WEATHER:
    case DZ_CAP_SENSOR: {
      /* the same page the climate section shows: readings with their units, honest "--" and stale marks */
      dz_units_t un;
      get_units(a, &un);
      ru_cl_units_t cu;
      ru_cl_units(&un, &cu);
      ru_cl_view_t cv;
      ru_cl_build(&d, &cu, &cv);
      char age[24], stale[40];
      ru_age_format(ru_val_age_ms(&v, a->now), age, sizeof age);
      snprintf(stale, sizeof stale, v.valid ? "UPDATED %s" : "STALE", age);
      ru_dzv_upper(stale, sizeof stale, stale);
      ru_cl_page_ctx_t ctx = { .v = &cv, .st = st, .stale_text = stale, .k = 0, .n = 1, .no_name = true };
      ru_climate_draw_page(s, &ctx);
      name_back(a, s, name, -40.5f);
      dial = true;
      climate = true;
      break;
    }
    case DZ_CAP_READONLY:
    case DZ_CAP_STATUS:
    case DZ_CAP_ENERGY:
    case DZ_CAP_SCENE:
    case DZ_CAP_GROUP:
    info: {
      label_u(s, name, ru_font_title(), t->text_dim, 0, -34);
      label_u(s, vt, ru_font_hero(), unknown ? t->text_dim : t->text, 0, -8);
      char line[64] = "";
      if (d.cap == DZ_CAP_ENERGY) { ru_energy_view_t ev; ru_dzv_energy(&d, &ev); if (ev.has_today) snprintf(line, sizeof line, "%s", ev.today); }
      if (line[0]) label_u(s, line, ru_font_body(), t->text_dim, 0, 12);
      break;
    }
  }

  /* freshness line: always honest about age when not fresh */
  if (st == RU_VAL_STALE && !climate && !a->light) {
    char age[24], buf[48];
    ru_age_format(ru_val_age_ms(&v, a->now), age, sizeof age);
    snprintf(buf, sizeof buf, "UPDATED %s", age);
    label_u(s, buf, ru_font_small(), t->warn, 0, 33);
  }
  if (!a->light) fb_label(a, s, 26);
  if (!dial) back_button(a, s, "BACK");
  return s;
}

/* ------------------------------------------------------------------ build, signature, refresh */

static void show(ru_dzapp_t *a, lv_obj_t *scr) {
  lv_obj_t *old = a->scr;
  a->scr = scr;
  lv_obj_update_layout(scr); /* so a touch right after the switch already finds the new objects */
  lv_screen_load(scr);
  if (old && old != scr) lv_obj_delete_async(old);
}

/* ---- refresh bookkeeping (diagnostics only with CONFIG_RU_HEAP_LOG) ---- */
#if CONFIG_RU_HEAP_LOG
#define STAT_T0() uint32_t t0_ = ru_now_us()
#define STAT_MAX(field) do { uint32_t d_ = ru_now_us() - t0_; if (d_ > a->stats.field) a->stats.field = d_; } while (0)
#define STAT_INC(field) (a->stats.field++)
#else
#define STAT_T0() do { } while (0)
#define STAT_MAX(field) do { } while (0)
#define STAT_INC(field) do { } while (0)
#endif

void ru_dzapp_stats(ru_dzapp_t *a, ru_dzapp_stats_t *out, bool reset) {
  memset(out, 0, sizeof *out);
#if CONFIG_RU_HEAP_LOG
  *out = a->stats;
  out->copies = dz_model_copies(a->model) - a->copies_base;
  if (reset) { memset(&a->stats, 0, sizeof a->stats); a->copies_base += out->copies; }
#else
  (void)a; (void)reset;
#endif
}

/* A screen the refresh can update in place: the tile grids and the utility rows. */
static bool grid_view(const ru_dzapp_t *a) {
  if (a->view != RU_VIEW_HOME) return false;
  switch (a->section) {
    case RU_SEC_HOME: case RU_SEC_LIGHTS: case RU_SEC_SCENES: case RU_SEC_TEMP: case RU_SEC_WEATHER: case RU_SEC_UTILITY: return true;
    default: return false;
  }
}

/* What the user (or the connection) changed, as opposed to the data: a change here rebuilds at once. */
static uint64_t imm_sig(const ru_dzapp_t *a) {
  uint64_t h = 1469598103934665603ULL;
  int c = (int)conn(a);
  h = fnv(h, &c, sizeof c);
  h = fnv(h, &a->view, sizeof a->view);
  h = fnv(h, &a->section, sizeof a->section);
  h = fnv(h, &a->energy_sel, sizeof a->energy_sel);
  h = fnv(h, &a->page, sizeof a->page);
  h = fnv(h, &a->fb.st, sizeof a->fb.st);
  h = fnv(h, a->fb.text, strlen(a->fb.text));
  return h;
}

/* Cheap and copy-free: when this equals the last evaluation nothing needs a look. Data moves it through the model
 * generation, time through the stale epoch (states are re-evaluated every STALE_EPOCH_MS, not on every tick). */
static uint64_t ctl_core(ru_dzapp_t *a) {
  uint64_t h = imm_sig(a);
  uint32_t g = dz_model_generation(a->model);
  h = fnv(h, &g, sizeof g);
  dz_units_t un;
  get_units(a, &un);
  h = fnv(h, &un.have_temp, sizeof un.have_temp);
  h = fnv(h, un.temp, strlen(un.temp));
  h = fnv(h, &un.have_wind, sizeof un.have_wind);
  h = fnv(h, un.wind, strlen(un.wind));
  return h;
}

/* The states of the entries of the section as one hash, copy-free: time alone moves a state (fresh -> stale), and only
 * this tells whether a time-only look changed anything. */
static uint64_t stale_hash(const ru_dzapp_t *a) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t k = 0; k < a->ix_n; k++) h = fnv(h, &a->ix[k].st, sizeof a->ix[k].st);
  return h;
}
static uint64_t stale_sig(ru_dzapp_t *a) { build_ix(a, a->section); return stale_hash(a); }

/* The full-content hash of the entries a non-grid screen shows (detail: its device; energy: the meters; sub screens
 * and settings: none), in one copy-free pass. */
typedef struct { const ru_dzapp_t *a; uint64_t h; } fullsig_t;

static bool fullsig_cb(size_t i, const dz_device_t *d, bool scene, ru_val_state_t st, uint16_t order, void *ud) {
  (void)i;
  fullsig_t *f = ud;
  const ru_dzapp_t *a = f->a;
  bool rel;
  if (a->view == RU_VIEW_DETAIL) rel = (scene == a->cur_scene && !strcmp(d->idx, a->cur_idx)) || (a->detail_thermo && (d->tabs & DZ_TAB_TEMP));
  else if (a->view == RU_VIEW_SUB) rel = false;
  else if (a->section == RU_SEC_ENERGY) rel = sec_match(RU_SEC_ENERGY, d, scene);
  else rel = a->section != RU_SEC_SETTINGS;
  if (!rel) return true;
  /* the whole entry, so no field can change unseen (the model compares the same bytes to detect a change) */
  uint64_t h = f->h;
  h = fnv(h, d, sizeof *d);
  h = fnv(h, &st, sizeof st);
  h = fnv(h, &scene, sizeof scene);
  h = fnv(h, &order, sizeof order);
  f->h = h;
  return true;
}

/* The STRUCTURE of a grid / rows screen: which entries are on this page, in which order, their kind, the page count,
 * the sections that exist. Not their values: those are updated in place. Leaves a->ix for the section. */
static uint64_t struct_sig(ru_dzapp_t *a) {
  uint64_t h = imm_sig(a);
  build_ix(a, a->section);
  int per = a->section == RU_SEC_UTILITY ? PAGE_ROWS : PAGE_TILES;
  int pages = a->ix_n ? (int)((a->ix_n + (size_t)per - 1) / (size_t)per) : 1;
  int page = a->page >= pages ? pages - 1 : a->page;
  if (page < 0) page = 0;
  unsigned mask = avail_mask(a);
  h = fnv(h, &a->ix_n, sizeof a->ix_n);
  h = fnv(h, &page, sizeof page);
  h = fnv(h, &mask, sizeof mask);
  size_t first = (size_t)page * (size_t)per;
  for (size_t k = first; k < a->ix_n && k < first + (size_t)per; k++) {
    dz_device_t d;
    bool sc;
    if (!ix_get(a, k, &d, NULL, NULL, &sc)) continue;
    h = fnv(h, d.idx, strlen(d.idx));
    h = fnv(h, d.name, strlen(d.name));
    h = fnv(h, &d.cap, sizeof d.cap);
    h = fnv(h, &sc, sizeof sc);
  }
  return h;
}

static uint64_t screen_sig(ru_dzapp_t *a) {
  STAT_T0();
  uint64_t h;
  dz_device_t ld;
  ru_val_t lv_;
  ru_val_state_t lst;
  if (a->view == RU_VIEW_DETAIL && a->light) {
    /* the light page: structure only, values go in place */
    h = get_entry(a, a->cur_idx, a->cur_scene, &ld, &lv_, &lst) ? light_struct_sig(a, &ld) : 1;
  } else if (grid_view(a)) h = struct_sig(a);
  else {
    fullsig_t f = { a, imm_sig(a) };
    dz_model_foreach(a->model, a->now, fullsig_cb, &f);
    unsigned mask = avail_mask(a); /* the pager dots show which sections exist */
    f.h = fnv(f.h, &mask, sizeof mask);
    if (a->view == RU_VIEW_SUB) { size_t n = dz_model_count(a->model); f.h = fnv(f.h, &n, sizeof n); }
    dz_units_t un;
    get_units(a, &un);
    f.h = fnv(f.h, &un.have_temp, sizeof un.have_temp);
    f.h = fnv(f.h, un.temp, strlen(un.temp));
    f.h = fnv(f.h, &un.have_wind, sizeof un.have_wind);
    f.h = fnv(f.h, un.wind, strlen(un.wind));
    h = f.h;
  }
  STAT_MAX(sig_max_us);
  return h ? h : 1;
}

static void build_and_show(ru_dzapp_t *a) {
  STAT_T0();
  lv_obj_t *s;
  a->detail_thermo = false;
  a->light = NULL;
  switch (a->view) {
    case RU_VIEW_CONFIRM: s = build_confirm(a); break;
    case RU_VIEW_DETAIL: s = build_detail(a); break;
    case RU_VIEW_SUB: s = build_sub(a); break;
    default:
      switch (a->section) {
        case RU_SEC_LIGHTS:
        case RU_SEC_SCENES:
        case RU_SEC_TEMP:
        case RU_SEC_WEATHER: s = build_tiles_section(a, a->section); break;
        case RU_SEC_UTILITY: s = build_rows_section(a, a->section); break;
        case RU_SEC_ENERGY: s = build_energy(a); break;
        case RU_SEC_SETTINGS: s = build_settings(a); break;
        default: s = build_home(a); break;
      }
  }
  show(a, s);
  a->sig = 0;
  a->ctl = 0;
  a->imm = imm_sig(a);
  a->last_build = a->now;
  STAT_INC(rebuilds);
  STAT_MAX(build_max_us);
}

/* The structure is unchanged (same entries, same order, same page): push the new values into the tiles that exist.
 * The tile setters compare with what they show, so only a tile whose value changed is redrawn. a->ix is current. */
static void update_in_place(ru_dzapp_t *a) {
  STAT_INC(inplace);
  size_t first = (size_t)a->page * (size_t)(a->section == RU_SEC_UTILITY ? PAGE_ROWS : PAGE_TILES);
  if (a->rim) ru_status_rim_set_state(a->rim, rim_for(conn(a), ix_any_stale(a)));
  dz_units_t un;
  get_units(a, &un);
  for (size_t r = 0; r < a->nrows; r++) {
    dz_device_t d;
    ru_val_state_t st;
    bool sc;
    if (!ix_get(a, first + r, &d, NULL, &st, &sc)) continue;
    if (a->section == RU_SEC_UTILITY) row_tile_update(a->row_tiles[r], &d, &un, st);
    else entry_tile_update(a, a->row_tiles[r], &d, sc, st);
  }
}

/* ------------------------------------------------------------------ public UI API */

void ru_dzapp_show_section(ru_dzapp_t *a, ru_section_t s) {
  if (s >= RU_SEC_COUNT || !ru_dzapp_section_available(a, s)) s = RU_SEC_HOME;
  /* the page is kept when the same section is shown again (back from a detail or a hold), reset when it changes */
  if (s != a->section) a->page = 0;
  a->section = s;
  a->view = RU_VIEW_HOME;
  build_and_show(a);
  a->sig = screen_sig(a);
}

void ru_dzapp_home(ru_dzapp_t *a) {
  a->page = 0;
  ru_dzapp_show_section(a, RU_SEC_HOME);
}
ru_section_t ru_dzapp_section(const ru_dzapp_t *a) { return a->section; }
int ru_dzapp_page(const ru_dzapp_t *a) { return a->page; }
int ru_dzapp_pages(const ru_dzapp_t *a) { return a->pages; }

bool ru_dzapp_step_section(ru_dzapp_t *a, int dir) {
  unsigned mask = avail_mask(a);
  int s = (int)a->section;
  do s += dir >= 0 ? 1 : -1; while (s >= 0 && s < RU_SEC_COUNT && !(mask >> s & 1u));
  if (s < 0 || s >= RU_SEC_COUNT) return false; /* no wrap: the first and the last section are ends */
  ru_dzapp_show_section(a, (ru_section_t)s);
  return true;
}

static bool page_step(ru_dzapp_t *a, int dir) {
  int p = a->page + (dir >= 0 ? 1 : -1);
  if (a->pages < 2 || p < 0 || p >= a->pages) return false;
  page_go(a, p);
  return true;
}

/* A swipe on a detail page steps to the next (left) or previous (right) device of the list the user came from, and stops at the
 * ends. It is never taken from a control: a drag that starts in a ring's touch band, on the thermostat dial or on the blinds
 * buttons belongs to that control. The disc of a light, the dead zones and the chips are free. */
static bool detail_swipe(ru_dzapp_t *a, ru_dir_t dir, int x0, int y0) {
  if (a->cur_scene || (dir != RU_DIR_LEFT && dir != RU_DIR_RIGHT)) return false;
  if (a->now < a->interact_until) return false; /* a control is held */
  float c = (float)ru_theme_geom()->diameter / 2, dx = (float)x0 - c, dy = (float)y0 - c, u = ru_theme_geom()->u;
  if (dx * dx + dy * dy >= 0.81f * c * c) return false; /* started in the rim: the shell owns it */
  if (a->light) {
    if (ru_light_ring_at(a->light, x0, y0)) return false;
  } else {
    float r = sqrtf(dx * dx + dy * dy);
    switch (a->detail_cap) {
      case DZ_CAP_THERMOSTAT: case DZ_CAP_SELECTOR: case DZ_CAP_MODE:
        if (r >= 31.0f * u || dy >= 12.0f * u) return false; /* the dial ring, the + / - buttons */
        break;
      case DZ_CAP_BLINDS:
        if (fabsf(dx) < 31.0f * u && dy > -37.0f * u && dy < 43.0f * u) return false; /* the buttons */
        break;
      default: break;
    }
  }
  int pos, total;
  char prev[12], next[12];
  if (!detail_pos(a, &pos, &total, prev, next)) return false;
  const char *to = dir == RU_DIR_LEFT ? next : prev;
  if (!to[0]) return false; /* the ends: no wrap */
  char idx[12];
  snprintf(idx, sizeof idx, "%s", to);
  ru_dzapp_open(a, idx, false);
  return true;
}

bool ru_dzapp_swipe(ru_dzapp_t *a, ru_dir_t dir, int x0, int y0) {
  if (a->view == RU_VIEW_DETAIL) return detail_swipe(a, dir, x0, y0);
  if (a->view != RU_VIEW_HOME) return false; /* hold and sub screens do not swipe */
  if (a->now < a->interact_until) return false; /* a slider is held */
  float c = (float)ru_theme_geom()->diameter / 2, dx = (float)x0 - c, dy = (float)y0 - c;
  if (dx * dx + dy * dy >= 0.81f * c * c) return false; /* started in the rim: the shell owns it */
  switch (dir) {
    case RU_DIR_LEFT: return ru_dzapp_step_section(a, 1);
    case RU_DIR_RIGHT: return ru_dzapp_step_section(a, -1);
    case RU_DIR_UP: return page_step(a, 1);
    case RU_DIR_DOWN: return page_step(a, -1);
    default: return false;
  }
}

void ru_dzapp_open(ru_dzapp_t *a, const char *idx, bool is_scene) {
  snprintf(a->cur_idx, sizeof a->cur_idx, "%s", idx);
  a->cur_scene = is_scene;
  if (is_scene) { scene_tap(a, idx); if (a->view != RU_VIEW_CONFIRM) { a->sig = 0; } return; }
  a->view = RU_VIEW_DETAIL;
  build_and_show(a);
  a->sig = screen_sig(a);
}

void ru_dzapp_pick_mode(ru_dzapp_t *a, int slot) { cb_mode(NULL, slot, true, a); }
void ru_dzapp_pick_setpoint(ru_dzapp_t *a, float v) { cb_thermo(NULL, v, true, a); }

void ru_dzapp_select(ru_dzapp_t *a, ru_section_t s, int n) {
  if (s == RU_SEC_ENERGY) a->energy_sel = n < 0 ? 0 : n;
  a->sig = 0;
}

void ru_dzapp_open_sub(ru_dzapp_t *a, ru_sub_t sub) {
  a->sub = sub;
  a->view = RU_VIEW_SUB;
  build_and_show(a);
  a->sig = screen_sig(a);
}

void ru_dzapp_ui_tick(ru_dzapp_t *a, uint32_t now_ms) {
  a->now = now_ms;
  drain_results(a);
  ru_fb_tick(&a->fb, now_ms);
  if (a->view == RU_VIEW_CONFIRM) return; /* a hold is in progress: never rebuild under the finger */
  if (now_ms < a->interact_until) return; /* nor while a slider is being dragged */
  uint64_t core = ctl_core(a);
  uint32_t epoch = now_ms / STALE_EPOCH_MS;
  if (a->sig && core == a->ctl && epoch == a->epoch) return; /* no new data, same stale epoch, same controls */
  bool time_only = a->sig && core == a->ctl;
  a->ctl = core;
  a->epoch = epoch;
  if (time_only && grid_view(a) && stale_sig(a) == a->stale_sig) return; /* only the clock moved and no state with it */
  STAT_INC(evals);
  uint64_t sig = screen_sig(a);
  if (a->view == RU_VIEW_DETAIL && a->light && a->sig && sig == a->sig) { /* the light page: same structure, new values go in place */
    STAT_INC(inplace);
    light_refresh(a);
    a->imm = imm_sig(a);
    return;
  }
  if (grid_view(a)) a->stale_sig = stale_hash(a);
  if (a->sig && sig == a->sig) { /* same structure: new values go into the existing tiles */
    if (grid_view(a)) update_in_place(a);
    return;
  }
  /* a change the user or the connection made rebuilds at once; a background change at most once per REBUILD_MIN_MS */
  bool urgent = !a->sig || imm_sig(a) != a->imm;
  if (!urgent && grid_view(a) && /* only the grids are rate limited; detail, energy and sub screens follow the data at once */ now_ms - a->last_build < REBUILD_MIN_MS) { a->ctl = 0; return; } /* looked at again on the next tick */
  int sx = a->list ? (int)lv_obj_get_scroll_x(a->list) : 0, sy = a->list ? (int)lv_obj_get_scroll_y(a->list) : 0;
  build_and_show(a);
  a->sig = sig;
  if (a->list && (sx > 0 || sy > 0)) lv_obj_scroll_to(a->list, sx, sy, LV_ANIM_OFF);
}

static void timer_cb(lv_timer_t *t) {
  ru_dzapp_t *a = lv_timer_get_user_data(t);
  ru_dzapp_ui_tick(a, ru_now_ms());
}

void ru_dzapp_start(ru_dzapp_t *a) {
  a->now = ru_now_ms();
  ru_dzapp_home(a);
  if (!a->timer) a->timer = lv_timer_create(timer_cb, 250, a);
}

/* ------------------------------------------------------------------ network side */

static void on_cmd_result(const dz_cmd_result_t *r, void *user) {
  ru_dzapp_t *a = user;
  res_msg_t m;
  memset(&m, 0, sizeof m);
  m.r = *r;
  ru_queue_send(a->res_q, &m, 0);
}

void ru_dzapp_net_step(ru_dzapp_t *a, uint32_t now_ms) {
  req_msg_t m;
  while (ru_queue_recv(a->req_q, &m, 0) == RU_OK) {
    uint32_t id;
    m.req.idx = m.idx;
    dz_cmd_err_t e = dz_cmd_submit(&a->cmd, &m.req, now_ms, &id);
    if (e != DZ_CMD_ACCEPTED) {
      res_msg_t r;
      memset(&r, 0, sizeof r);
      r.refused = true;
      r.err = e;
      ru_queue_send(a->res_q, &r, 0);
    }
  }
  dz_client_step(a->client, now_ms);
  dz_cmd_pump(&a->cmd, now_ms);
  atomic_store(&a->conn, (int)dz_client_state(a->client));
}

static void net_main(void *arg) {
  ru_dzapp_t *a = arg;
  while (!atomic_load(&a->net_stop)) {
    uint32_t now = ru_now_ms();
    ru_dzapp_net_step(a, now);
    uint32_t w = dz_client_ms_until_due(a->client, now);
    uint32_t w2 = dz_cmd_ms_until_due(&a->cmd, now);
    if (w2 < w) w = w2;
    if (w > 100) w = 100; /* also the latency for picking up a new request */
    if (w < 10) w = 10;
    ru_sleep_ms(w);
  }
}

bool ru_dzapp_net_start(ru_dzapp_t *a) {
  if (a->net) return true;
  atomic_store(&a->net_stop, false);
  a->net = ru_task_create("dz-net", net_main, a);
  return a->net != NULL;
}

void ru_dzapp_net_stop(ru_dzapp_t *a) {
  if (!a->net) return;
  atomic_store(&a->net_stop, true);
  ru_task_join(a->net);
  a->net = NULL;
}

/* ------------------------------------------------------------------ lifecycle and introspection */

ru_dzapp_t *ru_dzapp_create(dz_model_t *model, dz_client_t *client, bool allow_control) {
  ru_dzapp_t *a = calloc(1, sizeof *a);
  if (!a) return NULL;
  a->model = model;
  a->client = client;
  a->allow_control = allow_control;
  a->brightness = 80;
  ru_fb_init(&a->fb);
  a->req_q = ru_queue_create(sizeof(req_msg_t), 8);
  a->res_q = ru_queue_create(sizeof(res_msg_t), 16);
  if (!a->req_q || !a->res_q) { ru_dzapp_destroy(a); return NULL; }
  atomic_init(&a->net_stop, false);
  atomic_init(&a->conn, (int)DZ_CS_CONNECTING);
  dz_cmd_init(&a->cmd, client, model, allow_control, on_cmd_result, a);
  return a;
}

void ru_dzapp_destroy(ru_dzapp_t *a) {
  if (!a) return;
  ru_dzapp_net_stop(a);
  if (a->timer) lv_timer_delete(a->timer);
  free(a->ix);
  if (a->req_q) ru_queue_destroy(a->req_q);
  if (a->res_q) ru_queue_destroy(a->res_q);
  free(a);
}

ru_view_t ru_dzapp_view(const ru_dzapp_t *a) { return a->view; }
const char *ru_dzapp_current_idx(const ru_dzapp_t *a) { return a->cur_idx; }
const char *ru_dzapp_banner(const ru_dzapp_t *a) { return a->banner; }
lv_obj_t *ru_dzapp_screen(const ru_dzapp_t *a) { return a->scr; }
lv_obj_t *ru_dzapp_light(const ru_dzapp_t *a) { return a->light; }
size_t ru_dzapp_home_rows(const ru_dzapp_t *a) { return a->nrows; }
const char *ru_dzapp_toast(const ru_dzapp_t *a) { return ru_fb_text(&a->fb); }
const ru_fb_t *ru_dzapp_feedback(const ru_dzapp_t *a) { return &a->fb; }
void ru_dzapp_set_conn(ru_dzapp_t *a, dz_conn_state_t s) { atomic_store(&a->conn, (int)s); }
void ru_dzapp_set_brightness_hook(ru_dzapp_t *a, void (*fn)(int, void *), void *ud) { a->bright_fn = fn; a->bright_ud = ud; }
int ru_dzapp_brightness(const ru_dzapp_t *a) { return a->brightness; }
void ru_dzapp_set_cmd_spy(ru_dzapp_t *a, void (*fn)(const dz_cmd_req_t *, void *), void *ud) { a->spy_fn = fn; a->spy_ud = ud; }
const char *ru_dzapp_confirm_verb(const ru_dzapp_t *a) { return a->pend_verb; }
dz_cmd_kind_t ru_dzapp_confirm_kind(const ru_dzapp_t *a) { return a->pend.req.kind; }

bool ru_dzapp_row_info(const ru_dzapp_t *a, size_t n, ru_dzapp_row_info_t *out) {
  if (n >= a->nrows) return false;
  const row_t *r = &a->rows[n];
  memset(out, 0, sizeof *out);
  snprintf(out->idx, sizeof out->idx, "%s", r->idx);
  out->scene = r->scene;
  out->dot = RU_DOT_NONE;
  if (r->idx[0]) {
    dz_device_t d;
    ru_val_t v;
    ru_val_state_t st;
    char val[24];
    ru_dzapp_t *m = (ru_dzapp_t *)a;
    if (dz_model_get(m->model, r->idx, r->scene, &d, &v, &st, a->now)) {
      tile_content(a, &d, r->scene, &out->dot, out->sub, sizeof out->sub, val, sizeof val);
      if (d.cap == DZ_CAP_SCENE && a->fb.st == RU_FB_CONFIRMED && a->fb.scene && !strcmp(a->fb.idx, d.idx)) snprintf(out->sub, sizeof out->sub, "ACTIVATED");
      if (out->dot == RU_DOT_NONE) snprintf(out->sub, sizeof out->sub, "%s", val[0] ? val : "--");
    }
  }
  return true;
}

bool ru_dzapp_tap_row(ru_dzapp_t *a, size_t n) {
  if (a->view != RU_VIEW_HOME || n >= a->nrows) return false;
  lv_obj_send_event(a->row_tiles[n], LV_EVENT_CLICKED, NULL);
  return true;
}
