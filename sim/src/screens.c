#include "screens.h"
#include "ru_widgets.h"
#include "ru_splash.h"
#include <stdio.h>
#include <string.h>

const char *const sim_screen_names[] = { "home", "dimmer", "selector", "onoff", "stale", "hold", "thermostat", "blinds", "scenes", "splash", "splash_early", "splash_error" };
const int sim_screen_count = 12;

/* label centred at (dx,dy) units from the screen centre */
static lv_obj_t *label_u(lv_obj_t *scr, const char *txt, const lv_font_t *f, lv_color_t c, float dx_u, float dy_u) {
  int D = ru_theme_geom()->diameter;
  return ru_label_new(scr, txt, f, c, D / 2 + ru_u(dx_u), D / 2 + ru_u(dy_u));
}

static void centre(lv_obj_t *o, float dx_u, float dy_u) { lv_obj_align(o, LV_ALIGN_CENTER, ru_u(dx_u), ru_u(dy_u)); }

static void on_toggle(lv_obj_t *w, bool on, void *ud) {
  ru_toggle_set(w, on ? RU_ONOFF_ON : RU_ONOFF_OFF);
  lv_label_set_text((lv_obj_t *)ud, on ? "TAP TO SWITCH OFF" : "TAP TO SWITCH ON");
}
static void on_hold(lv_obj_t *w, ru_hold_event_t ev, void *ud) {
  (void)w;
  lv_label_set_text((lv_obj_t *)ud, ev == RU_HOLD_CONFIRMED ? "CONFIRMED" : "SWIPE DOWN TO CANCEL");
}

static lv_obj_t *scr_home(uint32_t now) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *s = ru_screen_new();
  ru_status_rim_create(s);
  label_u(s, "10:42", ru_font_hero(), t->text, 0, -24);
  label_u(s, "THU 31 OCT", ru_font_title(), t->text_dim, 0, -8);
  int d = ru_u(20);
  struct { const char *cap; float v, lo, hi; ru_unit_t u; lv_color_t c; float x; } g[3] = {
    { "INSIDE", 18, 0, 40, RU_UNIT_C, t->info, -25 },
    { "KW NOW", 2.1f, 0, 6, RU_UNIT_NONE, t->accent, 0 },
    { "BATT", 78, 0, 100, RU_UNIT_PCT, t->ok, 25 },
  };
  for (int i = 0; i < 3; i++) {
    lv_obj_t *r = ru_ring_gauge_create(s, d, g[i].cap);
    centre(r, g[i].x, 20);
    ru_ring_gauge_set_color(r, g[i].c);
    ru_val_t v = ru_val_make(g[i].v, g[i].u, now, 60000);
    ru_ring_gauge_set(r, &v, g[i].lo, g[i].hi, now);
  }
  for (int i = 0; i < 4; i++) {
    lv_obj_t *dot = ru_icon_dot(s, ru_u(1.3f), i == 0 ? t->accent : ru_dim(t->text_dim, 120));
    centre(dot, (float)(i - 1) * 3.4f - 1.7f, 38.5f);
  }
  return s;
}

static lv_obj_t *scr_dimmer(uint32_t now) {
  (void)now;
  const ru_theme_t *t = ru_theme();
  lv_obj_t *s = ru_screen_new();
  ru_status_rim_create(s);
  lv_obj_t *d = ru_dimmer_create(s, ru_u(86), 100, "DIMMER");
  ru_dimmer_set(d, 60);
  label_u(s, "LIVING ROOM", ru_font_body(), t->text_dim, 0, 40);
  return s;
}

static lv_obj_t *scr_selector(uint32_t now) {
  (void)now;
  const ru_theme_t *t = ru_theme();
  static const char *const names[] = { "OFF", "LOW", "MED", "HIGH" };
  lv_obj_t *s = ru_screen_new();
  ru_status_rim_create(s);
  ru_status_rim_set_color(lv_obj_get_child(s, 0), ru_dim(t->info, 90));
  lv_obj_t *w = ru_selector_create(s, ru_u(86), names, 4);
  ru_selector_set(w, 2);
  lv_obj_t *c = ru_selector_centre(w);
  lv_obj_t *fan = ru_icon_fan(c, ru_u(17), t->info);
  lv_obj_align(fan, LV_ALIGN_CENTER, 0, 0);
  label_u(s, "BEDROOM FAN", ru_font_body(), t->text_dim, 0, 40);
  return s;
}

