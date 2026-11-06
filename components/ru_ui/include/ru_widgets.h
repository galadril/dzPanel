/* Halo widgets. Plain values only (ru_val_t, ints, strings): no Domoticz types.
 * Every widget is an lv_obj_t* owned by its parent; deleting the object frees its private data.
 * Callbacks run in the LVGL thread. Strings are copied. Call ru_theme_init() first. */
#pragma once
#include "lvgl.h"
#include "ru_data.h"
#include "ru_theme.h"

typedef void (*ru_tap_cb_t)(lv_obj_t *w, void *ud);
typedef void (*ru_bool_cb_t)(lv_obj_t *w, bool on, void *ud);
typedef void (*ru_int_cb_t)(lv_obj_t *w, int value, bool final, void *ud);       /* final = finger lifted */
typedef void (*ru_float_cb_t)(lv_obj_t *w, float value, bool final, void *ud);

/* ---- value tile: label left, value right. OK / STALE (dim + border + warn glyph) / UNKNOWN ("--", never 0) ---- */
lv_obj_t *ru_value_tile_create(lv_obj_t *parent, int w, int h, const char *label);
void ru_value_tile_set(lv_obj_t *tile, const ru_val_t *v, uint32_t now_ms);
/* Non-numeric states such as "ON"/"OFF". Freshness is given explicitly. */
void ru_value_tile_set_text(lv_obj_t *tile, const char *text, ru_val_state_t st);
void ru_value_tile_set_color(lv_obj_t *tile, lv_color_t ok_color);  /* colour of the value while OK */
ru_val_state_t ru_value_tile_state(const lv_obj_t *tile);
void ru_value_tile_on_tap(lv_obj_t *tile, ru_tap_cb_t cb, void *ud);

/* ---- ring gauge: value 0..1 across min..max, centre text, caption below it. d = outer diameter px ---- */
lv_obj_t *ru_ring_gauge_create(lv_obj_t *parent, int d, const char *caption);
void ru_ring_gauge_set(lv_obj_t *g, const ru_val_t *v, float min, float max, uint32_t now_ms);
void ru_ring_gauge_set_color(lv_obj_t *g, lv_color_t c);
/* Shows caller-formatted text (any unit) with the ring filled to frac (0..1). frac < 0: text only, ring unlit (the value
 * cannot be scaled). st UNKNOWN: "--". The widget itself never decides a unit. */
void ru_ring_gauge_set_display(lv_obj_t *g, const char *text, float frac, ru_val_state_t st);

/* ---- status rim: hairline ring at the screen edge, status dot at 1 o'clock, optional bead ---- */
typedef enum { RU_RIM_OK, RU_RIM_STALE, RU_RIM_ERROR, RU_RIM_OFFLINE } ru_rim_state_t;
lv_obj_t *ru_status_rim_create(lv_obj_t *parent);
void ru_status_rim_set_state(lv_obj_t *rim, ru_rim_state_t s);
void ru_status_rim_set_color(lv_obj_t *rim, lv_color_t hairline);
void ru_status_rim_set_bead(lv_obj_t *rim, bool visible, float deg_from_top);

/* ---- dimmer arc: draggable 270 degree arc, 0..max, hero number and caption in the middle ---- */
lv_obj_t *ru_dimmer_create(lv_obj_t *parent, int d, int max, const char *caption);
void ru_dimmer_set(lv_obj_t *w, int value);   /* programmatic, no callback */
int ru_dimmer_get(const lv_obj_t *w);
void ru_dimmer_set_unknown(lv_obj_t *w);      /* shows "--" and an unlit ring */
void ru_dimmer_on_change(lv_obj_t *w, ru_int_cb_t cb, void *ud);

