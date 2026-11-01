#include "ru_climate_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ru_theme.h"

static lv_obj_t *label_u(lv_obj_t *scr, const char *txt, const lv_font_t *f, lv_color_t c, float dx_u, float dy_u) {
  int D = ru_theme_geom()->diameter;
  return ru_label_new(scr, txt, f, c, D / 2 + ru_u(dx_u), D / 2 + ru_u(dy_u));
}

lv_obj_t *ru_climate_next_zone(lv_obj_t *s, lv_event_cb_t cb, void *ud) {
  lv_obj_t *z = ru_cont_new(s, 0, 0, ru_u(34), ru_u(11));
  lv_obj_align(z, LV_ALIGN_CENTER, 0, ru_u(-38));
  lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(z, cb, LV_EVENT_CLICKED, ud);
  return z;
}

lv_obj_t *ru_climate_tap_zone(lv_obj_t *s, lv_event_cb_t cb, void *ud) {
  lv_obj_t *z = ru_cont_new(s, 0, 0, ru_u(46), ru_u(46));
  lv_obj_align(z, LV_ALIGN_CENTER, 0, ru_u(-4));
  lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(z, cb, LV_EVENT_CLICKED, ud);
  return z;
}

/* ---- compass: a ring, a bright marker at the bearing the wind comes from, N E S W, speed in the middle ---- */
static void compass(lv_obj_t *s, const ru_cl_view_t *v, ru_val_state_t st, float cy_u, float d_u) {
  const ru_theme_t *t = ru_theme();
  int D = ru_theme_geom()->diameter;
  int d = ru_u(d_u), w = ru_u(1.6f);
  lv_obj_t *track = lv_arc_create(s);
  lv_obj_remove_style_all(track);
  lv_obj_set_size(track, d, d);
  lv_obj_align(track, LV_ALIGN_CENTER, 0, ru_u(cy_u));
  lv_arc_set_bg_angles(track, 0, 360);
  lv_obj_set_style_arc_width(track, w, LV_PART_MAIN);
  lv_obj_set_style_arc_color(track, t->track, LV_PART_MAIN);
  lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
  lv_color_t hi = st == RU_VAL_STALE ? t->stale : t->info;
  if (v->has_dir && isfinite(v->dir_deg)) {
    /* LVGL arcs run clockwise from 3 o'clock; a compass bearing runs clockwise from 12 o'clock */
    float a = fmodf(v->dir_deg - 90.0f + 360.0f, 360.0f);
    lv_obj_t *m = lv_arc_create(s);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, d, d);
    lv_obj_align(m, LV_ALIGN_CENTER, 0, ru_u(cy_u));
    int a0 = (int)lroundf(a - 9.0f), a1 = (int)lroundf(a + 9.0f);
    a0 = (a0 % 360 + 360) % 360;
    a1 = (a1 % 360 + 360) % 360;
    lv_arc_set_bg_angles(m, (lv_value_precise_t)a0, (lv_value_precise_t)a1);
    lv_obj_set_style_arc_width(m, ru_u(2.8f), LV_PART_MAIN);
    lv_obj_set_style_arc_color(m, hi, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(m, true, LV_PART_MAIN);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_CLICKABLE);
  }
  /* cardinal letters just inside the ring */
  static const struct { const char *l; float dx, dy; } card[4] = {{"N", 0, -1}, {"E", 1, 0}, {"S", 0, 1}, {"W", -1, 0}};
  float r = d_u / 2 - 4.2f;
  for (int i = 0; i < 4; i++)
    label_u(s, card[i].l, ru_font_small(), i == 0 ? t->text : t->text_dim, card[i].dx * r, cy_u + card[i].dy * r);
  (void)D;
}

static void status_line(lv_obj_t *s, const ru_cl_page_ctx_t *c) {
  const ru_theme_t *t = ru_theme();
  if (c->st == RU_VAL_STALE) label_u(s, c->stale_text && c->stale_text[0] ? c->stale_text : "STALE", ru_font_small(), t->warn, 0, 37.6f);
  else if (c->v->battery_low) label_u(s, "BATTERY LOW", ru_font_small(), t->warn, 0, 37.6f);
}

static const char *trend_glyph(int tr) {
  return tr == DZ_TREND_UP ? LV_SYMBOL_UP : tr == DZ_TREND_DOWN ? LV_SYMBOL_DOWN : tr == DZ_TREND_STABLE ? LV_SYMBOL_RIGHT : "";
}

static void lines(lv_obj_t *s, const ru_cl_view_t *v, float y0) {
  const ru_theme_t *t = ru_theme();
  for (int i = 0; i < v->n_lines; i++)
    label_u(s, v->line[i].text, ru_font_body(), v->line[i].warn ? t->warn : t->text_dim, 0, y0 + 5.5f * (float)i);
}

