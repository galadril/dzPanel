/* LVGL drawing for climate pages (see ru_climate.h). Plain values in, objects out: no Domoticz types here. */
#pragma once
#include "lvgl.h"
#include "ru_climate.h"
#include "ru_data.h"
#include "ru_widgets.h"

typedef struct {
  const ru_cl_view_t *v;
  ru_val_state_t st;       /* freshness of the device: STALE dims the numbers */
  const char *stale_text;  /* "UPDATED 3 H AGO" shown when st is STALE, else NULL */
  int k, n;                /* page k of n for the "2/5" counter; n <= 1 hides it */
  bool no_name;            /* the caller draws the name itself (as a back control) */
} ru_cl_page_ctx_t;

/* Draws one device page onto scr (name, hero or compass, lines, status line). Everything stays inside the circle. */
void ru_climate_draw_page(lv_obj_t *scr, const ru_cl_page_ctx_t *c);
/* An invisible tap zone over the hero, for "next page". */
lv_obj_t *ru_climate_tap_zone(lv_obj_t *scr, lv_event_cb_t cb, void *ud);

/* An invisible tap zone over the name and the "2/5" counter at the top: always "next page". */
lv_obj_t *ru_climate_next_zone(lv_obj_t *scr, lv_event_cb_t cb, void *ud);

typedef struct { char name[24]; char value[28]; bool known; ru_val_state_t st; } ru_cl_item_t;
/* Compact overview: one row per sensor (name + value), scrollable. The caller registers taps on tiles_out[i]. */
lv_obj_t *ru_climate_overview(lv_obj_t *scr, const char *title, const ru_cl_item_t *items, int n, lv_obj_t **tiles_out,
                              int tiles_cap);
