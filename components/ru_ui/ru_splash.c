#include "ru_splash.h"

#include <string.h>

#include "ru_theme.h"

typedef struct {
  lv_obj_t *scr;
  lv_obj_t *track, *ring, *bead;
  lv_obj_t *name, *name2, *tagline, *version; /* name2: the blue "Panel" of the two-tone logo, else NULL */
  lv_point_precise_t house[6];
  lv_obj_t *mark;                            /* house mark container inside the ring */
  lv_obj_t *code, *hint;
  int ring_r;       /* radius of the ring centreline, px */
  int cy;           /* ring centre offset from the screen centre, px (negative = up) */
  int permille;     /* last drawn progress */
  bool error;
  bool complete;
} splash_t;

#define STATE(obj) ((splash_t *)lv_obj_get_user_data(obj))

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* bead position for a clockwise angle from 12 o'clock, in degrees */
static void place_bead(splash_t *s, int deg) {
  deg = ((deg % 360) + 360) % 360;
  int32_t sn = lv_trigo_sin((int16_t)deg), cs = lv_trigo_cos((int16_t)deg);
  int x = (int)(((int64_t)s->ring_r * sn) / 32768);
  int y = -(int)(((int64_t)s->ring_r * cs) / 32768);
  lv_obj_align(s->bead, LV_ALIGN_CENTER, x, s->cy + y);
}

/* 0..255 opacity ramp over permille [from, to] */
static lv_opa_t ramp(int p, int from, int to) {
  if (p <= from) return 0;
  if (p >= to) return 255;
  return (lv_opa_t)(((p - from) * 255) / (to - from));
}

static void draw(splash_t *s, int permille) {
  permille = clampi(permille, 0, 1000);
  s->permille = permille;
  /* the ring takes the first 900 ms of the 1200 ms: permille 0..750 sweeps, 750..1000 fades the words */
  int sweep = clampi((permille * 1000) / 750, 0, 1000);
  lv_arc_set_value(s->ring, sweep);
  lv_obj_set_style_opa(s->ring, sweep == 0 ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
  place_bead(s, (sweep * 360) / 1000);
  lv_obj_set_style_opa(s->bead, permille == 0 ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
  lv_obj_set_style_text_opa(s->name, ramp(permille, 750, 1000), 0);
  if (s->name2) lv_obj_set_style_text_opa(s->name2, ramp(permille, 750, 1000), 0);
  lv_obj_set_style_opa(s->mark, ramp(permille, 450, 800), 0);
  lv_obj_set_style_text_opa(s->tagline, ramp(permille, 800, 1000), 0);
  if (s->version) lv_obj_set_style_text_opa(s->version, ramp(permille, 750, 1000), 0);
  s->complete = permille >= 1000;
}

static void anim_progress_cb(void *var, int32_t v) {
  splash_t *s = var;
  if (!s->error) draw(s, v);
}

static void anim_orbit_cb(void *var, int32_t v) {
  splash_t *s = var;
  if (!s->error) place_bead(s, v);
}

static void orbit_start(splash_t *s) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s);
  lv_anim_set_exec_cb(&a, anim_orbit_cb);
  lv_anim_set_values(&a, 0, 360);
  lv_anim_set_duration(&a, 2400);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_linear);
  lv_anim_start(&a);
}

static void draw_done_cb(lv_anim_t *a) {
  splash_t *s = lv_anim_get_user_data(a);
  if (s && !s->error) orbit_start(s);
}

static void on_delete(lv_event_t *e) {
  splash_t *s = lv_event_get_user_data(e);
  lv_anim_delete(s, NULL);
  lv_free(s);
}

/* The house mark inside the ring, original artwork: a round-capped outline (roof, walls, floor) and a 2x2 window grid,
 * all in brand blue. Units are u = D/100 around the ring centre; it stays well inside the ring's inner edge. */
