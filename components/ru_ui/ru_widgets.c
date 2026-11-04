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
} ring_t;

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
  for (int k = 0; k < GLOW_N; k++) lv_arc_set_value(r->glow[k], v);
  lv_arc_set_value(r->arc, v);
}

static void ring_set_lit(ring_t *r, bool lit, lv_color_t c, bool glow) {
  r->lit = lit;
  r->color = c;
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
  ru_val_state_t state;
  ru_tap_cb_t tap;
  void *tap_ud;
} tile_t;

static void tile_apply(lv_obj_t *tile, const char *text, ru_val_state_t st) {
  tile_t *d = wd_get(tile);
  const ru_theme_t *t = ru_theme();
  d->state = st;
  lv_label_set_text(d->value, text);
  lv_color_t c = st == RU_VAL_OK ? d->ok_color : t->stale;
  lv_obj_set_style_text_color(d->value, c, 0);
  lv_obj_set_style_text_opa(d->value, st == RU_VAL_OK ? LV_OPA_COVER : (st == RU_VAL_STALE ? LV_OPA_70 : LV_OPA_COVER), 0);
  lv_obj_set_style_text_color(d->label, st == RU_VAL_OK ? t->text_dim : ru_dim(t->text_dim, 190), 0);
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
  d->ok_color = c;
  if (d->state == RU_VAL_OK) lv_obj_set_style_text_color(d->value, c, 0);
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

typedef struct { ring_t ring; lv_obj_t *val, *cap; } gauge_t;

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
  if (g->ring.lit) ring_set_lit(&g->ring, true, c, true);
}

void ru_ring_gauge_set(lv_obj_t *box, const ru_val_t *v, float min, float max, uint32_t now_ms) {
  gauge_t *g = wd_get(box);
  const ru_theme_t *t = ru_theme();
  char buf[32];
  ru_val_state_t st = ru_val_state(v, now_ms);
  if (st == RU_VAL_UNKNOWN || max <= min) {
    lv_label_set_text(g->val, "--");
    lv_obj_set_style_text_color(g->val, t->stale, 0);
    ring_set(&g->ring, 0);
    ring_set_lit(&g->ring, false, g->ring.color, false);
    return;
  }
  fmt_value(v, buf, sizeof buf);
  lv_label_set_text(g->val, buf);
  float f = clampf((v->v - min) / (max - min), 0.0f, 1.0f);
  ring_set(&g->ring, (int)lroundf(f * 1000.0f));
  if (st == RU_VAL_STALE) {
    lv_obj_set_style_text_color(g->val, t->stale, 0);
    ring_set_lit(&g->ring, true, ru_dim(t->stale, 170), false);
  } else {
    lv_obj_set_style_text_color(g->val, t->text, 0);
    ring_set_lit(&g->ring, true, g->ring.color, true);
  }
}

void ru_ring_gauge_set_display(lv_obj_t *box, const char *text, float frac, ru_val_state_t st) {
  gauge_t *g = wd_get(box);
  const ru_theme_t *t = ru_theme();
  if (st == RU_VAL_UNKNOWN || !text || !text[0]) {
    lv_label_set_text(g->val, "--");
    lv_obj_set_style_text_color(g->val, t->stale, 0);
    ring_set(&g->ring, 0);
    ring_set_lit(&g->ring, false, g->ring.color, false);
    return;
  }
  lv_label_set_text(g->val, text);
  if (frac < 0.0f) { /* a value that cannot be scaled (unit unknown): the text only, an unlit ring */
    lv_obj_set_style_text_color(g->val, st == RU_VAL_STALE ? t->stale : t->text, 0);
    ring_set(&g->ring, 0);
    ring_set_lit(&g->ring, false, g->ring.color, false);
    return;
  }
  ring_set(&g->ring, (int)lroundf(clampf(frac, 0.0f, 1.0f) * 1000.0f));
  if (st == RU_VAL_STALE) {
    lv_obj_set_style_text_color(g->val, t->stale, 0);
    ring_set_lit(&g->ring, true, ru_dim(t->stale, 170), false);
  } else {
    lv_obj_set_style_text_color(g->val, t->text, 0);
    ring_set_lit(&g->ring, true, g->ring.color, true);
  }
}

/* ------------------------------------------------------------ status rim */

typedef struct { lv_obj_t *line, *dot, *bead, *bead_halo; int D; } rim_t;

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
  s->st = st;
  snprintf(s->subtxt, sizeof s->subtxt, "%s", sub);
  lv_label_set_text(s->sub, s->subtxt);
  lv_obj_align(s->sub, LV_ALIGN_CENTER, 0, lv_obj_get_height(o) * 83 / 100 - lv_obj_get_height(o) / 2);
  lv_color_t on = fresh == RU_VAL_STALE ? ru_dim(t->stale, 170) : t->accent;
  if (st != RU_DOT_NONE && (st == RU_DOT_UNKNOWN || fresh == RU_VAL_UNKNOWN)) { st = RU_DOT_UNKNOWN; s->st = st; }
  ru_state_dot_set(s->dot, st, on);
  if (st == RU_DOT_NONE) lv_obj_remove_flag(s->val, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s->val, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_color(s->val, fresh == RU_VAL_STALE ? t->stale : fresh == RU_VAL_UNKNOWN ? t->text_dim : t->text, 0);
  lv_obj_set_style_text_color(s->name, fresh == RU_VAL_STALE ? t->text_dim : t->text, 0);
  lv_obj_set_style_text_color(s->sub, st == RU_DOT_MIXED ? t->warn : (fresh == RU_VAL_STALE ? t->stale : t->text_dim), 0);
}

void ru_state_tile_set_value(lv_obj_t *o, const char *text) {
  stile_t *s = wd_get(o);
  lv_label_set_text(s->val, text);
  /* long values (text sensors) drop to the body font so they fit the tile */
  const lv_font_t *f = strlen(text) > 5 ? ru_font_body() : ru_font_title();
  int room = lv_obj_get_width(o) - ru_u(2); /* same as the label width set at creation */
  lv_point_t sz;
  lv_text_get_size(&sz, text, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  if (sz.x > room) f = ru_font_small(); /* two readings ("19.8°C  45%") on one line */
  lv_obj_set_style_text_font(s->val, f, 0);
  lv_obj_set_height(s->val, lv_font_get_line_height(f)); /* one line: LONG_DOT cuts with dots instead of wrapping over the name */
  lv_obj_align(s->val, LV_ALIGN_CENTER, 0, lv_obj_get_height(o) * 28 / 100 - lv_obj_get_height(o) / 2);
}

ru_dot_state_t ru_state_tile_dot(const lv_obj_t *o) { return ((stile_t *)wd_get(o))->st; }
const char *ru_state_tile_sub(const lv_obj_t *o) { return ((stile_t *)wd_get(o))->subtxt; }
void ru_state_tile_on_tap(lv_obj_t *o, ru_tap_cb_t cb, void *ud) {
  stile_t *s = wd_get(o);
  s->cb = cb;
  s->ud = ud;
}