static lv_obj_t *scr_onoff(uint32_t now) {
  (void)now;
  const ru_theme_t *t = ru_theme();
  lv_obj_t *s = ru_screen_new();
  ru_status_rim_create(s);
  label_u(s, "HALL LAMP", ru_font_title(), t->text_dim, 0, -40);
  lv_obj_t *hint = label_u(s, "TAP TO SWITCH OFF", ru_font_body(), t->text_dim, 0, 39);
  lv_obj_t *w = ru_toggle_create(s, ru_u(54));
  ru_toggle_set(w, RU_ONOFF_ON);
  ru_toggle_on_change(w, on_toggle, hint);
  return s;
}

static lv_obj_t *scr_stale(uint32_t now) {
  const ru_theme_t *t = ru_theme();
  const ru_geom_t *g = ru_theme_geom();
  lv_obj_t *s = ru_screen_new();
  ru_status_rim_create(s);
  ru_status_rim_set_state(lv_obj_get_child(s, 0), RU_RIM_STALE);
  label_u(s, "LIVING ROOM", ru_font_title(), t->text_dim, 0, -40);
  /* now is the demo clock; values were captured at different ages */
  uint32_t base = now > 400000 ? now : 400000;
  struct { const char *l; ru_val_t v; const char *txt; float cy; float wf; } rows[4];
  rows[0].l = "TEMP"; rows[0].v = ru_val_make(21.4f, RU_UNIT_C, base - 5000, 60000); rows[0].txt = NULL; rows[0].cy = -22;
  rows[1].l = "HUMIDITY"; rows[1].v = ru_val_make(48, RU_UNIT_PCT, base - 240000, 60000); rows[1].txt = NULL; rows[1].cy = -7;
  rows[2].l = "GARAGE"; rows[2].v = ru_val_unknown(); rows[2].txt = NULL; rows[2].cy = 8;
  rows[3].l = "HALL LAMP"; rows[3].v = ru_val_make(0, RU_UNIT_NONE, base - 5000, 60000); rows[3].txt = "OFF"; rows[3].cy = 23;
  float rh = 12.5f;
  for (int i = 0; i < 4; i++) {
    float top = (float)g->diameter / 2 + rows[i].cy * g->u - rh * g->u / 2;
    float w = ru_geom_row_width(g, top, top + rh * g->u) - (float)ru_u(2);
    lv_obj_t *tile = ru_value_tile_create(s, (int)w, ru_u(rh), rows[i].l);
    centre(tile, 0, rows[i].cy);
    if (rows[i].txt) {
      ru_value_tile_set_color(tile, t->text_dim);
      ru_value_tile_set_text(tile, rows[i].txt, RU_VAL_OK);
    } else ru_value_tile_set(tile, &rows[i].v, base);
  }
  char buf[48], age[24];
  ru_age_format(240000, age, sizeof age);
  snprintf(buf, sizeof buf, "UPDATED %s", age);
  label_u(s, buf, ru_font_body(), t->warn, 0, 39);
  return s;
}

static lv_obj_t *scr_hold(uint32_t now) {
  (void)now;
  const ru_theme_t *t = ru_theme();
  lv_obj_t *s = ru_screen_new();
  ru_status_rim_create(s);
  label_u(s, "GARAGE DOOR", ru_font_title(), t->text_dim, 0, -29);
  label_u(s, "OPEN?", ru_font_hero(), t->text, 0, -19);
  lv_obj_t *hint = label_u(s, "SWIPE DOWN TO CANCEL", ru_font_small(), t->text_dim, 0, 39);
  lv_obj_t *h = ru_hold_confirm_create(s, ru_u(38), "HOLD", 1200);
  centre(h, 0, 13);
  ru_hold_confirm_set_progress(h, 750);
  ru_hold_confirm_on_event(h, on_hold, hint);
  return s;
}

