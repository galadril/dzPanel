#include "ru_widgets.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI_F 3.14159265f
#define GLOW_N 4
#define SWEEP 270
#define ROT 135

/* ---------------------------------------------------------------- helpers */

static void *wd_get(const lv_obj_t *o) { return lv_obj_get_user_data((lv_obj_t *)o); }

static void free_on_delete(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target(e);
  void *d = lv_obj_get_user_data(o);
  lv_obj_set_user_data(o, NULL);
  lv_free(d);
}

static void *wd_attach(lv_obj_t *o, size_t sz) {
  void *d = lv_malloc_zeroed(sz);
  lv_obj_set_user_data(o, d);
  lv_obj_add_event_cb(o, free_on_delete, LV_EVENT_DELETE, NULL);
  return d;
}

static lv_obj_t *circle(lv_obj_t *parent, int d, int cx, int cy) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, d, d);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(o, LV_ALIGN_CENTER, cx, cy);
  return o;
}

static void polar_off(float r, float deg_cw_from_3, int *dx, int *dy) {
  float a = deg_cw_from_3 * PI_F / 180.0f;
  *dx = (int)lroundf(r * cosf(a));
  *dy = (int)lroundf(r * sinf(a));
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* touch angle (deg, 0 = 3 o'clock, clockwise) and radius relative to an object's centre */
static bool touch_polar(lv_obj_t *o, float *deg, float *radius) {
  lv_indev_t *in = lv_indev_active();
  if (!in) return false;
  lv_point_t p;
  lv_indev_get_point(in, &p);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  float dx = (float)p.x - (float)(a.x1 + a.x2 + 1) / 2.0f;
  float dy = (float)p.y - (float)(a.y1 + a.y2 + 1) / 2.0f;
  float ang = atan2f(dy, dx) * 180.0f / PI_F;
  if (ang < 0) ang += 360.0f;
  *deg = ang;
  *radius = sqrtf(dx * dx + dy * dy);
  return true;
}

static void fmt_value(const ru_val_t *v, char *buf, size_t n) { ru_val_format(v, buf, n); }

/* ------------------------------------------------- ring with stepped glow */

typedef struct {
  lv_obj_t *box, *glow[GLOW_N], *arc;
  int d, w, max;
  bool has_knob;
  lv_color_t color;
  bool lit;
  bool applied, a_lit, a_glow; /* what ring_set_lit last put on the objects: identical calls touch nothing */
  lv_color_t a_color;
} ring_t;

/* LVGL invalidates on every style set, also for an identical value: set only what differs */
static void set_text_color(lv_obj_t *o, lv_color_t c) {
  if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c)) lv_obj_set_style_text_color(o, c, 0);
}
static void set_text_opa(lv_obj_t *o, lv_opa_t v) {
  if (lv_obj_get_style_text_opa(o, LV_PART_MAIN) != v) lv_obj_set_style_text_opa(o, v, 0);
}

/* lv_label_set_text invalidates even for identical text: only set what differs */
static void set_label(lv_obj_t *l, const char *txt) {
  const char *cur = lv_label_get_text(l);
  if (cur && !strcmp(cur, txt)) return;
  lv_label_set_text(l, txt);
}

static lv_obj_t *mk_arc(lv_obj_t *parent, int d, int w, int rot, int sweep, int max) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_remove_style_all(a);
  lv_obj_set_size(a, d, d);
  lv_obj_align(a, LV_ALIGN_CENTER, 0, 0);
  lv_arc_set_rotation(a, rot);
  lv_arc_set_bg_angles(a, 0, sweep);
  lv_arc_set_range(a, 0, max);
  lv_arc_set_value(a, 0);
  lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return a;
}

static int ring_glow_extent(void) { return GLOW_N * ru_u(1.1f); }