void ru_climate_draw_page(lv_obj_t *s, const ru_cl_page_ctx_t *c) {
  const ru_theme_t *t = ru_theme();
  const ru_cl_view_t *v = c->v;
  bool stale = c->st == RU_VAL_STALE;
  lv_color_t num = stale ? t->stale : t->text;
  if (!c->no_name) label_u(s, v->name, ru_font_body(), t->text_dim, 0, -40.5f);
  if (c->n > 1) {
    char k[24];
    snprintf(k, sizeof k, "%d/%d", c->k + 1, c->n);
    label_u(s, k, ru_font_small(), t->text_dim, 0, -35.5f);
  }
  switch (v->page) {
    case RU_CLP_TEMP:
    case RU_CLP_HUM:
    case RU_CLP_SETPOINT:
    case RU_CLP_UV: {
      lv_obj_t *g = ru_ring_gauge_create(s, ru_u(42), v->caption);
      lv_obj_align(g, LV_ALIGN_CENTER, 0, ru_u(-8));
      ru_ring_gauge_set_color(g, v->page == RU_CLP_UV ? t->warn : v->page == RU_CLP_HUM ? t->ok : v->page == RU_CLP_SETPOINT ? t->accent : t->info);
      ru_ring_gauge_set_display(g, v->hero, v->ring, v->known ? c->st : RU_VAL_UNKNOWN);
      if (v->trend != DZ_TREND_UNKNOWN) {
        /* an arrow next to the caption inside the ring, never colour alone: up, down or steady */
        label_u(s, trend_glyph(v->trend), ru_font_small(), stale ? t->stale : t->text_dim, 7.5f, -8 + 5.5f);
      }
      float y = 20.5f;
      if (v->word[0]) {
        label_u(s, v->word, ru_font_body(), v->page == RU_CLP_UV ? t->warn : t->text, 0, y);
        y += 5.5f;
      }
      if (v->n_lines > 3 - (v->word[0] ? 1 : 0)) { /* keep three rows at most */ }
      lines(s, v, y);
      break;
    }
    case RU_CLP_WIND: {
      compass(s, v, c->st, -8.0f, 42.0f);
      label_u(s, v->speed, ru_font_title(), v->known ? num : t->stale, 0, -9.5f);
      if (v->speed_unit[0]) label_u(s, v->speed_unit, ru_font_small(), t->text_dim, 0, -4.2f);
      lines(s, v, 20.5f);
      break;
    }
    case RU_CLP_RAIN:
    case RU_CLP_AIR:
    case RU_CLP_VALUE: {
      /* the number is the hero; the caption says what it is; extra readings follow as lines */
      label_u(s, v->known ? v->hero : "--", ru_font_hero(), v->known ? num : t->stale, 0, -12);
      label_u(s, v->caption, ru_font_title(), t->text_dim, 0, 2.5f);
      float y = 14.5f;
      if (v->word[0]) {
        label_u(s, v->word, ru_font_title(), t->text, 0, y + 1.5f);
        y += 8.0f;
      }
      lines(s, v, y + 2.0f);
      break;
    }
    case RU_CLP_TEXT: {
      lv_obj_t *l = label_u(s, v->known ? v->text : "--", ru_font_title(), v->known ? num : t->stale, 0, -8);
      lv_obj_set_width(l, ru_u(60));
      lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(l, LV_ALIGN_CENTER, 0, ru_u(-8));
      break;
    }
    case RU_CLP_NONE:
      label_u(s, "--", ru_font_hero(), t->stale, 0, -10);
      label_u(s, "NO READING", ru_font_body(), t->text_dim, 0, 6);
      break;
  }
  status_line(s, c);
}

lv_obj_t *ru_climate_overview(lv_obj_t *s, const char *title, const ru_cl_item_t *items, int n, lv_obj_t **tiles_out,
                              int tiles_cap) {
  const ru_theme_t *t = ru_theme();
  const ru_geom_t *g = ru_theme_geom();
  label_u(s, title, ru_font_title(), t->text_dim, 0, -40);
  float top_u = -29, bot_u = 36;
  float top = (float)g->diameter / 2 + top_u * g->u, bot = (float)g->diameter / 2 + bot_u * g->u;
  int w = (int)ru_geom_row_width(g, top, bot) - ru_u(2);
  lv_obj_t *list = lv_obj_create(s);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, w, (int)(bot - top));
  lv_obj_align(list, LV_ALIGN_CENTER, 0, ru_u((top_u + bot_u) / 2));
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(list, ru_u(1.5f), 0);
  for (int i = 0; i < n; i++) {
    lv_obj_t *tile = ru_value_tile_create(list, w, ru_u(12), items[i].name);
    ru_value_tile_set_text(tile, items[i].value, items[i].known ? items[i].st : RU_VAL_UNKNOWN);
    if (tiles_out && i < tiles_cap) tiles_out[i] = tile;
  }
  return list;
}