/* ---- on/off toggle: big power button ---- */
typedef enum { RU_ONOFF_OFF, RU_ONOFF_ON, RU_ONOFF_UNKNOWN } ru_onoff_t;
lv_obj_t *ru_toggle_create(lv_obj_t *parent, int d);
void ru_toggle_set(lv_obj_t *w, ru_onoff_t s);  /* programmatic, no callback */
ru_onoff_t ru_toggle_get(const lv_obj_t *w);
void ru_toggle_on_change(lv_obj_t *w, ru_bool_cb_t cb, void *ud);  /* asked state; widget does not flip itself on UNKNOWN */

/* ---- selector wheel: names around a 270 degree arc; tap or drag to pick ---- */
lv_obj_t *ru_selector_create(lv_obj_t *parent, int d, const char *const *names, int n);
void ru_selector_set(lv_obj_t *w, int index);  /* programmatic, no callback */
int ru_selector_get(const lv_obj_t *w);
void ru_selector_on_change(lv_obj_t *w, ru_int_cb_t cb, void *ud);
lv_obj_t *ru_selector_centre(lv_obj_t *w);     /* transparent container in the middle for icons */

/* ---- blinds: window with slats (0 = open, 100 = closed), up / stop / down ---- */
typedef enum { RU_BLINDS_UP, RU_BLINDS_STOP, RU_BLINDS_DOWN } ru_blinds_cmd_t;
typedef void (*ru_blinds_cb_t)(lv_obj_t *w, ru_blinds_cmd_t cmd, void *ud);
lv_obj_t *ru_blinds_create(lv_obj_t *parent, int w, int h);
void ru_blinds_set(lv_obj_t *w, const ru_val_t *closed_pct, uint32_t now_ms);
void ru_blinds_on_cmd(lv_obj_t *w, ru_blinds_cb_t cb, void *ud);

/* ---- hold-to-confirm: press and hold for hold_ms; releasing early cancels ---- */
typedef enum { RU_HOLD_CONFIRMED, RU_HOLD_CANCELLED } ru_hold_event_t;
typedef void (*ru_hold_cb_t)(lv_obj_t *w, ru_hold_event_t ev, void *ud);
lv_obj_t *ru_hold_confirm_create(lv_obj_t *parent, int d, const char *text, uint32_t hold_ms);
void ru_hold_confirm_set_progress(lv_obj_t *w, int permille);  /* for screenshots and tests */
int ru_hold_confirm_progress(const lv_obj_t *w);
void ru_hold_confirm_on_event(lv_obj_t *w, ru_hold_cb_t cb, void *ud);

/* ---- scene button: rounded tile with colour dot, name and kind ("SCENE", "GROUP") ---- */
lv_obj_t *ru_scene_button_create(lv_obj_t *parent, int w, int h, const char *name, const char *kind, lv_color_t dot);
void ru_scene_button_on_tap(lv_obj_t *w, ru_tap_cb_t cb, void *ud);

/* ---- thermostat dial: setpoint arc min..max with step, current-temperature bead, - and + buttons ---- */
lv_obj_t *ru_thermostat_create(lv_obj_t *parent, int d, float min, float max, float step);
void ru_thermostat_set_setpoint(lv_obj_t *w, float sp);                         /* programmatic, no callback */
void ru_thermostat_set_current(lv_obj_t *w, const ru_val_t *cur, uint32_t now_ms);
void ru_thermostat_set_unit(lv_obj_t *w, const char *unit);   /* text after the numbers: "\xC2\xB0C", "%", "" (bare). Default: a bare degree sign */
void ru_thermostat_set_readonly(lv_obj_t *w, bool ro);         /* hides - and +, ignores the arc: a setpoint that cannot be changed */
void ru_thermostat_set_mode(lv_obj_t *w, const char *text, lv_color_t color);   /* e.g. "HEATING" */
float ru_thermostat_get_setpoint(const lv_obj_t *w);
void ru_thermostat_on_change(lv_obj_t *w, ru_float_cb_t cb, void *ud);

/* ---- icons drawn from primitives ---- */
lv_obj_t *ru_icon_power(lv_obj_t *parent, int size, lv_color_t c);
lv_obj_t *ru_icon_fan(lv_obj_t *parent, int size, lv_color_t c);
lv_obj_t *ru_icon_dot(lv_obj_t *parent, int size, lv_color_t c);