static lv_obj_t *scr_thermo(uint32_t now) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *s = ru_screen_new();
  ru_status_rim_create(s);
  lv_obj_t *w = ru_thermostat_create(s, ru_u(86), 5, 30, 0.5f);
  ru_thermostat_set_setpoint(w, 21.5f);
  ru_val_t cur = ru_val_make(19.8f, RU_UNIT_C, now, 60000);
  ru_thermostat_set_current(w, &cur, now);
  ru_thermostat_set_mode(w, "HEATING", t->accent);
  label_u(s, "THERMOSTAT", ru_font_body(), t->text_dim, 0, 40);
  return s;
}

static lv_obj_t *scr_blinds(uint32_t now) {
  const ru_theme_t *t = ru_theme();
  lv_obj_t *s = ru_screen_new();
  ru_status_rim_create(s);
  label_u(s, "BEDROOM BLINDS", ru_font_body(), t->text_dim, 0, -40);
  lv_obj_t *b = ru_blinds_create(s, ru_u(60), ru_u(76));
  centre(b, 0, 5.2f);
  ru_val_t v = ru_val_make(65, RU_UNIT_PCT, now, 600000);
  ru_blinds_set(b, &v, now);
  return s;
}

/* Scene = push only (ring with a core, "SCENE", no on/off); Group = toggles: On filled, Off hollow, MIXED half filled. */
static lv_obj_t *scr_scenes(uint32_t now) {
  (void)now;
  const ru_theme_t *t = ru_theme();
  lv_obj_t *s = ru_screen_new();
  ru_status_rim_create(s);
  label_u(s, "SCENES", ru_font_title(), t->text_dim, 0, -40);
  struct { const char *n; ru_dot_state_t d; const char *sub; float x, y; } b[4] = {
    { "MOVIE", RU_DOT_PUSH, "SCENE", -14, -13 }, { "NIGHT", RU_DOT_PUSH, "ACTIVATED", 14, -13 },
    { "AWAY", RU_DOT_ON, "ON", -14, 12 }, { "ALL OFF", RU_DOT_MIXED, "MIXED", 14, 12 } };
  for (int i = 0; i < 4; i++) {
    lv_obj_t *o = ru_state_tile_create(s, ru_u(25), ru_u(21), b[i].n);
    ru_state_tile_set(o, b[i].d, b[i].sub, RU_VAL_OK);
    centre(o, b[i].x, b[i].y);
  }
  return s;
}

lv_obj_t *sim_screen_build(const char *name, uint32_t now) {
  if (!strcmp(name, "home")) return scr_home(now);
  if (!strcmp(name, "dimmer")) return scr_dimmer(now);
  if (!strcmp(name, "selector")) return scr_selector(now);
  if (!strcmp(name, "onoff")) return scr_onoff(now);
  if (!strcmp(name, "stale")) return scr_stale(now);
  if (!strcmp(name, "hold")) return scr_hold(now);
  if (!strcmp(name, "thermostat")) return scr_thermo(now);
  if (!strcmp(name, "blinds")) return scr_blinds(now);
  if (!strcmp(name, "scenes")) return scr_scenes(now);
  /* boot splash: deterministic frames (not animated): ring complete, ring half drawn, and the boot-error variant */
  if (!strncmp(name, "splash", 6)) {
    ru_splash_cfg_t cfg = { "RoundOS", "for Domoticz", "v0.1.0" };
    lv_obj_t *s = ru_splash_create(&cfg);
    if (!s) return NULL;
    if (!strcmp(name, "splash")) ru_splash_set_progress(s, 1000);
    else if (!strcmp(name, "splash_early")) ru_splash_set_progress(s, 400);
    else if (!strcmp(name, "splash_error")) ru_splash_set_error(s, "E12 DISPLAY", "HOLD TO RECOVER");
    else { lv_obj_delete(s); return NULL; }
    return s;
  }
  return NULL;
}