/* d = outer diameter of the ring itself; the box adds room for the glow */
static void ring_init(ring_t *r, lv_obj_t *parent, int d, int w, int rot, int sweep, int max, bool knob, bool interactive) {
  const ru_theme_t *t = ru_theme();
  int step = ru_u(1.1f);
  int S = d + 2 * ring_glow_extent();
  r->d = d; r->w = w; r->max = max; r->color = t->accent; r->lit = true;
  r->box = ru_cont_new(parent, 0, 0, S, S);
  lv_obj_align(r->box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_update_layout(r->box); /* resolve the alignment before children are placed */
  static const uint8_t glow_opa[GLOW_N] = { 56, 30, 16, 8 };
  for (int k = GLOW_N - 1; k >= 0; k--) {
    int gd = d + 2 * step * (k + 1);
    r->glow[k] = mk_arc(r->box, gd, w + 2 * step * (k + 1), rot, sweep, max);
    lv_obj_set_style_arc_opa(r->glow[k], LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(r->glow[k], t->accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(r->glow[k], glow_opa[k], LV_PART_INDICATOR);
  }
  r->arc = mk_arc(r->box, d, w, rot, sweep, max);
  lv_obj_set_style_arc_color(r->arc, t->track, LV_PART_MAIN);
  lv_obj_set_style_arc_color(r->arc, t->accent, LV_PART_INDICATOR);
  r->has_knob = knob;
  if (knob) {
    int pad = ru_u(0.9f);
    lv_obj_set_style_bg_color(r->arc, t->text, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(r->arc, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(r->arc, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(r->arc, pad, LV_PART_KNOB);
  }
  if (interactive) {
    lv_obj_add_flag(r->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_change_rate(r->arc, 5000); /* follow the finger, no rate limit */
  }
}

static void ring_set(ring_t *r, int v) {
  if (v < 0) v = 0;
  if (v > r->max) v = r->max;
  if (lv_arc_get_value(r->arc) == v) return;
  for (int k = 0; k < GLOW_N; k++) lv_arc_set_value(r->glow[k], v);
  lv_arc_set_value(r->arc, v);
}

static void ring_set_lit(ring_t *r, bool lit, lv_color_t c, bool glow) {
  r->lit = lit;
  r->color = c;
  bool gl = lit && glow;
  if (r->applied && r->a_lit == lit && r->a_glow == gl && lv_color_eq(r->a_color, c)) return;
  r->applied = true;
  r->a_lit = lit;
  r->a_glow = gl;
  r->a_color = c;
  lv_obj_set_style_arc_color(r->arc, c, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(r->arc, lit ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
  if (r->has_knob) lv_obj_set_style_bg_opa(r->arc, lit ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_KNOB);
  for (int k = 0; k < GLOW_N; k++) {
    lv_obj_set_style_arc_color(r->glow[k], c, LV_PART_INDICATOR);
    if (lit && glow) lv_obj_remove_flag(r->glow[k], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(r->glow[k], LV_OBJ_FLAG_HIDDEN);
  }
}

/* ------------------------------------------------------------ value tile */

typedef struct {
  lv_obj_t *label, *value, *dot;
  lv_color_t ok_color;
  lv_color_t applied_ok;
  bool inited;
  ru_val_state_t state;
  ru_tap_cb_t tap;
  void *tap_ud;
} tile_t;

static void tile_apply(lv_obj_t *tile, const char *text, ru_val_state_t st) {
  tile_t *d = wd_get(tile);
  const ru_theme_t *t = ru_theme();
  if (d->inited && d->state == st && lv_color_eq(d->applied_ok, d->ok_color) &&
      !strcmp(lv_label_get_text(d->value), text)) return;
  d->inited = true;
  d->applied_ok = d->ok_color;
  d->state = st;
  set_label(d->value, text);
  lv_color_t c = st == RU_VAL_OK ? d->ok_color : t->stale;
  set_text_color(d->value, c);
  set_text_opa(d->value, st == RU_VAL_OK ? LV_OPA_COVER : (st == RU_VAL_STALE ? LV_OPA_70 : LV_OPA_COVER));
  set_text_color(d->label, st == RU_VAL_OK ? t->text_dim : ru_dim(t->text_dim, 190));
  /* second channel: stale gets a border and a warn dot, unknown gets a border only */
  lv_obj_set_style_border_width(tile, st == RU_VAL_OK ? 0 : 1, 0);
  lv_obj_set_style_border_color(tile, st == RU_VAL_STALE ? ru_dim(t->warn, 150) : ru_dim(t->stale, 140), 0);
  if (st == RU_VAL_STALE) lv_obj_remove_flag(d->dot, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(d->dot, LV_OBJ_FLAG_HIDDEN);
}

static void tile_clicked(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target(e);
  tile_t *d = wd_get(o);
  if (d && d->tap) d->tap(o, d->tap_ud);
}

lv_obj_t *ru_value_tile_create(lv_obj_t *parent, int w, int h, const char *label) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, h / 2, 0);
  lv_obj_set_style_bg_color(o, t->surface, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  tile_t *d = wd_attach(o, sizeof(tile_t));
  d->ok_color = t->ok;
  int pad = h / 2 - ru_u(1);
  d->label = lv_label_create(o);
  lv_obj_set_style_text_font(d->label, ru_font_body(), 0);
  lv_obj_set_style_text_color(d->label, t->text_dim, 0);
  lv_label_set_text(d->label, label);
  lv_obj_align(d->label, LV_ALIGN_LEFT_MID, pad, 0);
  d->value = lv_label_create(o);
  lv_obj_set_style_text_font(d->value, ru_font_title(), 0);
  lv_label_set_text(d->value, "--");
  lv_obj_align(d->value, LV_ALIGN_RIGHT_MID, -pad, 0);
  d->dot = circle(o, ru_u(1.4f), 0, 0);
  lv_obj_set_style_bg_color(d->dot, t->warn, 0);
  lv_obj_set_style_bg_opa(d->dot, LV_OPA_COVER, 0);
  lv_obj_align(d->dot, LV_ALIGN_TOP_RIGHT, -ru_u(1.5f), ru_u(1.5f));
  lv_obj_add_flag(d->dot, LV_OBJ_FLAG_HIDDEN);
  tile_apply(o, "--", RU_VAL_UNKNOWN);
  return o;
}

void ru_value_tile_set(lv_obj_t *tile, const ru_val_t *v, uint32_t now_ms) {
  char buf[32];
  ru_val_state_t st = ru_val_state(v, now_ms);
  if (st == RU_VAL_UNKNOWN) snprintf(buf, sizeof buf, "--");
  else fmt_value(v, buf, sizeof buf);
  tile_apply(tile, buf, st);
}

void ru_value_tile_set_text(lv_obj_t *tile, const char *text, ru_val_state_t st) {
  tile_apply(tile, st == RU_VAL_UNKNOWN ? "--" : text, st);
}

void ru_value_tile_set_color(lv_obj_t *tile, lv_color_t c) {
  tile_t *d = wd_get(tile);
  if (lv_color_eq(d->ok_color, c)) return;
  d->ok_color = c;
  if (d->state == RU_VAL_OK) set_text_color(d->value, c);
}

ru_val_state_t ru_value_tile_state(const lv_obj_t *tile) { return ((tile_t *)wd_get(tile))->state; }

void ru_value_tile_on_tap(lv_obj_t *tile, ru_tap_cb_t cb, void *ud) {
  tile_t *d = wd_get(tile);
  d->tap = cb;
  d->tap_ud = ud;
  lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(tile, tile_clicked, LV_EVENT_CLICKED, NULL);
}

/* ------------------------------------------------------------ ring gauge */

typedef struct { ring_t ring; lv_obj_t *val, *cap; bool init; char txt[32]; int perm, st; uint32_t col; } gauge_t;

/* true when the gauge already shows exactly this (text, ring position, state, lit colour): then nothing is touched */
static bool gauge_same(gauge_t *g, const char *txt, int perm, int st) {
  uint32_t col = g->ring.color.red + 256u * g->ring.color.green + 65536u * g->ring.color.blue;
  if (g->init && g->perm == perm && g->st == st && g->col == col && !strcmp(g->txt, txt)) return true;
  g->init = true;
  g->perm = perm;
  g->st = st;
  g->col = col;
  snprintf(g->txt, sizeof g->txt, "%s", txt);
  return false;
}

lv_obj_t *ru_ring_gauge_create(lv_obj_t *parent, int d, const char *caption) {
  const ru_theme_t *t = ru_theme();
  gauge_t tmp;
  memset(&tmp, 0, sizeof tmp);
  ring_init(&tmp.ring, parent, d, ru_u(1.6f), ROT, SWEEP, 1000, false, false);
  lv_obj_t *box = tmp.ring.box;
  gauge_t *g = wd_attach(box, sizeof(gauge_t));
  g->ring = tmp.ring;
  int S = lv_obj_get_width(box);
  g->val = ru_label_new(box, "--", ru_font_title(), t->text, S / 2, S / 2 - ru_u(1));
  g->cap = ru_label_new(box, caption, ru_font_small(), t->text_dim, S / 2, S / 2 + ru_u(5.5f));
  ring_set_lit(&g->ring, false, t->accent, false);
  return box;
}

void ru_ring_gauge_set_color(lv_obj_t *box, lv_color_t c) {
  gauge_t *g = wd_get(box);
  g->ring.color = c;
  if (g->ring.lit && !(g->ring.applied && g->ring.a_lit && lv_color_eq(g->ring.a_color, c))) ring_set_lit(&g->ring, true, c, true);
}

void ru_ring_gauge_set(lv_obj_t *box, const ru_val_t *v, float min, float max, uint32_t now_ms) {
  gauge_t *g = wd_get(box);
  const ru_theme_t *t = ru_theme();
  char buf[32];
  ru_val_state_t st = ru_val_state(v, now_ms);
  if (st == RU_VAL_UNKNOWN || max <= min) {
    if (gauge_same(g, "--", -1, (int)RU_VAL_UNKNOWN)) return;
    set_label(g->val, "--");
    set_text_color(g->val, t->stale);
    ring_set(&g->ring, 0);
    ring_set_lit(&g->ring, false, g->ring.color, false);
    return;
  }
  fmt_value(v, buf, sizeof buf);
  float f = clampf((v->v - min) / (max - min), 0.0f, 1.0f);
  if (gauge_same(g, buf, (int)lroundf(f * 1000.0f), (int)st)) return;
  set_label(g->val, buf);
  ring_set(&g->ring, (int)lroundf(f * 1000.0f));
  if (st == RU_VAL_STALE) {
    set_text_color(g->val, t->stale);
    ring_set_lit(&g->ring, true, ru_dim(t->stale, 170), false);
  } else {
    set_text_color(g->val, t->text);
    ring_set_lit(&g->ring, true, g->ring.color, true);
  }
}

void ru_ring_gauge_set_display(lv_obj_t *box, const char *text, float frac, ru_val_state_t st) {
  gauge_t *g = wd_get(box);
  const ru_theme_t *t = ru_theme();
  if (st == RU_VAL_UNKNOWN || !text || !text[0]) {
    if (gauge_same(g, "--", -1, (int)RU_VAL_UNKNOWN)) return;
    set_label(g->val, "--");
    set_text_color(g->val, t->stale);
    ring_set(&g->ring, 0);
    ring_set_lit(&g->ring, false, g->ring.color, false);
    return;
  }
  if (gauge_same(g, text, frac < 0.0f ? -2 : (int)lroundf(clampf(frac, 0.0f, 1.0f) * 1000.0f), (int)st)) return;
  set_label(g->val, text);
  if (frac < 0.0f) { /* a value that cannot be scaled (unit unknown): the text only, an unlit ring */
    set_text_color(g->val, st == RU_VAL_STALE ? t->stale : t->text);
    ring_set(&g->ring, 0);
    ring_set_lit(&g->ring, false, g->ring.color, false);
    return;
  }
  ring_set(&g->ring, (int)lroundf(clampf(frac, 0.0f, 1.0f) * 1000.0f));
  if (st == RU_VAL_STALE) {
    set_text_color(g->val, t->stale);
    ring_set_lit(&g->ring, true, ru_dim(t->stale, 170), false);
  } else {
    set_text_color(g->val, t->text);
    ring_set_lit(&g->ring, true, g->ring.color, true);
  }
}

/* ------------------------------------------------------------ status rim */

typedef struct { lv_obj_t *line, *dot, *bead, *bead_halo; int D; int state; } rim_t;

lv_obj_t *ru_status_rim_create(lv_obj_t *parent) {
  const ru_theme_t *t = ru_theme();
  int D = ru_theme_geom()->diameter;
  lv_obj_t *root = ru_cont_new(parent, 0, 0, D, D);
  rim_t *r = wd_attach(root, sizeof(rim_t));
  r->D = D;
  r->line = circle(root, D - 2, 0, 0);
  lv_obj_set_style_border_width(r->line, 1, 0);
  lv_obj_set_style_border_color(r->line, t->hairline, 0);
  r->dot = circle(root, ru_u(1.6f), 0, 0);
  lv_obj_set_style_bg_opa(r->dot, LV_OPA_COVER, 0);
  int dx, dy;
  polar_off(D / 2.0f - ru_u(1.6f), 270.0f + 35.0f, &dx, &dy);
  lv_obj_align(r->dot, LV_ALIGN_CENTER, dx, dy);
  r->bead_halo = circle(root, ru_u(3.2f), 0, 0);
  lv_obj_set_style_bg_color(r->bead_halo, t->accent, 0);
  lv_obj_set_style_bg_opa(r->bead_halo, LV_OPA_30, 0);
  r->bead = circle(root, ru_u(1.6f), 0, 0);
  lv_obj_set_style_bg_color(r->bead, t->accent, 0);
  lv_obj_set_style_bg_opa(r->bead, LV_OPA_COVER, 0);
  lv_obj_add_flag(r->bead, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(r->bead_halo, LV_OBJ_FLAG_HIDDEN);
  ru_status_rim_set_state(root, RU_RIM_OK);
  return root;
}

void ru_status_rim_set_state(lv_obj_t *root, ru_rim_state_t s) {
  rim_t *r = wd_get(root);
  const ru_theme_t *t = ru_theme();
  if (r->state == (int)s + 1) return;
  r->state = (int)s + 1;
  lv_color_t c = s == RU_RIM_OK ? t->ok : s == RU_RIM_STALE ? t->warn : s == RU_RIM_ERROR ? t->error : t->stale;
  lv_obj_set_style_bg_color(r->dot, c, 0);
}

void ru_status_rim_set_color(lv_obj_t *root, lv_color_t c) {
  rim_t *r = wd_get(root);
  lv_obj_set_style_border_color(r->line, c, 0);
}

void ru_status_rim_set_bead(lv_obj_t *root, bool visible, float deg_from_top) {
  rim_t *r = wd_get(root);
  if (!visible) {
    lv_obj_add_flag(r->bead, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(r->bead_halo, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  int dx, dy;
  polar_off(r->D / 2.0f - 1.5f, deg_from_top - 90.0f, &dx, &dy);
  lv_obj_align(r->bead, LV_ALIGN_CENTER, dx, dy);
  polar_off(r->D / 2.0f - ru_u(1.6f), deg_from_top - 90.0f, &dx, &dy);
  lv_obj_align(r->bead_halo, LV_ALIGN_CENTER, dx, dy);
  lv_obj_remove_flag(r->bead, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(r->bead_halo, LV_OBJ_FLAG_HIDDEN);
}

/* --------------------------------------------------------------- dimmer */

typedef struct {
  ring_t ring;
  lv_obj_t *hero, *cap;
  int max;
  bool unknown;
  ru_int_cb_t cb;
  void *ud;
} dimmer_t;

static void dimmer_text(dimmer_t *d, int v) {
  char buf[16];
  snprintf(buf, sizeof buf, "%d%%", d->max == 100 ? v : (int)lroundf(100.0f * (float)v / (float)d->max));
  lv_label_set_text(d->hero, buf);
}

static void dimmer_event(lv_event_t *e) {
  lv_obj_t *arc = lv_event_get_target(e);
  lv_obj_t *box = lv_obj_get_parent(arc);
  dimmer_t *d = wd_get(box);
  lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_VALUE_CHANGED) {
    int v = lv_arc_get_value(arc);
    for (int k = 0; k < GLOW_N; k++) lv_arc_set_value(d->ring.glow[k], v);
    if (d->unknown) { d->unknown = false; ring_set_lit(&d->ring, true, ru_theme()->accent, true); }
    dimmer_text(d, v);
    if (d->cb) d->cb(box, v, false, d->ud);
  } else if (c == LV_EVENT_RELEASED && !d->unknown) {
    if (d->cb) d->cb(box, lv_arc_get_value(arc), true, d->ud);
  }
}

lv_obj_t *ru_dimmer_create(lv_obj_t *parent, int d, int max, const char *caption) {
  const ru_theme_t *t = ru_theme();
  if (max < 1) max = 1;
  dimmer_t tmp;
  memset(&tmp, 0, sizeof tmp);
  ring_init(&tmp.ring, parent, d, ru_u(2.2f), ROT, SWEEP, max, true, true);
  lv_obj_t *box = tmp.ring.box;
  dimmer_t *w = wd_attach(box, sizeof(dimmer_t));
  w->ring = tmp.ring;
  w->max = max;
  int S = lv_obj_get_width(box);
  w->hero = ru_label_new(box, "0%", ru_font_hero(), t->text, S / 2, S / 2 - ru_u(2));
  w->cap = ru_label_new(box, caption, ru_font_body(), t->text_dim, S / 2, S / 2 + ru_u(11));
  lv_obj_add_event_cb(w->ring.arc, dimmer_event, LV_EVENT_ALL, NULL);
  ring_set_lit(&w->ring, true, t->accent, true);
  return box;
}

void ru_dimmer_set(lv_obj_t *box, int v) {
  dimmer_t *d = wd_get(box);
  if (v < 0) v = 0;
  if (v > d->max) v = d->max;
  d->unknown = false;
  ring_set(&d->ring, v);
  ring_set_lit(&d->ring, v > 0, ru_theme()->accent, true);
  if (v == 0) lv_obj_set_style_bg_opa(d->ring.arc, LV_OPA_COVER, LV_PART_KNOB);
  dimmer_text(d, v);
}

int ru_dimmer_get(const lv_obj_t *box) { return lv_arc_get_value(((dimmer_t *)wd_get(box))->ring.arc); }

void ru_dimmer_set_unknown(lv_obj_t *box) {
  dimmer_t *d = wd_get(box);
  d->unknown = true;
  ring_set(&d->ring, 0);
  ring_set_lit(&d->ring, false, ru_theme()->stale, false);
  lv_label_set_text(d->hero, "--");
}

void ru_dimmer_on_change(lv_obj_t *box, ru_int_cb_t cb, void *ud) {
  dimmer_t *d = wd_get(box);
  d->cb = cb;
  d->ud = ud;
}

/* ----------------------------------------------------------------- icons */

lv_obj_t *ru_icon_dot(lv_obj_t *parent, int size, lv_color_t c) {
  lv_obj_t *o = circle(parent, size, 0, 0);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}

lv_obj_t *ru_icon_power(lv_obj_t *parent, int size, lv_color_t c) {
  int w = size / 8 > 1 ? size / 8 : 2;
  lv_obj_t *box = ru_cont_new(parent, 0, 0, size, size);
  lv_obj_t *a = mk_arc(box, size, w, 310, 280, 100);
  lv_obj_set_style_arc_color(a, c, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_t *bar = lv_obj_create(box);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, w, size * 11 / 20);
  lv_obj_set_style_radius(bar, w / 2 + 1, 0);
  lv_obj_set_style_bg_color(bar, c, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  return box;
}

lv_obj_t *ru_icon_fan(lv_obj_t *parent, int size, lv_color_t c) {
  int w = size / 9 > 1 ? size / 9 : 2;
  lv_obj_t *box = ru_cont_new(parent, 0, 0, size, size);
  int bd = size * 11 / 20;
  for (int k = 0; k < 3; k++) {
    int dx, dy;
    polar_off(size * 0.24f, 120.0f * (float)k - 60.0f, &dx, &dy);
    lv_obj_t *a = mk_arc(box, bd, w, 120 * k + 200, 110, 100);
    lv_obj_set_style_arc_color(a, c, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_align(a, LV_ALIGN_CENTER, dx, dy);
  }
  lv_obj_t *hub = ru_icon_dot(box, size / 6, ru_theme()->text);
  lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);
  return box;
}

/* --------------------------------------------------------------- toggle */

typedef struct {
  lv_obj_t *ringc, *glow[3], *face, *icon_box, *state_lbl;
  ru_onoff_t st;
  ru_bool_cb_t cb;
  void *ud;
  int d;
} toggle_t;

static void toggle_paint(lv_obj_t *box) {
  toggle_t *w = wd_get(box);
  const ru_theme_t *t = ru_theme();
  lv_color_t c = w->st == RU_ONOFF_ON ? t->accent : (w->st == RU_ONOFF_OFF ? ru_dim(t->text_dim, 150) : ru_dim(t->stale, 110));
  lv_obj_set_style_border_color(w->ringc, w->st == RU_ONOFF_ON ? t->accent : t->track, 0);
  lv_obj_set_style_border_color(w->face, w->st == RU_ONOFF_ON ? ru_dim(t->accent, 40) : t->track, 0);
  static const uint8_t opa[3] = { 64, 34, 14 };
  for (int k = 0; k < 3; k++) {
    if (w->st == RU_ONOFF_ON) {
      lv_obj_remove_flag(w->glow[k], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_border_color(w->glow[k], t->accent, 0);
      lv_obj_set_style_border_opa(w->glow[k], opa[k], 0);
    } else lv_obj_add_flag(w->glow[k], LV_OBJ_FLAG_HIDDEN);
  }
  /* icon recoloured by rebuilding its two parts */
  lv_obj_t *arc = lv_obj_get_child(w->icon_box, 0), *bar = lv_obj_get_child(w->icon_box, 1);
  lv_obj_set_style_arc_color(arc, c, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, c, 0);
  lv_label_set_text(w->state_lbl, w->st == RU_ONOFF_ON ? "ON" : (w->st == RU_ONOFF_OFF ? "OFF" : "--"));
  lv_obj_set_style_text_color(w->state_lbl, w->st == RU_ONOFF_ON ? t->text : t->text_dim, 0);
}

static void toggle_clicked(lv_event_t *e) {
  lv_obj_t *box = lv_event_get_target(e);
  toggle_t *w = wd_get(box);
  if (w->st == RU_ONOFF_UNKNOWN) return; /* never guess the state */
  if (w->cb) w->cb(box, w->st == RU_ONOFF_OFF, w->ud);
}

lv_obj_t *ru_toggle_create(lv_obj_t *parent, int d) {
  const ru_theme_t *t = ru_theme();
  int step = ru_u(1.2f);
  int S = d + 2 * 3 * step;
  lv_obj_t *box = ru_cont_new(parent, 0, 0, S, S);
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_update_layout(box);
  lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
  toggle_t *w = wd_attach(box, sizeof(toggle_t));
  w->d = d;
  for (int k = 2; k >= 0; k--) {
    w->glow[k] = circle(box, d + 2 * step * (k + 1), 0, 0);
    lv_obj_set_style_border_width(w->glow[k], step + 1, 0);
  }
  w->ringc = circle(box, d, 0, 0);
  lv_obj_set_style_border_width(w->ringc, ru_u(2.4f), 0);
  w->face = circle(box, d - 2 * ru_u(2.4f), 0, 0);
  lv_obj_set_style_bg_color(w->face, t->surface, 0);
  lv_obj_set_style_bg_opa(w->face, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(w->face, 1, 0);
  int isz = d * 30 / 100;
  w->icon_box = ru_icon_power(box, isz, t->text);
  lv_obj_align(w->icon_box, LV_ALIGN_CENTER, 0, -d * 8 / 100);
  w->state_lbl = ru_label_new(box, "OFF", ru_font_title(), t->text, S / 2, S / 2 + d * 18 / 100);
  w->st = RU_ONOFF_OFF;
  toggle_paint(box);
  lv_obj_add_event_cb(box, toggle_clicked, LV_EVENT_CLICKED, NULL);
  return box;
}

void ru_toggle_set(lv_obj_t *box, ru_onoff_t s) {
  toggle_t *w = wd_get(box);
  w->st = s;
  toggle_paint(box);
}
ru_onoff_t ru_toggle_get(const lv_obj_t *box) { return ((toggle_t *)wd_get(box))->st; }
void ru_toggle_on_change(lv_obj_t *box, ru_bool_cb_t cb, void *ud) {
  toggle_t *w = wd_get(box);
  w->cb = cb;
  w->ud = ud;
}

/* ------------------------------------------------------------- selector */

#define SEL_MAX 12
#define SEL_A0 170.0f
#define SEL_A1 370.0f

typedef struct {
  ring_t ring;
  lv_obj_t *lbl[SEL_MAX], *name, *centre;
  char *names[SEL_MAX];
  int n, idx;
  float r_lab;
  ru_int_cb_t cb;
  void *ud;
} sel_t;

static float sel_angle(const sel_t *s, int i) {
  return s->n == 1 ? 270.0f : SEL_A0 + (SEL_A1 - SEL_A0) * (float)i / (float)(s->n - 1);
}

static void sel_paint(lv_obj_t *box) {
  sel_t *s = wd_get(box);
  const ru_theme_t *t = ru_theme();
  float a = sel_angle(s, s->idx);
  ring_set(&s->ring, (int)lroundf(a - (float)ROT));
  ring_set_lit(&s->ring, true, t->info, true);
  for (int i = 0; i < s->n; i++) {
    bool on = i == s->idx;
    lv_obj_set_style_text_color(s->lbl[i], on ? t->text : t->text_dim, 0);
    lv_obj_set_style_text_font(s->lbl[i], on ? ru_font_body() : ru_font_small(), 0);
  }
  lv_label_set_text(s->name, s->names[s->idx]);
}

static void sel_free(lv_event_t *e) {
  sel_t *s = wd_get(lv_event_get_target(e));
  if (!s) return;
  for (int i = 0; i < s->n; i++) lv_free(s->names[i]);
}

static void sel_touch(lv_event_t *e) {
  lv_obj_t *box = lv_event_get_target(e);
  sel_t *s = wd_get(box);
  lv_event_code_t c = lv_event_get_code(e);
  float deg, r;
  if (c == LV_EVENT_RELEASED) { if (s->cb) s->cb(box, s->idx, true, s->ud); return; }
  if (!touch_polar(box, &deg, &r)) return;
  if (r < s->r_lab * 0.45f) return; /* centre belongs to icons */
  int best = s->idx;
  float bd = 1e9f;
  for (int i = 0; i < s->n; i++) {
    float a = sel_angle(s, i);
    float df = fabsf(fmodf(deg - a + 540.0f, 360.0f) - 180.0f);
    if (df < bd) { bd = df; best = i; }
  }
  if (best != s->idx) {
    s->idx = best;
    sel_paint(box);
    if (s->cb) s->cb(box, best, false, s->ud);
  }
}

lv_obj_t *ru_selector_create(lv_obj_t *parent, int d, const char *const *names, int n) {
  const ru_theme_t *t = ru_theme();
  if (n < 1) n = 1;
  if (n > SEL_MAX) n = SEL_MAX;
  sel_t tmp;
  memset(&tmp, 0, sizeof tmp);
  int w = ru_u(2.2f);
  ring_init(&tmp.ring, parent, d, w, ROT, SWEEP, SWEEP, false, false);
  lv_obj_t *box = tmp.ring.box;
  lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
  /* delete handlers run in registration order: names must go before the struct */
  lv_obj_add_event_cb(box, sel_free, LV_EVENT_DELETE, NULL);
  sel_t *s = wd_attach(box, sizeof(sel_t));
  s->ring = tmp.ring;
  s->n = n;
  s->r_lab = d / 2.0f - (float)w - (float)ru_u(5.5f);
  int S = lv_obj_get_width(box);
  for (int i = 0; i < n; i++) {
    const char *nm = names ? names[i] : "";
    size_t l = strlen(nm) + 1;
    s->names[i] = lv_malloc(l);
    memcpy(s->names[i], nm, l);
    int dx, dy;
    polar_off(s->r_lab, sel_angle(s, i), &dx, &dy);
    s->lbl[i] = ru_label_new(box, nm, ru_font_small(), t->text_dim, S / 2 + dx, S / 2 + dy);
  }
  s->centre = ru_cont_new(box, 0, 0, d * 2 / 5, d * 2 / 5);
  lv_obj_align(s->centre, LV_ALIGN_CENTER, 0, -d * 4 / 100);
  s->name = ru_label_new(box, "", ru_font_title(), t->text, S / 2, S / 2 + d * 20 / 100);
  lv_obj_add_event_cb(box, sel_touch, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(box, sel_touch, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(box, sel_touch, LV_EVENT_RELEASED, NULL);
  sel_paint(box);
  return box;
}

void ru_selector_set(lv_obj_t *box, int i) {
  sel_t *s = wd_get(box);
  if (i < 0 || i >= s->n) return;
  s->idx = i;
  sel_paint(box);
}
int ru_selector_get(const lv_obj_t *box) { return ((sel_t *)wd_get(box))->idx; }
void ru_selector_on_change(lv_obj_t *box, ru_int_cb_t cb, void *ud) {
  sel_t *s = wd_get(box);
  s->cb = cb;
  s->ud = ud;
}
lv_obj_t *ru_selector_centre(lv_obj_t *box) { return ((sel_t *)wd_get(box))->centre; }

static lv_obj_t *bar(lv_obj_t *parent, int w, int h, lv_color_t c, int dx, int dy) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(o, LV_ALIGN_CENTER, dx, dy);
  return o;
}

/* chevron drawn as a polyline; pts must outlive the object */
static void chevron(lv_obj_t *parent, lv_point_precise_t *pts, int size, bool up, lv_color_t c) {
  int h = size / 4, w = size / 2;
  pts[0].x = 0; pts[0].y = up ? h : 0;
  pts[1].x = w / 2; pts[1].y = up ? 0 : h;
  pts[2].x = w; pts[2].y = up ? h : 0;
  lv_obj_t *l = lv_line_create(parent);
  lv_line_set_points(l, pts, 3);
  lv_obj_set_style_line_width(l, ru_u(1.4f), 0);
  lv_obj_set_style_line_color(l, c, 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
}

/* --------------------------------------------------------------- blinds */

#define SLATS 9

typedef struct {
  lv_obj_t *win, *slat[SLATS], *lbl;
  lv_point_precise_t up_pts[3], dn_pts[3];
  ru_blinds_cb_t cb;
  void *ud;
} blinds_t;

static void blinds_btn(lv_event_t *e) {
  lv_obj_t *btn = lv_event_get_target(e);
  lv_obj_t *box = lv_obj_get_parent(btn);
  blinds_t *b = wd_get(box);
  if (b->cb) b->cb(box, (ru_blinds_cmd_t)(intptr_t)lv_event_get_user_data(e), b->ud);
}

static lv_obj_t *round_btn(lv_obj_t *parent, int d, int x, int y, ru_blinds_cmd_t cmd) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *b = circle(parent, d, 0, 0);
  lv_obj_set_align(b, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(b, x - d / 2, y - d / 2);
  lv_obj_set_style_border_width(b, ru_u(0.5f) > 1 ? ru_u(0.5f) : 2, 0);
  lv_obj_set_style_border_color(b, ru_dim(t->accent, 170), 0);
  lv_obj_set_style_bg_color(b, t->surface, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, ru_dim(t->accent, 60), LV_STATE_PRESSED);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(b, blinds_btn, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);
  return b;
}

lv_obj_t *ru_blinds_create(lv_obj_t *parent, int w, int h) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *box = ru_cont_new(parent, 0, 0, w, h);
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_update_layout(box);
  blinds_t *b = wd_attach(box, sizeof(blinds_t));
  int bd = ru_u(12.5f);
  int ww = w * 55 / 100, wh = h - bd - ru_u(9);
  b->win = lv_obj_create(box);
  lv_obj_remove_style_all(b->win);
  lv_obj_set_size(b->win, ww, wh);
  lv_obj_align(b->win, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_radius(b->win, ru_u(1.2f), 0);
  lv_obj_set_style_bg_color(b->win, t->surface, 0);
  lv_obj_set_style_bg_opa(b->win, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(b->win, 2, 0);
  lv_obj_set_style_border_color(b->win, ru_dim(t->accent, 170), 0);
  lv_obj_remove_flag(b->win, LV_OBJ_FLAG_SCROLLABLE);
  int pad = ru_u(1);
  int pitch = (wh - 2 * pad) / SLATS;
  for (int i = 0; i < SLATS; i++) {
    b->slat[i] = lv_obj_create(b->win);
    lv_obj_remove_style_all(b->slat[i]);
    lv_obj_set_size(b->slat[i], ww - 2 * pad - 4, ru_u(1.7f));
    lv_obj_set_pos(b->slat[i], pad, pad + i * pitch);
    lv_obj_set_style_radius(b->slat[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b->slat[i], ru_dim(t->accent, 200), 0);
    lv_obj_set_style_bg_opa(b->slat[i], LV_OPA_COVER, 0);
    lv_obj_add_flag(b->slat[i], LV_OBJ_FLAG_HIDDEN);
  }
  b->lbl = ru_label_new(box, "--", ru_font_title(), t->text, w / 2, wh + ru_u(4));
  int by = h - bd / 2, gap = ru_u(20);
  lv_obj_t *up = round_btn(box, bd, w / 2 - gap, by, RU_BLINDS_UP);
  lv_obj_t *st = round_btn(box, bd, w / 2, by, RU_BLINDS_STOP);
  lv_obj_t *dn = round_btn(box, bd, w / 2 + gap, by, RU_BLINDS_DOWN);
  chevron(up, b->up_pts, bd * 9 / 10, true, t->text);
  chevron(dn, b->dn_pts, bd * 9 / 10, false, t->text);
  lv_obj_t *sq = lv_obj_create(st);
  lv_obj_remove_style_all(sq);
  lv_obj_set_size(sq, bd * 3 / 10, bd * 3 / 10);
  lv_obj_set_style_radius(sq, 2, 0);
  lv_obj_set_style_bg_color(sq, t->text, 0);
  lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
  lv_obj_align(sq, LV_ALIGN_CENTER, 0, 0);
  return box;
}

void ru_blinds_set(lv_obj_t *box, const ru_val_t *closed, uint32_t now_ms) {
  blinds_t *b = wd_get(box);
  const ru_theme_t *t = ru_theme();
  ru_val_state_t st = ru_val_state(closed, now_ms);
  char buf[32];
  int shown = 0;
  if (st == RU_VAL_UNKNOWN) {
    snprintf(buf, sizeof buf, "-- CLOSED");
  } else {
    float p = clampf(closed->v, 0.0f, 100.0f);
    snprintf(buf, sizeof buf, "%.0f%% CLOSED", p);
    shown = (int)lroundf((float)SLATS * p / 100.0f);
  }
  for (int i = 0; i < SLATS; i++) {
    if (i < shown) lv_obj_remove_flag(b->slat[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(b->slat[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(b->slat[i], st == RU_VAL_STALE ? ru_dim(t->stale, 170) : ru_dim(t->accent, 200), 0);
  }
  lv_label_set_text(b->lbl, buf);
  lv_obj_set_style_text_color(b->lbl, st == RU_VAL_OK ? t->text : t->stale, 0);
}

void ru_blinds_on_cmd(lv_obj_t *box, ru_blinds_cb_t cb, void *ud) {
  blinds_t *b = wd_get(box);
  b->cb = cb;
  b->ud = ud;
}

/* ---------------------------------------------------------------- hold */

typedef struct {
  ring_t ring;
  lv_obj_t *lbl;
  lv_timer_t *timer;
  uint32_t hold_ms, t0;
  bool holding, done;
  ru_hold_cb_t cb;
  void *ud;
} hold_t;

static void hold_paint(hold_t *h, int permille) {
  const ru_theme_t *t = ru_theme();
  ring_set(&h->ring, permille);
  ring_set_lit(&h->ring, true, t->error, permille > 0);
  /* the bead rides the ring: visible at zero too, so the control reads as a ring */
  lv_obj_set_style_bg_opa(h->ring.arc, LV_OPA_COVER, LV_PART_KNOB);
}

static void hold_timer(lv_timer_t *tm) {
  lv_obj_t *box = lv_timer_get_user_data(tm);
  hold_t *h = wd_get(box);
  if (!h->holding) return;
  uint32_t el = lv_tick_elaps(h->t0);
  int pm = h->hold_ms ? (int)(el * 1000u / h->hold_ms) : 1000;
  if (pm >= 1000) {
    hold_paint(h, 1000);
    h->holding = false;
    h->done = true;
    lv_timer_pause(h->timer);
    if (h->cb) h->cb(box, RU_HOLD_CONFIRMED, h->ud);
  } else hold_paint(h, pm);
}

static void hold_press(lv_event_t *e) {
  lv_obj_t *box = lv_event_get_target(e);
  hold_t *h = wd_get(box);
  lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_PRESSED) {
    h->holding = true;
    h->done = false;
    h->t0 = lv_tick_get();
    lv_timer_resume(h->timer);
  } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
    bool was = h->holding;
    h->holding = false;
    lv_timer_pause(h->timer);
    if (was) {
      hold_paint(h, 0);
      if (h->cb) h->cb(box, RU_HOLD_CANCELLED, h->ud);
    } else if (h->done) {
      h->done = false;
      hold_paint(h, 0);
    }
  }
}

static void hold_free(lv_event_t *e) {
  hold_t *h = wd_get(lv_event_get_target(e));
  if (h && h->timer) lv_timer_delete(h->timer);
}

lv_obj_t *ru_hold_confirm_create(lv_obj_t *parent, int d, const char *text, uint32_t hold_ms) {
  const ru_theme_t *t = ru_theme();
  hold_t tmp;
  memset(&tmp, 0, sizeof tmp);
  ring_init(&tmp.ring, parent, d, ru_u(2.6f), 270, 360, 1000, true, false);
  lv_obj_t *box = tmp.ring.box;
  lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(box, hold_free, LV_EVENT_DELETE, NULL);
  hold_t *h = wd_attach(box, sizeof(hold_t));
  h->ring = tmp.ring;
  h->hold_ms = hold_ms;
  int S = lv_obj_get_width(box);
  h->lbl = ru_label_new(box, text, ru_font_title(), t->error, S / 2, S / 2);
  h->timer = lv_timer_create(hold_timer, 16, box);
  lv_timer_pause(h->timer);
  hold_paint(h, 0);
  lv_obj_add_event_cb(box, hold_press, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(box, hold_press, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(box, hold_press, LV_EVENT_PRESS_LOST, NULL);
  return box;
}

void ru_hold_confirm_set_progress(lv_obj_t *box, int pm) {
  hold_t *h = wd_get(box);
  if (pm < 0) pm = 0;
  if (pm > 1000) pm = 1000;
  hold_paint(h, pm);
}
int ru_hold_confirm_progress(const lv_obj_t *box) { return lv_arc_get_value(((hold_t *)wd_get(box))->ring.arc); }
void ru_hold_confirm_on_event(lv_obj_t *box, ru_hold_cb_t cb, void *ud) {
  hold_t *h = wd_get(box);
  h->cb = cb;
  h->ud = ud;
}

/* --------------------------------------------------------------- scene */

typedef struct { ru_tap_cb_t cb; void *ud; } scene_t;

static void scene_clicked(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target(e);
  scene_t *s = wd_get(o);
  if (s->cb) s->cb(o, s->ud);
}

lv_obj_t *ru_scene_button_create(lv_obj_t *parent, int w, int h, const char *name, const char *kind, lv_color_t dot) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, h * 3 / 10, 0);
  lv_obj_set_style_bg_color(o, t->surface, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(0x23272E), LV_STATE_PRESSED);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  wd_attach(o, sizeof(scene_t));
  lv_obj_t *dt = ru_icon_dot(o, ru_u(5.5f), dot);
  lv_obj_align(dt, LV_ALIGN_CENTER, 0, -h * 26 / 100);
  ru_label_new(o, name, ru_font_body(), t->text, w / 2, h * 60 / 100);
  ru_label_new(o, kind, ru_font_small(), t->text_dim, w / 2, h * 82 / 100);
  lv_obj_add_event_cb(o, scene_clicked, LV_EVENT_CLICKED, NULL);
  return o;
}

void ru_scene_button_on_tap(lv_obj_t *o, ru_tap_cb_t cb, void *ud) {
  scene_t *s = wd_get(o);
  s->cb = cb;
  s->ud = ud;
}

/* ----------------------------------------------------------- thermostat */

typedef struct {
  ring_t ring;
  lv_obj_t *hero, *now, *mode, *row, *bead, *btn[2];
  char unit[12];
  float min, max, step;
  float cur; /* the shown setpoint: exact as given (66.2 stays 66.2), on the step grid only after a drag */
  int steps;
  ru_float_cb_t cb;
  void *ud;
} thermo_t;

static float thermo_value(const thermo_t *t, int i) { return t->min + (float)i * t->step; }

static void thermo_text(thermo_t *t, float v) {
  char buf[24];
  bool attach = !t->unit[0] || t->unit[0] == '%' || (unsigned char)t->unit[0] == 0xC2;
  snprintf(buf, sizeof buf, attach ? "%.1f%s" : "%.1f %s", (double)v, t->unit);
  lv_label_set_text(t->hero, buf);
}

static void thermo_arc(lv_event_t *e) {
  lv_obj_t *arc = lv_event_get_target(e);
  lv_obj_t *box = lv_obj_get_parent(arc);
  thermo_t *t = wd_get(box);
  int v = lv_arc_get_value(arc);
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    for (int k = 0; k < GLOW_N; k++) lv_arc_set_value(t->ring.glow[k], v);
    t->cur = thermo_value(t, v);
    thermo_text(t, t->cur);
    if (t->cb) t->cb(box, t->cur, false, t->ud);
  } else if (t->cb) t->cb(box, t->cur, true, t->ud);
}

static void thermo_btn(lv_event_t *e) {
  lv_obj_t *btn = lv_event_get_target(e);
  lv_obj_t *box = lv_obj_get_parent(btn);
  thermo_t *t = wd_get(box);
  float nv = clampf(t->cur + (float)(intptr_t)lv_event_get_user_data(e) * t->step, t->min, t->max);
  ring_set(&t->ring, (int)lroundf((nv - t->min) / t->step));
  t->cur = nv;
  thermo_text(t, nv);
  if (t->cb) t->cb(box, nv, true, t->ud);
}

static void thermo_place_bead(thermo_t *t, float cur) {
  float f = clampf((cur - t->min) / (t->max - t->min), 0.0f, 1.0f);
  int dx, dy;
  polar_off((float)(t->ring.d - t->ring.w) / 2.0f, (float)ROT + f * (float)SWEEP, &dx, &dy);
  lv_obj_align(t->bead, LV_ALIGN_CENTER, dx, dy);
}

lv_obj_t *ru_thermostat_create(lv_obj_t *parent, int d, float min, float max, float step) {
  const ru_theme_t *th = ru_theme();
  if (step <= 0 || max <= min) { step = 0.5f; min = 5; max = 30; }
  int steps = (int)lroundf((max - min) / step);
  thermo_t tmp;
  memset(&tmp, 0, sizeof tmp);
  ring_init(&tmp.ring, parent, d, ru_u(2.2f), ROT, SWEEP, steps, true, true);
  lv_obj_t *box = tmp.ring.box;
  thermo_t *t = wd_attach(box, sizeof(thermo_t));
  t->ring = tmp.ring;
  t->min = min; t->max = max; t->step = step; t->steps = steps;
  snprintf(t->unit, sizeof t->unit, "\xC2\xB0"); /* default: a bare degree sign, as before; screens set the real unit */
  int S = lv_obj_get_width(box);
  t->hero = ru_label_new(box, "--", ru_font_hero(), th->text, S / 2, S / 2 - ru_u(7));
  t->row = lv_obj_create(box);
  lv_obj_remove_style_all(t->row);
  lv_obj_set_size(t->row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(t->row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(t->row, ru_u(2), 0);
  lv_obj_remove_flag(t->row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(t->row, LV_ALIGN_CENTER, 0, ru_u(4));
  t->now = lv_label_create(t->row);
  lv_obj_set_style_text_font(t->now, ru_font_body(), 0);
  lv_obj_set_style_text_color(t->now, th->text_dim, 0);
  lv_label_set_text(t->now, "NOW --");
  t->mode = lv_label_create(t->row);
  lv_obj_set_style_text_font(t->mode, ru_font_body(), 0);
  lv_label_set_text(t->mode, "");
  t->bead = ru_icon_dot(box, ru_u(2.2f), th->info);
  lv_obj_add_flag(t->bead, LV_OBJ_FLAG_HIDDEN);
  int bd = ru_u(12.5f);
  for (int s = -1; s <= 1; s += 2) {
    lv_obj_t *b = circle(box, bd, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, s * ru_u(14), ru_u(24));
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, th->text_dim, 0);
    lv_obj_set_style_bg_color(b, th->surface, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, ru_dim(th->accent, 60), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    t->btn[s > 0] = b;
    lv_obj_add_event_cb(b, thermo_btn, LV_EVENT_CLICKED, (void *)(intptr_t)s);
    bar(b, bd * 40 / 100, ru_u(1.2f) > 3 ? ru_u(1.2f) : 3, th->text, 0, 0);
    if (s > 0) bar(b, ru_u(1.2f) > 3 ? ru_u(1.2f) : 3, bd * 40 / 100, th->text, 0, 0);
  }
  lv_obj_add_event_cb(t->ring.arc, thermo_arc, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(t->ring.arc, thermo_arc, LV_EVENT_RELEASED, NULL);
  ring_set_lit(&t->ring, true, th->accent, true);
  ru_thermostat_set_setpoint(box, min);
  return box;
}

void ru_thermostat_set_setpoint(lv_obj_t *box, float sp) {
  thermo_t *t = wd_get(box);
  t->cur = clampf(sp, t->min, t->max);
  ring_set(&t->ring, (int)lroundf((t->cur - t->min) / t->step));
  thermo_text(t, t->cur);
}

float ru_thermostat_get_setpoint(const lv_obj_t *box) {
  thermo_t *t = wd_get(box);
  return t->cur;
}

void ru_thermostat_set_current(lv_obj_t *box, const ru_val_t *cur, uint32_t now_ms) {
  thermo_t *t = wd_get(box);
  const ru_theme_t *th = ru_theme();
  ru_val_state_t st = ru_val_state(cur, now_ms);
  char buf[40], num[24];
  if (st == RU_VAL_UNKNOWN) {
    lv_label_set_text(t->now, "NOW --");
    lv_obj_add_flag(t->bead, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  snprintf(num, sizeof num, "%.1f", (double)cur->v);
  bool attach = !t->unit[0] || t->unit[0] == '%' || (unsigned char)t->unit[0] == 0xC2;
  snprintf(buf, sizeof buf, attach ? "NOW %s%s" : "NOW %s %s", num, t->unit);
  lv_label_set_text(t->now, buf);
  lv_obj_set_style_text_color(t->now, st == RU_VAL_OK ? th->text_dim : th->warn, 0);
  lv_obj_set_style_bg_color(t->bead, st == RU_VAL_OK ? th->info : th->stale, 0);
  thermo_place_bead(t, cur->v);
  lv_obj_remove_flag(t->bead, LV_OBJ_FLAG_HIDDEN);
}

void ru_thermostat_set_unit(lv_obj_t *box, const char *unit) {
  thermo_t *t = wd_get(box);
  snprintf(t->unit, sizeof t->unit, "%s", unit ? unit : "");
  ru_thermostat_set_setpoint(box, ru_thermostat_get_setpoint(box));
}

void ru_thermostat_set_readonly(lv_obj_t *box, bool ro) {
  thermo_t *t = wd_get(box);
  for (int i = 0; i < 2; i++) {
    if (!t->btn[i]) continue;
    if (ro) lv_obj_add_flag(t->btn[i], LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
    else lv_obj_remove_flag(t->btn[i], LV_OBJ_FLAG_HIDDEN);
  }
  if (ro) lv_obj_remove_flag(t->ring.arc, LV_OBJ_FLAG_CLICKABLE);
  else lv_obj_add_flag(t->ring.arc, LV_OBJ_FLAG_CLICKABLE);
}

void ru_thermostat_set_mode(lv_obj_t *box, const char *text, lv_color_t color) {
  thermo_t *t = wd_get(box);
  lv_label_set_text(t->mode, text);
  lv_obj_set_style_text_color(t->mode, color, 0);
}

void ru_thermostat_on_change(lv_obj_t *box, ru_float_cb_t cb, void *ud) {
  thermo_t *t = wd_get(box);
  t->cb = cb;
  t->ud = ud;
}

/* --------------------------------------------------------------- state dot and tile */

/* children: 0 ring, 1 fill disc, 2 left half arc, 3 core, 4 dash */
static lv_obj_t *sdot_child(lv_obj_t *dot, int i) { return lv_obj_get_child(dot, i); }

lv_obj_t *ru_state_dot_create(lv_obj_t *parent, int d) {
  lv_obj_t *box = ru_cont_new(parent, 0, 0, d, d);
  int bw = d / 8 > 2 ? d / 8 : 2;
  lv_obj_t *ring = circle(box, d, 0, 0);
  lv_obj_set_style_border_width(ring, bw, 0);
  lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
  lv_obj_t *fill = circle(box, d - bw, 0, 0);
  lv_obj_align(fill, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
  lv_obj_t *half = lv_arc_create(box);
  lv_obj_remove_style_all(half);
  lv_obj_set_size(half, d - bw, d - bw);
  lv_obj_align(half, LV_ALIGN_CENTER, 0, 0);
  lv_arc_set_rotation(half, 0);
  lv_arc_set_bg_angles(half, 90, 270);
  lv_arc_set_range(half, 0, 100);
  lv_arc_set_value(half, 100);
  lv_obj_set_style_arc_width(half, (d - bw) / 2, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(half, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(half, (d - bw) / 2, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(half, false, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(half, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_remove_flag(half, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *core = circle(box, d / 3 > 2 ? d / 3 : 2, 0, 0);
  lv_obj_align(core, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(core, LV_OPA_COVER, 0);
  lv_obj_t *dash = lv_obj_create(box);
  lv_obj_remove_style_all(dash);
  lv_obj_set_size(dash, d / 3 > 3 ? d / 3 : 3, bw);
  lv_obj_align(dash, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(dash, LV_OPA_COVER, 0);
  ru_state_dot_set(box, RU_DOT_UNKNOWN, ru_theme()->accent);
  return box;
}

void ru_state_dot_set(lv_obj_t *dot, ru_dot_state_t s, lv_color_t on) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *ring = sdot_child(dot, 0), *fill = sdot_child(dot, 1), *half = sdot_child(dot, 2), *core = sdot_child(dot, 3),
           *dash = sdot_child(dot, 4);
  lv_color_t rc = s == RU_DOT_OFF ? t->text_dim : s == RU_DOT_UNKNOWN ? ru_dim(t->stale, 150) : on;
  lv_obj_set_style_border_color(ring, rc, 0);
  lv_obj_set_style_bg_color(fill, on, 0);
  lv_obj_set_style_arc_color(half, on, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(core, on, 0);
  lv_obj_set_style_bg_color(dash, rc, 0);
  if (s == RU_DOT_ON) lv_obj_remove_flag(fill, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
  if (s == RU_DOT_MIXED) lv_obj_remove_flag(half, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(half, LV_OBJ_FLAG_HIDDEN);
  if (s == RU_DOT_PUSH) lv_obj_remove_flag(core, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(core, LV_OBJ_FLAG_HIDDEN);
  if (s == RU_DOT_UNKNOWN) lv_obj_remove_flag(dash, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(dash, LV_OBJ_FLAG_HIDDEN);
  if (s == RU_DOT_NONE) { lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN); } else lv_obj_remove_flag(ring, LV_OBJ_FLAG_HIDDEN);
}

typedef struct {
  ru_tap_cb_t cb;
  void *ud;
  lv_obj_t *dot, *name, *sub, *val;
  ru_dot_state_t st;
  char subtxt[24];
  bool inited, dot_init;
  lv_color_t dot_on;
  ru_dot_state_t in_st, dot_st;
  ru_val_state_t fresh;
  char valtxt[32];
  bool val_set; /* set_value ran once (the label has its font and position), also for an empty text */
} stile_t;

static void stile_clicked(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target(e);
  stile_t *s = wd_get(o);
  if (s->cb) s->cb(o, s->ud);
}

lv_obj_t *ru_state_tile_create(lv_obj_t *parent, int w, int h, const char *name) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, h * 3 / 10, 0);
  lv_obj_set_style_bg_color(o, t->surface, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(0x23272E), LV_STATE_PRESSED);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
  stile_t *s = wd_attach(o, sizeof(stile_t));
  s->dot = ru_state_dot_create(o, ru_u(5.5f));
  lv_obj_align(s->dot, LV_ALIGN_CENTER, 0, -h * 26 / 100);
  lv_obj_remove_flag(s->dot, LV_OBJ_FLAG_CLICKABLE);
  s->name = ru_label_new(o, name, ru_font_body(), t->text, w / 2, h * 60 / 100);
  lv_label_set_long_mode(s->name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s->name, w - ru_u(2));
  lv_obj_set_height(s->name, lv_font_get_line_height(ru_font_body())); /* one line: cut with dots, never a second line over the sub text */
  lv_obj_set_style_text_align(s->name, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s->name, LV_ALIGN_CENTER, 0, h * 60 / 100 - h / 2);
  s->sub = ru_label_new(o, "--", ru_font_small(), t->text_dim, w / 2, h * 83 / 100);
  s->val = ru_label_new(o, "", ru_font_title(), t->text, w / 2, h * 28 / 100);
  lv_label_set_long_mode(s->val, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s->val, w - ru_u(2));
  lv_obj_set_style_text_align(s->val, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_flag(s->val, LV_OBJ_FLAG_HIDDEN);
  s->st = RU_DOT_UNKNOWN;
  snprintf(s->subtxt, sizeof s->subtxt, "--");
  lv_obj_add_event_cb(o, stile_clicked, LV_EVENT_CLICKED, NULL);
  return o;
}

void ru_state_tile_set(lv_obj_t *o, ru_dot_state_t st, const char *sub, ru_val_state_t fresh) {
  stile_t *s = wd_get(o);
  const ru_theme_t *t = ru_theme();
  char subc[sizeof s->subtxt];
  snprintf(subc, sizeof subc, "%s", sub);
  if (s->inited && s->in_st == st && s->fresh == fresh && !strcmp(subc, s->subtxt)) return; /* nothing changed: no redraw */
  s->inited = true;
  s->in_st = st;
  s->fresh = fresh;
  s->st = st;
  bool sub_changed = strcmp(subc, s->subtxt) != 0;
  memcpy(s->subtxt, subc, sizeof subc);
  if (sub_changed || !s->dot_init) {
    set_label(s->sub, s->subtxt);
    lv_obj_align(s->sub, LV_ALIGN_CENTER, 0, lv_obj_get_height(o) * 83 / 100 - lv_obj_get_height(o) / 2);
  }
  lv_color_t on = fresh == RU_VAL_STALE ? ru_dim(t->stale, 170) : t->accent;
  if (st != RU_DOT_NONE && (st == RU_DOT_UNKNOWN || fresh == RU_VAL_UNKNOWN)) { st = RU_DOT_UNKNOWN; s->st = st; }
  if (!s->dot_init || s->dot_st != st || !lv_color_eq(s->dot_on, on)) {
    s->dot_init = true;
    s->dot_st = st;
    s->dot_on = on;
    ru_state_dot_set(s->dot, st, on);
  }
  bool vis = !lv_obj_has_flag(s->val, LV_OBJ_FLAG_HIDDEN);
  if (st == RU_DOT_NONE && !vis) lv_obj_remove_flag(s->val, LV_OBJ_FLAG_HIDDEN);
  if (st != RU_DOT_NONE && vis) lv_obj_add_flag(s->val, LV_OBJ_FLAG_HIDDEN);
  set_text_color(s->val, fresh == RU_VAL_STALE ? t->stale : fresh == RU_VAL_UNKNOWN ? t->text_dim : t->text);
  set_text_color(s->name, fresh == RU_VAL_STALE ? t->text_dim : t->text);
  set_text_color(s->sub, st == RU_DOT_MIXED ? t->warn : (fresh == RU_VAL_STALE ? t->stale : t->text_dim));
}

void ru_state_tile_set_value(lv_obj_t *o, const char *text) {
  stile_t *s = wd_get(o);
  char vc[sizeof s->valtxt];
  snprintf(vc, sizeof vc, "%s", text);
  if (s->val_set && !strcmp(vc, s->valtxt)) return;
  s->val_set = true;
  memcpy(s->valtxt, vc, sizeof vc);
  lv_label_set_text(s->val, text);
  /* long values (text sensors) drop to the body font so they fit the tile */
  const lv_font_t *f = strlen(text) > 5 ? ru_font_body() : ru_font_title();
  int room = lv_obj_get_width(o) - ru_u(2); /* same as the label width set at creation */
  lv_point_t sz;
  lv_text_get_size(&sz, text, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  if (sz.x > room) f = ru_font_small(); /* two readings ("19.8°C  45%") on one line */
  if (lv_obj_get_style_text_font(s->val, LV_PART_MAIN) != f) lv_obj_set_style_text_font(s->val, f, 0);
  if (lv_obj_get_height(s->val) != lv_font_get_line_height(f)) /* one line: LONG_DOT cuts with dots instead of wrapping over the name */
    lv_obj_set_height(s->val, lv_font_get_line_height(f));
  lv_obj_align(s->val, LV_ALIGN_CENTER, 0, lv_obj_get_height(o) * 28 / 100 - lv_obj_get_height(o) / 2);
}

ru_dot_state_t ru_state_tile_dot(const lv_obj_t *o) { return ((stile_t *)wd_get(o))->st; }
const char *ru_state_tile_sub(const lv_obj_t *o) { return ((stile_t *)wd_get(o))->subtxt; }
void ru_state_tile_on_tap(lv_obj_t *o, ru_tap_cb_t cb, void *ud) {
  stile_t *s = wd_get(o);
  s->cb = cb;
  s->ud = ud;
}

/* ---------------------------------------------------------------- weather tile */

typedef struct {
  ru_tap_cb_t cb;
  void *ud;
  lv_obj_t *part[6]; /* name, icon, value, unit, second, dot */
  bool inited;
  char name[40], icon[8], value[16], unit[24], second[32];
  lv_color_t icon_color, second_color;
  ru_val_state_t state;
  const lv_font_t *vfont;
  int w, h;
  int pos[5][4]; /* x, y, w, h of the five labels as last placed */
  bool placed;
} wtile_t;

static lv_style_t st_wx_tile, st_wx_pressed, st_wx_stale, st_wx_name, st_wx_icon, st_wx_unit, st_wx_second;
static bool st_wx_ready;

static void wx_styles(void) {
  if (st_wx_ready) return;
  st_wx_ready = true;
  const ru_theme_t *t = ru_theme();
  lv_style_init(&st_wx_tile);
  lv_style_set_bg_color(&st_wx_tile, t->surface);
  lv_style_set_bg_opa(&st_wx_tile, LV_OPA_COVER);
  lv_style_init(&st_wx_pressed);
  lv_style_set_bg_color(&st_wx_pressed, lv_color_hex(0x23272E));
  lv_style_init(&st_wx_stale);
  lv_style_set_border_width(&st_wx_stale, 1);
  lv_style_set_border_color(&st_wx_stale, t->stale);
  lv_style_set_border_opa(&st_wx_stale, LV_OPA_COVER);
  lv_style_init(&st_wx_name);
  lv_style_set_text_font(&st_wx_name, ru_font_caption());
  lv_style_set_text_color(&st_wx_name, t->text_dim);
  lv_style_set_text_align(&st_wx_name, LV_TEXT_ALIGN_CENTER);
  lv_style_init(&st_wx_icon);
  lv_style_set_text_font(&st_wx_icon, ru_font_icon_s());
  lv_style_init(&st_wx_unit);
  lv_style_set_text_font(&st_wx_unit, ru_font_body());
  lv_style_set_text_color(&st_wx_unit, t->text_dim);
  lv_style_init(&st_wx_second);
  lv_style_set_text_font(&st_wx_second, ru_font_caption());
  lv_style_set_text_align(&st_wx_second, LV_TEXT_ALIGN_CENTER);
}

static lv_obj_t *wx_label(lv_obj_t *parent, lv_style_t *st, lv_label_long_mode_t mode) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_style(l, st, 0);
  lv_label_set_long_mode(l, mode);
  lv_label_set_text_static(l, "");
  return l;
}

static int text_w(const char *s, const lv_font_t *f) {
  if (!s[0]) return 0;
  lv_point_t sz;
  lv_text_get_size(&sz, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return sz.x + 1;
}

/* Cut s (UTF-8) with ".." until it is at most maxw wide. */
static void fit_text(char *s, const lv_font_t *f, int maxw) {
  while (s[0] && text_w(s, f) > maxw) {
    size_t l = strlen(s);
    size_t cut = l > 3 ? l - 3 : 0;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    if (l <= 2) { s[0] = 0; break; }
    s[cut] = 0;
    strcat(s, "..");
    if (cut == 0) break;
  }
}

/* Place label i at (x, y) with size (w, h) unless it is already there: a style set invalidates even for the same value. */
static void wx_place(wtile_t *s, int i, int x, int y, int w, int h) {
  int *p = s->pos[i];
  lv_obj_t *l = s->part[i];
  if (s->placed && p[0] == x && p[1] == y && p[2] == w && p[3] == h) return;
  p[0] = x; p[1] = y; p[2] = w; p[3] = h;
  lv_obj_set_pos(l, x, y);
  lv_obj_set_size(l, w, h);
}

/* Geometry of the value row from the strings (a pure function of them): the labels are placed, the value text that fits is returned. The
 * labels clip and never rewrite their text, so the order of text and size changes does not matter. */
static void wx_layout(lv_obj_t *o, wtile_t *s, char *vshow, size_t vn, char *sshow, size_t sn) {
  (void)o;
  int w = s->w, h = s->h;
  int pad = ru_u(1.2f), gap = ru_u(0.8f), room = w - 2 * pad;
  const lv_font_t *cap = ru_font_caption(), *body = ru_font_body(), *icf = ru_font_icon_s();
  int cl = lv_font_get_line_height(cap);
  int name_y = h * 8 / 100, sec_y = h - h * 8 / 100 - cl;
  int rc = (name_y + cl + sec_y) / 2; /* centre line of the value row */
  /* the value row: icon, value, unit as one centred group; the value font drops until it fits, last the icon goes */
  const lv_font_t *fonts[3] = { ru_font_big(), ru_font_title(), body };
  int uw = text_w(s->unit, body), icw = s->icon[0] ? lv_font_get_line_height(icf) : 0;
  const lv_font_t *vf = body;
  int vw = 0;
  bool icon = icw > 0;
  snprintf(vshow, vn, "%s", s->value);
  for (int k = 0; k < 4; k++) {
    vf = fonts[k < 3 ? k : 2];
    if (k == 3) icon = false; /* even the small font does not fit beside the icon */
    vw = text_w(vshow, vf);
    if ((icon ? icw + gap : 0) + vw + uw <= room || k == 3) break;
  }
  int lead = icon ? icw + gap : 0;
  if (lead + vw + uw > room) { /* still too wide: the value is cut with dots, the unit keeps its room */
    fit_text(vshow, vf, room - uw - lead);
    vw = text_w(vshow, vf);
  }
  int total = lead + vw + uw;
  s->vfont = vf;
  if (lv_obj_get_style_text_font(s->part[2], LV_PART_MAIN) != vf) lv_obj_set_style_text_font(s->part[2], vf, 0);
  int vl = lv_font_get_line_height(vf);
  int x = (w - total) / 2;
  if (icon) { wx_place(s, 1, x, rc - icw / 2, icw, icw); x += icw + gap; lv_obj_remove_flag(s->part[1], LV_OBJ_FLAG_HIDDEN); }
  else { wx_place(s, 1, x, rc - icw / 2, 0, 0); lv_obj_add_flag(s->part[1], LV_OBJ_FLAG_HIDDEN); }
  int vy = rc - vl / 2;
  wx_place(s, 2, x, vy, vw, vl);
  /* the unit sits on the baseline of the value */
  int ul = lv_font_get_line_height(body);
  int uy = vy + (vl - vf->base_line) - (ul - body->base_line);
  wx_place(s, 3, x + vw, uy, uw, ul);
  int sroom = w - 2 * ru_u(2.5f); /* the stale dot owns the corner */
  snprintf(sshow, sn, "%s", s->second);
  fit_text(sshow, cap, sroom);
  int sw = text_w(sshow, cap);
  wx_place(s, 4, (w - sw) / 2, sec_y, sw, cl);
  s->placed = true;
}

static void wtile_clicked(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target(e);
  wtile_t *s = wd_get(o);
  if (s->cb) s->cb(o, s->ud);
}

lv_obj_t *ru_wx_tile_create(lv_obj_t *parent, int w, int h) {
  const ru_theme_t *t = ru_theme();
  wx_styles();
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_add_style(o, &st_wx_tile, 0);
  lv_obj_add_style(o, &st_wx_pressed, LV_STATE_PRESSED);
  lv_obj_add_style(o, &st_wx_stale, LV_STATE_USER_1);
  lv_obj_set_style_radius(o, h * 3 / 10, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
  wtile_t *s = wd_attach(o, sizeof(wtile_t));
  s->w = w;
  s->h = h;
  /* the name label has its final size from the start: LONG_DOT rewrites the text for the width it has when the text is set */
  int cl = lv_font_get_line_height(ru_font_caption());
  s->part[0] = wx_label(o, &st_wx_name, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(s->part[0], ru_u(1.5f), h * 8 / 100);
  lv_obj_set_size(s->part[0], w - 2 * ru_u(1.5f), cl);
  s->part[1] = wx_label(o, &st_wx_icon, LV_LABEL_LONG_CLIP);
  s->part[2] = wx_label(o, &st_wx_unit, LV_LABEL_LONG_CLIP); /* the value label: unit style gives font and colour, both are set per tile state below */
  lv_obj_set_style_text_color(s->part[2], t->text, 0);
  s->part[3] = wx_label(o, &st_wx_unit, LV_LABEL_LONG_CLIP);
  s->part[4] = wx_label(o, &st_wx_second, LV_LABEL_LONG_CLIP);
  int dd = ru_u(1.1f); /* 6 px at the usual size */
  lv_obj_t *dot = lv_obj_create(o);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, dd, dd);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, t->warn, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(dot, w - ru_u(2.2f) - dd, h - ru_u(2.2f) - dd);
  lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
  s->part[5] = dot;
  s->icon_color = t->text;
  s->second_color = t->text_dim;
  lv_obj_add_event_cb(o, wtile_clicked, LV_EVENT_CLICKED, NULL);
  return o;
}

void ru_wx_tile_set(lv_obj_t *o, const ru_wxtile_data_t *d) {
  wtile_t *s = wd_get(o);
  const ru_theme_t *t = ru_theme();
  char name[sizeof s->name], icon[sizeof s->icon], value[sizeof s->value], unit[sizeof s->unit], second[sizeof s->second];
  snprintf(name, sizeof name, "%s", d->name ? d->name : "");
  snprintf(icon, sizeof icon, "%s", d->icon ? d->icon : "");
  snprintf(value, sizeof value, "%s", d->value ? d->value : "");
  snprintf(unit, sizeof unit, "%s", d->unit ? d->unit : "");
  snprintf(second, sizeof second, "%s", d->second ? d->second : "");
  bool first = !s->inited;
  s->inited = true;
  bool relayout = first;
  if (first || strcmp(name, s->name)) { memcpy(s->name, name, sizeof name); set_label(s->part[0], name); }
  if (first || strcmp(icon, s->icon)) { memcpy(s->icon, icon, sizeof icon); relayout = true; }
  if (first || strcmp(value, s->value)) { memcpy(s->value, value, sizeof value); relayout = true; }
  if (first || strcmp(unit, s->unit)) { memcpy(s->unit, unit, sizeof unit); relayout = true; }
  if (first || strcmp(second, s->second)) { memcpy(s->second, second, sizeof second); relayout = true; }
  if (relayout) {
    char vshow[sizeof s->value], sshow[sizeof s->second];
    wx_layout(o, s, vshow, sizeof vshow, sshow, sizeof sshow);
    set_label(s->part[1], s->icon);
    set_label(s->part[2], vshow);
    set_label(s->part[3], s->unit);
    set_label(s->part[4], sshow);
  }
  bool grey = d->state != RU_VAL_OK;
  s->state = d->state;
  set_text_color(s->part[1], grey ? t->stale : d->icon_color);
  set_text_color(s->part[2], d->state == RU_VAL_STALE ? t->stale : d->state == RU_VAL_UNKNOWN ? t->text_dim : t->text);
  set_text_color(s->part[3], t->text_dim);
  set_text_color(s->part[4], grey ? t->stale : d->second_color);
  bool dot = lv_obj_has_flag(s->part[5], LV_OBJ_FLAG_HIDDEN);
  if (grey && dot) lv_obj_remove_flag(s->part[5], LV_OBJ_FLAG_HIDDEN);
  if (!grey && !dot) lv_obj_add_flag(s->part[5], LV_OBJ_FLAG_HIDDEN);
  bool bord = lv_obj_has_state(o, LV_STATE_USER_1);
  if (grey && !bord) lv_obj_add_state(o, LV_STATE_USER_1);
  if (!grey && bord) lv_obj_remove_state(o, LV_STATE_USER_1);
}

void ru_wx_tile_on_tap(lv_obj_t *o, ru_tap_cb_t cb, void *ud) {
  wtile_t *s = wd_get(o);
  s->cb = cb;
  s->ud = ud;
}

lv_obj_t *ru_wx_tile_part(const lv_obj_t *o, int which) {
  wtile_t *s = wd_get(o);
  return which >= 0 && which < 6 ? s->part[which] : NULL;
}

/* ---------------------------------------------------------------- light */

#define LT_RANGE 360     /* the colour arc runs 0..360: hue in degrees, or the cool fraction of the temperature scale in 1/3.6 % */
#define LT_MAXPART 9
#define LT_ANGLE_TOL 8.0f
#define LT_TAP_PX 8

typedef enum { ZONE_NONE, ZONE_BRI, ZONE_COL, ZONE_DISC } lt_zone_t;
typedef enum { CHIP_BRI, CHIP_COLOUR, CHIP_WHITE } lt_chip_t;

typedef struct {
  ru_light_cfg_t cfg;
  ru_light_hooks_t hk;
  ring_t bri;
  lv_obj_t *root, *carc, *touch, *disc, *glyph, *hero, *cap, *pill, *chip[3];
  int nchip;
  lt_chip_t chip_kind[3];
  int d_ring, dc, wc, disc_d, wb;
  float band_b0, band_b1, band_c0, band_c1, disc_r; /* touch bands (px from the centre) */
  int max;
  /* model and view state */
  ru_light_state_t st;
  bool have_st;
  int vkind;            /* RU_RING_HUE / RU_RING_TEMP: what the colour arc shows */
  bool show_col;        /* round panel: the colour arc is the ring shown (else brightness) */
  bool user_view;       /* the user picked a chip: kept until the lamp's mode changes */
  int img_kind;         /* what the arc image holds (-1: none yet) */
  ru_light_fb_t fb;
  char fb_text[24];
  bool ro;
  char ro_text[24];
  /* interaction */
  lt_zone_t zone;
  bool dragging, moved_far;
  int px0, py0;
  int drag_v;           /* the value under the finger (bri device units, hue 0..359, temp arc 0..360) */
  bool pend;            /* released, waiting for the model: the knob stays where it was left */
  lt_zone_t pend_zone;
  int pend_v, pend_vkind;
  ru_light_cb_t cb;
  void *ud;
} light_t;

static light_t *lt_get(const lv_obj_t *o) { return wd_get(o); }

static void sty_bg(lv_obj_t *o, lv_color_t c, lv_opa_t opa, lv_part_t p) {
  if (!lv_color_eq(lv_obj_get_style_bg_color(o, p), c)) lv_obj_set_style_bg_color(o, c, p);
  if (lv_obj_get_style_bg_opa(o, p) != opa) lv_obj_set_style_bg_opa(o, opa, p);
}
static void sty_border(lv_obj_t *o, int w, lv_color_t c, lv_opa_t opa, lv_part_t p) {
  if (lv_obj_get_style_border_width(o, p) != w) lv_obj_set_style_border_width(o, w, p);
  if (!lv_color_eq(lv_obj_get_style_border_color(o, p), c)) lv_obj_set_style_border_color(o, c, p);
  if (lv_obj_get_style_border_opa(o, p) != opa) lv_obj_set_style_border_opa(o, opa, p);
}
static void set_hidden(lv_obj_t *o, bool hide) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != hide) {
    if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}
static void set_clickable(lv_obj_t *o, bool on) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) != on) {
    if (on) lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  }
}
static lv_color_t rgb_of(uint32_t c) { return lv_color_hex(c); }

static int lt_pct(const light_t *l, int v) { return l->max == 100 ? v : (int)lroundf(100.0f * (float)v / (float)l->max); }

/* angle 0 = 3 o'clock clockwise -> position along the sweep. false when outside the sweep +- tolerance. *p is clamped to 0..1 */
static bool lt_sweep_p(float deg, float tol, float *p) {
  float rel = deg - 135.0f;
  while (rel < 0.0f) rel += 360.0f;
  while (rel >= 360.0f) rel -= 360.0f;
  if (rel > 270.0f + tol && rel < 360.0f - tol) return false;
  if (rel <= 270.0f) *p = rel / 270.0f;
  else *p = rel < 315.0f ? 1.0f : 0.0f;
  return true;
}

static bool lt_centre(const light_t *l, float *cx, float *cy) {
  lv_area_t a;
  lv_obj_get_coords(l->touch, &a);
  *cx = (float)(a.x1 + a.x2 + 1) / 2.0f;
  *cy = (float)(a.y1 + a.y2 + 1) / 2.0f;
  return true;
}

static lt_zone_t lt_zone_of(const light_t *l, int x, int y, float *p) {
  float cx, cy;
  lt_centre(l, &cx, &cy);
  float dx = (float)x - cx, dy = (float)y - cy;
  float r = sqrtf(dx * dx + dy * dy);
  float deg = atan2f(dy, dx) * 180.0f / PI_F;
  if (deg < 0) deg += 360.0f;
  float pp = 0;
  if (r < l->disc_r) return ZONE_DISC;
  if (l->cfg.two_rings) {
    if (r >= l->band_b0 && r <= l->band_b1 && lt_sweep_p(deg, LT_ANGLE_TOL, &pp)) { if (p) *p = pp; return ZONE_BRI; }
    if (l->carc && r >= l->band_c0 && r <= l->band_c1 && lt_sweep_p(deg, LT_ANGLE_TOL, &pp)) { if (p) *p = pp; return ZONE_COL; }
    return ZONE_NONE;
  }
  if (r >= l->band_b0 && r <= l->band_b1 && lt_sweep_p(deg, LT_ANGLE_TOL, &pp)) {
    if (p) *p = pp;
    return l->show_col && l->carc ? ZONE_COL : ZONE_BRI;
  }
  return ZONE_NONE;
}

static void lt_ensure_img(light_t *l) {
  if (!l->carc || l->img_kind == l->vkind) return;
  ru_ring_color_fn fn = l->vkind == RU_RING_HUE ? l->hk.hue_fn : l->hk.temp_fn;
  const lv_image_dsc_t *im = ru_ring_img_get(l->dc, l->wc, (ru_ring_kind_t)l->vkind, fn);
  if (!im) return;
  lv_obj_set_style_arc_image_src(l->carc, im, LV_PART_MAIN);
  lv_obj_invalidate(l->carc);
  l->img_kind = l->vkind;
}

static void icon_recolor(lv_obj_t *icon, lv_color_t c) {
  lv_obj_t *arc = lv_obj_get_child(icon, 0), *bar = lv_obj_get_child(icon, 1);
  if (!lv_color_eq(lv_obj_get_style_arc_color(arc, LV_PART_MAIN), c)) lv_obj_set_style_arc_color(arc, c, LV_PART_MAIN);
  if (!lv_color_eq(lv_obj_get_style_bg_color(bar, LV_PART_MAIN), c)) lv_obj_set_style_bg_color(bar, c, LV_PART_MAIN);
}

/* the arc value the colour arc shows for the model; -1 = no knob (the lamp is not in this mode, or has no hue) */
static int lt_model_col_v(const light_t *l) {
  const ru_light_state_t *s = &l->st;
  if (!l->have_st || !s->known) return -1;
  if (l->vkind == RU_RING_HUE) return s->mode == RU_LIGHT_COLOR && s->has_hue ? s->hue % 360 : -1;
  if (s->mode != RU_LIGHT_TEMP) return -1;
  int w = s->warmth < 0 ? 0 : s->warmth > 100 ? 100 : s->warmth;
  return (int)lroundf((float)(100 - w) * 3.6f);
}

static const char *lt_word(const light_t *l, bool col_view, int col_v) {
  const ru_light_state_t *s = &l->st;
  if (!col_view) return "BRIGHTNESS";
  if (l->vkind == RU_RING_HUE) {
    if (col_v >= 0 && l->hk.hue_word) return l->hk.hue_word((float)col_v);
    return s->mode == RU_LIGHT_WHITE ? "WHITE" : "COLOUR";
  }
  if (col_v >= 0 && l->hk.warm_word) return l->hk.warm_word((int)lroundf(100.0f - (float)col_v / 3.6f));
  return "WHITE";
}

static void lt_paint(light_t *l) {
  const ru_theme_t *t = ru_theme();
  const ru_light_state_t *s = &l->st;
  bool known = l->have_st && s->known;
  bool on = known && s->on;
  bool drag_bri = l->dragging && l->zone == ZONE_BRI;
  bool drag_col = l->dragging && l->zone == ZONE_COL;
  bool pend_bri = !l->dragging && l->pend && l->pend_zone == ZONE_BRI;
  bool pend_col = !l->dragging && l->pend && l->pend_zone == ZONE_COL;
  bool lit = on || drag_bri || drag_col;
  lv_color_t lamp = s->color ? rgb_of(s->color) : t->accent;
  bool col_vis = l->carc && (l->cfg.two_rings || l->show_col);
  bool bri_vis = l->cfg.two_rings || !l->show_col || !l->carc;

  /* brightness ring: the model, or the finger; the tint of the ring and glow follows the MODEL (it changes on release) */
  int bv = drag_bri ? l->drag_v : pend_bri ? l->pend_v : known ? s->level : 0;
  set_hidden(l->bri.box, !bri_vis);
  ring_set(&l->bri, bv);
  ring_set_lit(&l->bri, known && (on || drag_bri || pend_bri), lamp, true);
  if (known && !lit && !pend_bri) { /* off: no fill, a dim ghost knob at the stored level */
    sty_bg(l->bri.arc, ru_dim(t->text_dim, 150), LV_OPA_COVER, LV_PART_KNOB);
  } else if (known) {
    sty_bg(l->bri.arc, t->text, LV_OPA_COVER, LV_PART_KNOB);
  } else {
    sty_bg(l->bri.arc, t->text, LV_OPA_TRANSP, LV_PART_KNOB);
  }

  /* colour / temperature ring */
  int col_v = -1;
  if (l->carc) {
    lt_ensure_img(l);
    int mv = lt_model_col_v(l);
    col_v = drag_col ? l->drag_v : (pend_col && l->pend_vkind == l->vkind) ? l->pend_v : mv;
    set_hidden(l->carc, !col_vis);
    lv_opa_t aop = on || drag_col ? LV_OPA_COVER : LV_OPA_40;
    if (lv_obj_get_style_arc_opa(l->carc, LV_PART_MAIN) != aop) lv_obj_set_style_arc_opa(l->carc, aop, LV_PART_MAIN);
    if (col_v >= 0) {
      if (lv_arc_get_value(l->carc) != col_v) lv_arc_set_value(l->carc, col_v);
      ru_ring_color_fn fn = l->vkind == RU_RING_HUE ? l->hk.hue_fn : l->hk.temp_fn;
      lv_color_t kc = rgb_of(fn((float)col_v / (float)LT_RANGE));
      sty_bg(l->carc, kc, aop, LV_PART_KNOB);
      sty_border(l->carc, l->ro || !known ? 0 : ru_u(0.6f) < 2 ? 2 : ru_u(0.6f), t->text, aop, LV_PART_KNOB);
    } else {
      sty_bg(l->carc, t->text, LV_OPA_TRANSP, LV_PART_KNOB);
      sty_border(l->carc, 0, t->text, LV_OPA_TRANSP, LV_PART_KNOB);
    }
  }

  /* centre disc */
  lv_color_t dtint = lamp;
  if (drag_col && l->carc) {
    ru_ring_color_fn fn = l->vkind == RU_RING_HUE ? l->hk.hue_fn : l->hk.temp_fn;
    dtint = rgb_of(fn((float)l->drag_v / (float)LT_RANGE));
  } else if (!drag_col && pend_col && l->carc && col_v >= 0) {
    ru_ring_color_fn fn = l->vkind == RU_RING_HUE ? l->hk.hue_fn : l->hk.temp_fn;
    dtint = rgb_of(fn((float)col_v / (float)LT_RANGE));
  }
  if (lit || pend_bri || pend_col) {
    sty_bg(l->disc, ru_dim(dtint, 60), LV_OPA_COVER, LV_PART_MAIN);
    sty_border(l->disc, 2, ru_dim(dtint, 140), LV_OPA_COVER, LV_PART_MAIN);
  } else {
    sty_bg(l->disc, t->surface, LV_OPA_COVER, LV_PART_MAIN);
    sty_border(l->disc, 2, t->track, LV_OPA_COVER, LV_PART_MAIN);
  }
  char buf[24];
  if (!known) snprintf(buf, sizeof buf, "--");
  else if (!lit && !pend_bri && !pend_col) snprintf(buf, sizeof buf, "OFF");
  else snprintf(buf, sizeof buf, "%d%%", lt_pct(l, bv));
  set_label(l->hero, buf);
  set_text_color(l->hero, lit || pend_bri || pend_col ? t->text : t->text_dim);
  icon_recolor(l->glyph, lit || pend_bri || pend_col ? t->text : ru_dim(t->text_dim, 150));

  /* caption: the read-only reason, then the feedback, then the word for what the visible ring edits */
  const char *cap;
  lv_color_t capc = t->text_dim;
  if (l->ro && l->ro_text[0]) {
    cap = l->ro_text;
    capc = t->warn;
  } else if (l->fb != RU_LIGHT_FB_NONE && l->fb_text[0]) {
    cap = l->fb_text;
    capc = l->fb == RU_LIGHT_FB_OK ? t->ok : l->fb == RU_LIGHT_FB_WARN ? t->warn : l->fb == RU_LIGHT_FB_ERROR ? t->error : t->text_dim;
  } else if (!known) {
    cap = "";
  } else if (l->cfg.two_rings && l->carc) {
    /* two rings: the caption names the colour (or the white); brightness has the % */
    cap = lt_word(l, true, col_v);
  } else {
    bool cview = l->show_col && l->carc;
    cap = lt_word(l, cview, col_v);
  }
  set_label(l->cap, cap);
  set_text_color(l->cap, capc);

  /* chips */
  if (l->pill) {
    bool show = !l->ro && known;
    set_hidden(l->pill, !show);
    for (int i = 0; i < l->nchip; i++) {
      bool sel;
      switch (l->chip_kind[i]) {
        case CHIP_BRI: sel = !l->cfg.two_rings && !l->show_col; break;
        case CHIP_COLOUR: sel = l->carc && (l->cfg.two_rings || l->show_col) && l->vkind == RU_RING_HUE; break;
        default: sel = (l->carc && (l->cfg.two_rings || l->show_col) && l->vkind == RU_RING_TEMP) ||
                       (!(l->cfg.caps & RU_LIGHT_CAP_CT) && s->mode == RU_LIGHT_WHITE); break;
      }
      sty_bg(l->chip[i], t->text, sel ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
      set_text_color(l->chip[i], sel ? t->void_bg : t->text_dim);
    }
  }
  set_clickable(l->touch, !l->ro && known);
}

/* ---- interaction ---- */

static int lt_cmd_value(const light_t *l, lt_zone_t z, int v) {
  if (z == ZONE_BRI) return v;
  if (l->vkind == RU_RING_HUE) return v % 360;
  return (int)lroundf(100.0f - (float)v / 3.6f); /* temperature: warmth 0..100 */
}

static void lt_point(int *x, int *y) {
  lv_point_t p = { 0, 0 };
  lv_indev_t *in = lv_indev_active();
  if (in) lv_indev_get_point(in, &p);
  *x = p.x;
  *y = p.y;
}

static bool lt_drag_update(light_t *l, int x, int y) {
  float cx, cy;
  lt_centre(l, &cx, &cy);
  float deg = atan2f((float)y - cy, (float)x - cx) * 180.0f / PI_F, p = 0;
  if (deg < 0) deg += 360.0f;
  if (!lt_sweep_p(deg, 90.0f, &p)) return false;
  int v;
  if (l->zone == ZONE_BRI) v = (int)lroundf(p * (float)l->max);
  else if (l->vkind == RU_RING_HUE) { v = (int)lroundf(p * (float)LT_RANGE); if (v > 359) v = 359; }
  else v = (int)lroundf(p * (float)LT_RANGE);
  if (l->dragging && v == l->drag_v) return false;
  l->drag_v = v;
  l->dragging = true;
  lt_paint(l);
  return true;
}

static void lt_emit(light_t *l, ru_light_cmd_t c, int v, bool fin) {
  if (l->cb) l->cb(l->root, c, v, fin, l->ud);
}

static void lt_touch_event(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target(e);
  light_t *l = lt_get(lv_obj_get_parent(o));
  if (!l) return;
  lv_event_code_t code = lv_event_get_code(e);
  int x, y;
  lt_point(&x, &y);
  if (code == LV_EVENT_PRESSED) {
    l->zone = lt_zone_of(l, x, y, NULL);
    l->px0 = x;
    l->py0 = y;
    l->moved_far = false;
    l->dragging = false;
    if (l->zone == ZONE_BRI || l->zone == ZONE_COL) {
      l->pend = false;
      lt_drag_update(l, x, y);
      if (l->dragging) lt_emit(l, l->zone == ZONE_BRI ? RU_LIGHT_CMD_LEVEL : l->vkind == RU_RING_HUE ? RU_LIGHT_CMD_HUE : RU_LIGHT_CMD_TEMP,
                               lt_cmd_value(l, l->zone, l->drag_v), false);
    }
  } else if (code == LV_EVENT_PRESSING) {
    if (l->zone == ZONE_DISC) {
      int dx = x - l->px0, dy = y - l->py0;
      if (dx * dx + dy * dy >= LT_TAP_PX * LT_TAP_PX) l->moved_far = true;
    } else if (l->zone == ZONE_BRI || l->zone == ZONE_COL) {
      if (lt_drag_update(l, x, y))
        lt_emit(l, l->zone == ZONE_BRI ? RU_LIGHT_CMD_LEVEL : l->vkind == RU_RING_HUE ? RU_LIGHT_CMD_HUE : RU_LIGHT_CMD_TEMP,
                lt_cmd_value(l, l->zone, l->drag_v), false);
    }
  } else if (code == LV_EVENT_RELEASED) {
    lt_zone_t z = l->zone;
    l->zone = ZONE_NONE;
    if (z == ZONE_DISC) {
      int dx = x - l->px0, dy = y - l->py0;
      bool far = l->moved_far || dx * dx + dy * dy >= LT_TAP_PX * LT_TAP_PX;
      if (!far && l->have_st && l->st.known) lt_emit(l, RU_LIGHT_CMD_POWER, l->st.on ? 0 : 1, true);
    } else if ((z == ZONE_BRI || z == ZONE_COL) && l->dragging) {
      int v = l->drag_v;
      l->dragging = false;
      l->pend = true;
      l->pend_zone = z;
      l->pend_v = v;
      l->pend_vkind = l->vkind;
      lt_zone_t zz = l->zone;
      l->zone = z; /* lt_cmd_value reads the ring */
      int cv = lt_cmd_value(l, z, v);
      l->zone = zz;
      lt_paint(l);
      lt_emit(l, z == ZONE_BRI ? RU_LIGHT_CMD_LEVEL : l->vkind == RU_RING_HUE ? RU_LIGHT_CMD_HUE : RU_LIGHT_CMD_TEMP, cv, true);
    }
  } else if (code == LV_EVENT_PRESS_LOST) {
    l->zone = ZONE_NONE;
    if (l->dragging) { l->dragging = false; lt_paint(l); }
  }
}

static void lt_chip_apply(light_t *l, int i) {
  if (i < 0 || i >= l->nchip || l->ro) return;
  switch (l->chip_kind[i]) {
    case CHIP_BRI: l->show_col = false; l->user_view = true; break;
    case CHIP_COLOUR: if (!l->cfg.two_rings) l->show_col = true; l->vkind = RU_RING_HUE; l->user_view = true; break;
    default:
      if (l->cfg.caps & RU_LIGHT_CAP_CT) { if (!l->cfg.two_rings) l->show_col = true; l->vkind = RU_RING_TEMP; l->user_view = true; }
      else lt_emit(l, RU_LIGHT_CMD_WHITE, 0, true); /* single white channel: the chip is a plain command, no ring */
      break;
  }
  lt_paint(l);
}

static void lt_chip_click(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target(e);
  light_t *l = lt_get(lv_obj_get_parent(lv_obj_get_parent(o)));
  if (l) lt_chip_apply(l, (int)(intptr_t)lv_event_get_user_data(e));
}

static void lt_derive_view(light_t *l, int prev_mode, bool first) {
  if (!first && l->st.mode != prev_mode) l->user_view = false;
  if (l->user_view) return;
  int caps = l->cfg.caps;
  bool rgb = caps & RU_LIGHT_CAP_RGB, ct = caps & RU_LIGHT_CAP_CT;
  if (rgb && ct) l->vkind = (l->st.mode == RU_LIGHT_WHITE || l->st.mode == RU_LIGHT_TEMP) ? RU_RING_TEMP : RU_RING_HUE;
  else l->vkind = ct && !rgb ? RU_RING_TEMP : RU_RING_HUE;
}

lv_obj_t *ru_light_create(lv_obj_t *parent, int d, const ru_light_cfg_t *cfg) {
  const ru_theme_t *t = ru_theme();
  int caps = cfg->caps & 7;
  bool colour = (caps & (RU_LIGHT_CAP_RGB | RU_LIGHT_CAP_CT)) != 0;
  bool two = cfg->two_rings;
  int S = d + 2 * ring_glow_extent();
  lv_obj_t *root = ru_cont_new(parent, 0, 0, S, S);
  lv_obj_align(root, LV_ALIGN_CENTER, 0, 0);
  lv_obj_update_layout(root);
  light_t *l = wd_attach(root, sizeof(light_t));
  l->root = root;
  l->cfg = *cfg;
  l->cfg.caps = caps;
  if (cfg->hooks) l->hk = *cfg->hooks;
  l->cfg.hooks = NULL;
  l->max = cfg->max < 1 ? 1 : cfg->max > 100 ? 100 : cfg->max;
  l->d_ring = d;
  l->img_kind = -1;
  l->vkind = (caps & RU_LIGHT_CAP_RGB) ? RU_RING_HUE : RU_RING_TEMP;
  /* geometry (u = D/100): square: outer ring 86u (2.2u wide) + inner ring 62u (5u wide) + disc 38u; round: one ring (4.4u wide when
   * chips switch it) + disc 44u. A plain dimmer keeps today's thin ring and gets the disc. */
  l->wb = (!two && colour) ? ru_u(4.4f) : ru_u(2.2f);
  l->dc = two ? ru_u(62) : d;
  l->wc = two ? ru_u(5) : l->wb;
  l->disc_d = (two && colour) ? ru_u(38) : ru_u(44);
  l->band_b0 = (two && colour) ? ru_u(37.5f) : ru_u(31);
  l->band_b1 = ru_u(48.3f);
  l->band_c0 = ru_u(23.3f);
  l->band_c1 = ru_u(37.3f);
  l->disc_r = (two && colour) ? ru_u(23.3f) : ru_u(31);

  ring_init(&l->bri, root, d, l->wb, ROT, SWEEP, l->max, true, false);
  if (colour) {
    l->carc = mk_arc(root, l->dc, l->wc, ROT, SWEEP, LT_RANGE);
    lv_obj_set_style_arc_color(l->carc, t->track, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(l->carc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    int pad = (ru_u(7.5f) - l->wc) / 2;
    if (pad < 1) pad = 1;
    lv_obj_set_style_radius(l->carc, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(l->carc, pad, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(l->carc, LV_OPA_TRANSP, LV_PART_KNOB);
  }
  l->disc = circle(root, l->disc_d, 0, 0);
  l->glyph = ru_icon_power(l->disc, ru_u(7), t->text_dim);
  lv_obj_align(l->glyph, LV_ALIGN_CENTER, 0, -ru_u(9.5f));
  l->hero = ru_label_new(l->disc, "--", ru_font_hero(), t->text_dim, l->disc_d / 2, l->disc_d / 2 + ru_u(1.2f));
  l->cap = ru_label_new(l->disc, "", ru_font_caption(), t->text_dim, l->disc_d / 2, l->disc_d / 2 + ru_u(10.5f));
  l->touch = lv_obj_create(root);
  lv_obj_remove_style_all(l->touch);
  lv_obj_set_size(l->touch, S, S);
  lv_obj_align(l->touch, LV_ALIGN_CENTER, 0, 0);
  lv_obj_remove_flag(l->touch, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(l->touch, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(l->touch, lt_touch_event, LV_EVENT_ALL, NULL);

  /* chips: square panels only for lights with colour AND white (a segmented pill COLOUR | WHITE); round panels one per ring the light
   * has, they switch the single ring */
  bool rgb = caps & RU_LIGHT_CAP_RGB, white = caps & (RU_LIGHT_CAP_W | RU_LIGHT_CAP_CT);
  if (two) {
    if (rgb && white) { l->chip_kind[l->nchip++] = CHIP_COLOUR; l->chip_kind[l->nchip++] = CHIP_WHITE; }
  } else if (colour) {
    l->chip_kind[l->nchip++] = CHIP_BRI;
    if (rgb) l->chip_kind[l->nchip++] = CHIP_COLOUR;
    if (white) l->chip_kind[l->nchip++] = CHIP_WHITE;
  }
  if (l->nchip) {
    int pw = ru_u(l->nchip == 3 ? 46 : 34), ph = ru_u(8.5f);
    l->pill = lv_obj_create(root);
    lv_obj_remove_style_all(l->pill);
    lv_obj_set_size(l->pill, pw, ph);
    lv_obj_remove_flag(l->pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(l->pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(l->pill, t->surface, 0);
    lv_obj_set_style_bg_opa(l->pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(l->pill, 1, 0);
    lv_obj_set_style_border_color(l->pill, ru_dim(t->text_dim, 120), 0);
    lv_obj_align(l->pill, LV_ALIGN_CENTER, 0, ru_u(27));
    static const char *const names[3] = { "BRIGHT", "COLOUR", "WHITE" };
    int cw = (pw - 4) / l->nchip, lh = lv_font_get_line_height(ru_font_caption());
    for (int i = 0; i < l->nchip; i++) {
      lv_obj_t *c = lv_label_create(l->pill);
      l->chip[i] = c;
      lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_CLIP);
      lv_label_set_text(c, names[l->chip_kind[i]]);
      lv_obj_set_size(c, cw, ph - 4);
      lv_obj_set_pos(c, 2 + i * cw, 2);
      lv_obj_set_style_text_font(c, ru_font_caption(), 0);
      lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_pad_top(c, (ph - 4 - lh) / 2, 0);
      lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
      lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(c, lt_chip_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
  }
  lt_paint(l);
  return root;
}

void ru_light_set(lv_obj_t *w, const ru_light_state_t *s) {
  light_t *l = lt_get(w);
  bool first = !l->have_st;
  int prev_mode = l->st.mode;
  bool changed = first || l->st.known != s->known || l->st.on != s->on || l->st.level != s->level || l->st.mode != s->mode ||
                 l->st.has_hue != s->has_hue || l->st.hue != s->hue || l->st.warmth != s->warmth || l->st.color != s->color;
  if (!changed) return;
  l->st = *s;
  l->have_st = true;
  if (!first) l->pend = false; /* the model moved: the knob goes to the model value */
  lt_derive_view(l, prev_mode, first);
  if (!l->dragging) lt_paint(l);
}

void ru_light_set_feedback(lv_obj_t *w, ru_light_fb_t st, const char *text) {
  light_t *l = lt_get(w);
  if (!text) text = "";
  if (l->fb == st && !strcmp(l->fb_text, text)) return;
  if (st != RU_LIGHT_FB_NONE && st != RU_LIGHT_FB_SENDING) l->pend = false;   /* the result is in: the knob goes to the model value */
  if (st == RU_LIGHT_FB_NONE && l->fb == RU_LIGHT_FB_SENDING) l->pend = false;
  l->fb = st;
  snprintf(l->fb_text, sizeof l->fb_text, "%s", text);
  if (!l->dragging) lt_paint(l);
}

void ru_light_set_readonly(lv_obj_t *w, bool ro, const char *text) {
  light_t *l = lt_get(w);
  if (!text) text = "";
  if (l->ro == ro && !strcmp(l->ro_text, text)) return;
  l->ro = ro;
  snprintf(l->ro_text, sizeof l->ro_text, "%s", text);
  if (!l->dragging) lt_paint(l);
}

void ru_light_on_change(lv_obj_t *w, ru_light_cb_t cb, void *ud) {
  light_t *l = lt_get(w);
  l->cb = cb;
  l->ud = ud;
}

bool ru_light_ring_at(const lv_obj_t *w, int x, int y) {
  light_t *l = lt_get(w);
  lt_zone_t z = lt_zone_of(l, x, y, NULL);
  return z == ZONE_BRI || z == ZONE_COL;
}

lv_obj_t *ru_light_part(const lv_obj_t *w, int which) {
  light_t *l = lt_get(w);
  switch (which) {
    case 0: return l->root;
    case 1: return l->bri.arc;
    case 2: return l->carc;
    case 3: return l->disc;
    case 4: return l->hero;
    case 5: return l->cap;
    case 6: return l->touch;
    case 7: return l->pill;
    case 8: return l->glyph;
    default: return NULL;
  }
}

int ru_light_view(const lv_obj_t *w) {
  light_t *l = lt_get(w);
  if (!l->carc || (!l->cfg.two_rings && !l->show_col)) return -1;
  return l->vkind;
}

int ru_light_chips(const lv_obj_t *w) { return lt_get(w)->nchip; }
void ru_light_press_chip(lv_obj_t *w, int i) { lt_chip_apply(lt_get(w), i); }