static void build_mark(splash_t *s, int d) {
  const ru_theme_t *t = ru_theme();
  int D = ru_theme_geom()->diameter;
  s->mark = ru_cont_new(s->scr, D / 2 - d / 2, D / 2 + s->cy - d / 2, d, d);
  int cx = d / 2, cy = d / 2 + ru_u(0.5f);
  const float u[6][2] = { { -7.2f, 7.5f }, { -7.2f, -1.2f }, { 0, -7.8f }, { 7.2f, -1.2f }, { 7.2f, 7.5f }, { -7.2f, 7.5f } };
  for (int i = 0; i < 6; i++) {
    s->house[i].x = cx + ru_u(u[i][0]);
    s->house[i].y = cy + ru_u(u[i][1]);
  }
  lv_obj_t *ln = lv_line_create(s->mark);
  lv_line_set_points(ln, s->house, 6);
  lv_obj_set_pos(ln, 0, 0);
  lv_obj_set_style_line_width(ln, ru_u(1.6f), 0);
  lv_obj_set_style_line_color(ln, t->brand, 0);
  lv_obj_set_style_line_rounded(ln, true, 0);
  int q = ru_u(2.6f), g = ru_u(1.0f);
  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 2; c++) {
      lv_obj_t *sq = lv_obj_create(s->mark);
      lv_obj_remove_style_all(sq);
      lv_obj_set_size(sq, q, q);
      lv_obj_set_pos(sq, cx - q - g / 2 + c * (q + g), cy - q + r * (q + g) - ru_u(1.0f));
      lv_obj_set_style_radius(sq, ru_u(0.6f), 0);
      lv_obj_set_style_bg_color(sq, t->brand, 0);
      lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
      lv_obj_remove_flag(sq, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
}

static lv_obj_t *two_tone_label(lv_obj_t *row, const char *text, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(row);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  return l;
}

static lv_obj_t *build(lv_obj_t *root, const ru_splash_cfg_t *cfg) {
  const ru_theme_t *t = ru_theme();
  splash_t *s = lv_malloc_zeroed(sizeof *s);
  if (!s) {
    lv_obj_delete(root);
    return NULL;
  }
  s->scr = root;
  lv_obj_set_user_data(s->scr, s);
  lv_obj_add_event_cb(s->scr, on_delete, LV_EVENT_DELETE, s);

  const char *name = cfg && cfg->name ? cfg->name : "dzPanel";
  const char *tag = cfg && cfg->tagline ? cfg->tagline : "for Domoticz";
  const char *ver = cfg ? cfg->version : NULL;

  int D = ru_theme_geom()->diameter, mid = D / 2;
  int d = ru_u(40), w = ru_u(2.2f);
  s->ring_r = (d - w) / 2;
  s->cy = -ru_u(9);

  /* unlit track, so the ring is seen "drawing" along a faint guide */
  s->track = lv_arc_create(s->scr);
  lv_obj_remove_style_all(s->track);
  lv_obj_set_size(s->track, d, d);
  lv_obj_align(s->track, LV_ALIGN_CENTER, 0, s->cy);
  lv_arc_set_bg_angles(s->track, 0, 360);
  lv_arc_set_range(s->track, 0, 1000);
  lv_arc_set_value(s->track, 0);
  lv_obj_set_style_arc_width(s->track, w, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s->track, t->track, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(s->track, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s->track, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_remove_flag(s->track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  s->ring = lv_arc_create(s->scr);
  lv_obj_remove_style_all(s->ring);
  lv_obj_set_size(s->ring, d, d);
  lv_obj_align(s->ring, LV_ALIGN_CENTER, 0, s->cy);
  lv_arc_set_rotation(s->ring, 270); /* start at 12 o'clock, clockwise */
  lv_arc_set_bg_angles(s->ring, 0, 360);
  lv_arc_set_range(s->ring, 0, 1000);
  lv_arc_set_value(s->ring, 0);
  lv_obj_set_style_arc_opa(s->ring, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s->ring, w, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s->ring, t->brand, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(s->ring, true, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s->ring, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_remove_flag(s->ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  int bd = w + ru_u(1.2f);
  s->bead = lv_obj_create(s->scr);
  lv_obj_remove_style_all(s->bead);
  lv_obj_set_size(s->bead, bd, bd);
  lv_obj_set_style_radius(s->bead, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s->bead, t->brand, 0);
  lv_obj_set_style_bg_opa(s->bead, LV_OPA_COVER, 0);
  lv_obj_remove_flag(s->bead, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  build_mark(s, d);

  /* "dzPanel" (or no name) is the two-tone logo in the 48 px face: "dz" in text colour, "Panel" in brand blue.
   * Any other name is a plain label. */
  const lv_font_t *lf = &lv_font_montserrat_48;
  int wy = mid + ru_u(16);
  if (strcmp(name, "dzPanel") == 0) {
    lv_obj_t *row = lv_obj_create(s->scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s->name = two_tone_label(row, "dz", lf, t->text);
    s->name2 = two_tone_label(row, "Panel", lf, t->brand);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, wy - mid);
  } else {
    s->name = ru_label_new(s->scr, name, ru_font_title(), t->text, mid, wy);
  }
  s->tagline = ru_label_new(s->scr, tag, ru_font_small(), t->text_dim, mid, mid + ru_u(25));
  if (ver && ver[0]) s->version = ru_label_new(s->scr, ver, ru_font_small(), ru_dim(t->text_dim, 150), mid, mid + ru_u(40));
  draw(s, 0);
  return s->scr;
}

lv_obj_t *ru_splash_create(const ru_splash_cfg_t *cfg) { return build(ru_screen_new(), cfg); }

lv_obj_t *ru_splash_overlay_create(const ru_splash_cfg_t *cfg) {
  lv_obj_t *root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(root);
  lv_obj_set_pos(root, 0, 0);
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(root, ru_theme()->void_bg, 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE); /* swallows touches while it covers the real screen */
  return build(root, cfg);
}

void ru_splash_dismiss(lv_obj_t *splash, uint32_t fade_ms) {
  if (!splash) return;
  if (fade_ms == 0) {
    lv_obj_delete(splash);
    return;
  }
  lv_obj_remove_flag(splash, LV_OBJ_FLAG_CLICKABLE); /* the screen underneath is usable during the fade */
  lv_obj_fade_out(splash, fade_ms, 0);
  lv_obj_delete_delayed(splash, fade_ms + 20);
}

void ru_splash_set_progress(lv_obj_t *splash, int permille) {
  splash_t *s = splash ? STATE(splash) : NULL;
  if (s && !s->error) draw(s, permille);
}

void ru_splash_start(lv_obj_t *splash) {
  splash_t *s = splash ? STATE(splash) : NULL;
  if (!s || s->error) return;
  lv_anim_delete(s, NULL);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s);
  lv_anim_set_exec_cb(&a, anim_progress_cb);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_duration(&a, RU_SPLASH_TOTAL_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_user_data(&a, s);
  lv_anim_set_completed_cb(&a, draw_done_cb);
  lv_anim_start(&a);
}

void ru_splash_set_error(lv_obj_t *splash, const char *code, const char *hint) {
  splash_t *s = splash ? STATE(splash) : NULL;
  if (!s) return;
  const ru_theme_t *t = ru_theme();
  lv_anim_delete(s, NULL);
  s->error = true;
  s->complete = true;
  /* an open ring in the error colour: three quarters drawn, the bead parked where the ring stops */
  lv_obj_set_style_arc_color(s->ring, t->error, LV_PART_INDICATOR);
  lv_obj_set_style_opa(s->ring, LV_OPA_COVER, 0);
  lv_arc_set_value(s->ring, 750);
  lv_obj_set_style_bg_color(s->bead, t->error, 0);
  lv_obj_set_style_opa(s->bead, LV_OPA_COVER, 0);
  place_bead(s, 270);
  lv_obj_set_style_text_opa(s->name, LV_OPA_COVER, 0);
  if (s->name2) lv_obj_set_style_text_opa(s->name2, LV_OPA_COVER, 0);
  lv_obj_set_style_opa(s->mark, LV_OPA_COVER, 0);
  lv_obj_set_style_text_opa(s->tagline, LV_OPA_COVER, 0);
  if (s->version) lv_obj_add_flag(s->version, LV_OBJ_FLAG_HIDDEN); /* the error text takes its place */
  if (!s->code && code && code[0]) s->code = ru_label_new(s->scr, code, ru_font_body(), t->error, ru_theme_geom()->diameter / 2, ru_theme_geom()->diameter / 2 + ru_u(33));
  else if (s->code) lv_label_set_text(s->code, code ? code : "");
  if (!s->hint && hint && hint[0]) s->hint = ru_label_new(s->scr, hint, ru_font_small(), t->text_dim, ru_theme_geom()->diameter / 2, ru_theme_geom()->diameter / 2 + ru_u(40));
  else if (s->hint) lv_label_set_text(s->hint, hint ? hint : "");
}

bool ru_splash_is_complete(lv_obj_t *splash) {
  splash_t *s = splash ? STATE(splash) : NULL;
  return s && s->complete;
}