/* ---- state tile (Scenes and Groups, apps): a rounded tile with a state indicator, a name and a state line.
 * The indicator differs by SHAPE, not only colour: UNKNOWN dashed ring + "-", OFF hollow ring, ON filled disc,
 * MIXED ring with the left half filled, PUSH (a Scene: no state) ring with a centre core, NONE no indicator
 * (the tile then shows a value in its place, see ru_state_tile_set_value). ---- */
typedef enum { RU_DOT_UNKNOWN, RU_DOT_OFF, RU_DOT_ON, RU_DOT_MIXED, RU_DOT_PUSH, RU_DOT_NONE } ru_dot_state_t;
lv_obj_t *ru_state_dot_create(lv_obj_t *parent, int d);
void ru_state_dot_set(lv_obj_t *dot, ru_dot_state_t s, lv_color_t color);
lv_obj_t *ru_state_tile_create(lv_obj_t *parent, int w, int h, const char *name);
/* sub = the state line ("ON", "OFF", "MIXED", "SCENE", "ACTIVATED", "--"). fresh dims a STALE tile. */
void ru_state_tile_set(lv_obj_t *tile, ru_dot_state_t s, const char *sub, ru_val_state_t fresh);
ru_dot_state_t ru_state_tile_dot(const lv_obj_t *tile);
const char *ru_state_tile_sub(const lv_obj_t *tile);
/* The big text shown instead of the indicator when the state is RU_DOT_NONE (sensor values). */
void ru_state_tile_set_value(lv_obj_t *tile, const char *text);
void ru_state_tile_on_tap(lv_obj_t *tile, ru_tap_cb_t cb, void *ud);

/* ---- weather tile (Temperature and Weather pages): the device name on top, then the icon and the big value side by side,
 * centred, with the unit (and a trend chevron) in one small label after the value, and an optional small line below.
 * 7 objects, shared styles. The icon (and only the icon, plus the small line when the caller tints it) carries the band colour; the
 * value stays white. A stale or unknown tile greys value and icon, shows a warn dot in the bottom right corner and a 1 px
 * border: colour is never the only signal. The setter compares every field and touches only what changed. ---- */
typedef struct {
  const char *name;        /* the device name, one line, cut with dots */
  const char *icon;        /* UTF-8 of one icon-font glyph (ru_icons.h), "" for none */
  lv_color_t icon_color;   /* band colour of the icon; the theme text colour when there is no band */
  const char *value;       /* "21.5" */
  const char *unit;        /* "\xC2\xB0C", " hPa", may end with a trend chevron; "" for none */
  const char *second;      /* the small line, "" for none */
  lv_color_t second_color; /* theme dim colour unless the caller tints it (the UV word) */
  ru_val_state_t state;    /* OK, STALE, UNKNOWN */
} ru_wxtile_data_t;
lv_obj_t *ru_wx_tile_create(lv_obj_t *parent, int w, int h);
void ru_wx_tile_set(lv_obj_t *tile, const ru_wxtile_data_t *d);
void ru_wx_tile_on_tap(lv_obj_t *tile, ru_tap_cb_t cb, void *ud);
/* The label objects of a tile, for tests and layout checks: 0 name, 1 icon, 2 value, 3 unit, 4 second line, 5 stale dot. */
lv_obj_t *ru_wx_tile_part(const lv_obj_t *tile, int which);

/* ---- light control (device detail of dimmers and colour lights; see docs/research/ux.md "Light control").
 * Square panels: an outer BRIGHTNESS ring and an inner COLOUR / TEMPERATURE ring (conic image, see ru_ring_img.h); round panels: ONE ring
 * that the chips switch between brightness, colour and white. A centre disc is the ON/OFF button (power glyph, large percent, caption).
 * Commands are reported on RELEASE only (final = true); during a drag the callback is called with final = false so the app can keep
 * the screen from being rebuilt under the finger. Setters compare with what is shown and touch only what changed. ---- */
