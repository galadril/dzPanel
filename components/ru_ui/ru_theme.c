#include "ru_theme.h"
#include <math.h>

static ru_theme_t g_t;
static ru_geom_t g_geom;

static const uint32_t k_accent[] = { 0xFF8A3D, 0x3DE0C5, 0x8C7CFF, 0xFF5C8A, 0xB6F25A, 0xE6E9EE };

void ru_theme_init(int diameter) {
  ru_geom_init(&g_geom, diameter);
  g_t.void_bg = lv_color_hex(0x000000);
  g_t.surface = lv_color_hex(0x0E1013);
  g_t.track = lv_color_hex(0x15171B);
  g_t.hairline = lv_color_hex(0x4A2B14);
  g_t.text = lv_color_hex(0xE6E9EE);
  g_t.text_dim = lv_color_hex(0x8A93A0);
  g_t.ok = lv_color_hex(0x3DDC84);
  g_t.warn = lv_color_hex(0xFFC247);
  g_t.error = lv_color_hex(0xFF5A5F);
  g_t.info = lv_color_hex(0x4DB3FF);
  g_t.stale = lv_color_hex(0x8A93A0);
  ru_theme_set_accent(RU_ACCENT_EMBER);
}

const ru_theme_t *ru_theme(void) { return &g_t; }
const ru_geom_t *ru_theme_geom(void) { return &g_geom; }

void ru_theme_set_accent(ru_accent_t a) {
  if ((unsigned)a > RU_ACCENT_MONO) a = RU_ACCENT_EMBER;
  g_t.accent = lv_color_hex(k_accent[a]);
  /* hairline = accent at about 28% */
  g_t.hairline = ru_dim(g_t.accent, 72);
}

int ru_u(float units) {
  int px = (int)lroundf(units * g_geom.u);
  if (px < 1 && units > 0) px = 1;
  return px;
}

lv_color_t ru_dim(lv_color_t c, uint8_t f) {
  return lv_color_make((uint8_t)(c.red * f / 255), (uint8_t)(c.green * f / 255), (uint8_t)(c.blue * f / 255));
}

const lv_font_t *ru_font_hero(void) { return &lv_font_montserrat_48; }
const lv_font_t *ru_font_title(void) { return g_geom.diameter >= 400 ? &lv_font_montserrat_24 : &lv_font_montserrat_20; }
const lv_font_t *ru_font_body(void) { return &lv_font_montserrat_16; }
const lv_font_t *ru_font_small(void) { return &lv_font_montserrat_12; }

lv_obj_t *ru_cont_new(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return o;
}

lv_obj_t *ru_screen_new(void) {
  lv_obj_t *s = lv_obj_create(NULL);
  lv_obj_remove_style_all(s);
  lv_obj_set_size(s, g_geom.diameter, g_geom.diameter);
  lv_obj_set_style_bg_color(s, g_t.void_bg, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

lv_obj_t *ru_label_new(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color, int cx, int cy) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  lv_obj_update_layout(parent); /* parent size must be resolved before computing the offset */
  /* centre-aligned so later text changes stay centred on (cx,cy) */
  lv_obj_align(l, LV_ALIGN_CENTER, cx - lv_obj_get_width(parent) / 2, cy - lv_obj_get_height(parent) / 2);
  return l;
}
