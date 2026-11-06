/* Halo theme tokens: Void black + Ember accent. DESIGN.md section 8.
 * Depends on LVGL and ru_geom only, never on Domoticz types. */
#pragma once
#include "lvgl.h"
#include "ru_geom.h"

typedef struct {
  lv_color_t void_bg;     /* #000000 */
  lv_color_t surface;     /* #0E1013 raised surface */
  lv_color_t track;       /* #15171B unlit ring track */
  lv_color_t hairline;    /* dim rim, at or below 30% luminance */
  lv_color_t text;        /* #E6E9EE */
  lv_color_t text_dim;    /* #8A93A0 */
  lv_color_t accent;      /* Ember #FF8A3D by default */
  lv_color_t ok, warn, error, info, stale;
} ru_theme_t;

typedef enum { RU_ACCENT_EMBER, RU_ACCENT_AURORA, RU_ACCENT_IRIS, RU_ACCENT_ROSE, RU_ACCENT_LIME, RU_ACCENT_MONO } ru_accent_t;

/* Call once after the display exists. diameter = 466 or 480 (any value works). */
void ru_theme_init(int diameter);
const ru_theme_t *ru_theme(void);
const ru_geom_t *ru_theme_geom(void);
void ru_theme_set_accent(ru_accent_t a);

/* px for a size given in units u = D/100 (rounded, at least 1 for positive input) */
int ru_u(float units);
/* colour blended toward black: f in 0..255 (255 = unchanged) */
lv_color_t ru_dim(lv_color_t c, uint8_t f);

/* Fonts. Built-in Montserrat until Inter / Space Grotesk are baked offline. */
const lv_font_t *ru_font_hero(void);   /* big numerals */
const lv_font_t *ru_font_title(void);
const lv_font_t *ru_font_body(void);
const lv_font_t *ru_font_small(void);
/* Lucide icon font (see ru_icons.h for the glyph macros), 24 px, 4 bpp. A label with this font and one RU_ICON_* string. */
const lv_font_t *ru_font_icon_s(void);

/* A black, non-scrolling, circle-sized screen. */
lv_obj_t *ru_screen_new(void);
/* A transparent, non-scrolling, non-clickable container. */
lv_obj_t *ru_cont_new(lv_obj_t *parent, int x, int y, int w, int h);
/* A label with the theme font/colour, centred on (cx,cy) in the parent. */
lv_obj_t *ru_label_new(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color, int cx, int cy);