#include "ru_ring_img.h"
#define RU_LIGHT_CAP_RGB 1  /* same bits as DZ_COLCAP_* */
#define RU_LIGHT_CAP_W 2
#define RU_LIGHT_CAP_CT 4
typedef enum { RU_LIGHT_NONE = 0, RU_LIGHT_WHITE, RU_LIGHT_TEMP, RU_LIGHT_COLOR } ru_light_mode_t; /* the lamp's mode, as Color m 0..3 */
typedef enum { RU_LIGHT_CMD_LEVEL, RU_LIGHT_CMD_HUE, RU_LIGHT_CMD_TEMP, RU_LIGHT_CMD_WHITE, RU_LIGHT_CMD_POWER } ru_light_cmd_t;
typedef enum { RU_LIGHT_FB_NONE, RU_LIGHT_FB_SENDING, RU_LIGHT_FB_OK, RU_LIGHT_FB_WARN, RU_LIGHT_FB_ERROR } ru_light_fb_t;
typedef struct {
  ru_ring_color_fn hue_fn;          /* 0..1 along the sweep -> hue colour */
  ru_ring_color_fn temp_fn;         /* 0 warm .. 1 cool -> white colour */
  const char *(*hue_word)(float hue);
  const char *(*warm_word)(int warmth);
} ru_light_hooks_t;
typedef struct {
  int caps;          /* RU_LIGHT_CAP_*; 0 = plain dimmer */
  bool two_rings;    /* square panel: the two ring layout; round: one ring and chips */
  int max;           /* MaxDimLevel, 1..100 */
  const ru_light_hooks_t *hooks;
} ru_light_cfg_t;
typedef struct {
  bool known;        /* false: "--", rings unlit, no knobs */
  bool on;
  int level;         /* 0..max, the stored level also while off */
  int mode;          /* ru_light_mode_t */
  bool has_hue;      /* colour mode: the hue is known (a white-ish RGB has none: knob hidden) */
  int hue;           /* 0..359 */
  int warmth;        /* 0..100, temperature mode */
  uint32_t color;    /* tint of the lamp 0xRRGGBB (disc, outer ring); 0 = the theme accent */
} ru_light_state_t;
/* value: LEVEL device units 0..max; HUE 0..359; TEMP warmth 0..100; WHITE none; POWER 1 = switch on, 0 = off */
typedef void (*ru_light_cb_t)(lv_obj_t *w, ru_light_cmd_t cmd, int value, bool final, void *ud);
lv_obj_t *ru_light_create(lv_obj_t *parent, int d, const ru_light_cfg_t *cfg);
void ru_light_set(lv_obj_t *w, const ru_light_state_t *s);
void ru_light_set_feedback(lv_obj_t *w, ru_light_fb_t st, const char *text);
/* not controllable / protected / stale: rings cannot be touched, knobs lose their border, chips are hidden, the caption shows text in warn */
void ru_light_set_readonly(lv_obj_t *w, bool ro, const char *text);
void ru_light_on_change(lv_obj_t *w, ru_light_cb_t cb, void *ud);
/* true when a press at (x, y) (screen px) would start a ring drag: the touch bands of the rings, inside the sweep +- 8 degrees */
bool ru_light_ring_at(const lv_obj_t *w, int x, int y);
/* test and layout access: 0 root, 1 brightness arc, 2 colour arc, 3 disc, 4 large text, 5 caption, 6 touch layer, 7 chip pill, 8 power glyph */
lv_obj_t *ru_light_part(const lv_obj_t *w, int which);
/* what the colour arc shows now: RU_RING_HUE, RU_RING_TEMP, or -1 when it is hidden or absent; and the shown ring on a round panel */
int ru_light_view(const lv_obj_t *w);
int ru_light_chips(const lv_obj_t *w);   /* number of chips */
void ru_light_press_chip(lv_obj_t *w, int i); /* as a tap on chip i (tests) */
