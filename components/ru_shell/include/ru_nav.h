/* ru_nav: navigation model as a pure state machine. No UI, no allocation.
 * Feed inputs, get back actions for the UI to execute (show screen, play transition).
 *
 *   HOME level (rim pager over favourite pages) -> APP -> DETAIL ...
 *   Overlays: LAUNCHER (from home), QUICK controls (from anywhere), above the current screen.
 * Back stack holds entries below HOME; HOME itself is the root and never in the stack.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ru_gesture.h"

#define RU_NAV_MAX_DEPTH 8
#define RU_NAV_MAX_PAGES 8
#define RU_NAV_MAX_ACTIONS 3

typedef enum { RU_NAV_IN_NONE = 0, RU_NAV_IN_OPEN_APP, RU_NAV_IN_PUSH_DETAIL, RU_NAV_IN_BACK, RU_NAV_IN_HOME_ACTION,
               RU_NAV_IN_QUICK, RU_NAV_IN_PAGE_STEP, RU_NAV_IN_PAGE_SET, RU_NAV_IN_LAUNCHER, RU_NAV_IN_GO_HOME } ru_nav_in_t;

typedef struct { ru_nav_in_t type; uint16_t app; uint16_t detail; int16_t n; } ru_nav_input_t;
/* OPEN_APP: app. PUSH_DETAIL: detail (within the current app). PAGE_STEP: n = +/-1. PAGE_SET: n = index.
 * GO_HOME: rim long-press; always ends on home, no overlay (no-op if already there).
 * HOME_ACTION: bottom-edge swipe / home pill: launcher when on home, else go home. */

typedef enum { RU_NAV_ACT_NONE = 0, RU_NAV_ACT_SHOW_HOME, RU_NAV_ACT_SHOW_APP, RU_NAV_ACT_SHOW_DETAIL,
               RU_NAV_ACT_SHOW_LAUNCHER, RU_NAV_ACT_SHOW_QUICK, RU_NAV_ACT_HIDE_OVERLAY,
               RU_NAV_ACT_PAGER_TO } ru_nav_act_t;
typedef enum { RU_NAV_ANIM_NONE = 0, RU_NAV_ANIM_PUSH, RU_NAV_ANIM_POP, RU_NAV_ANIM_UP, RU_NAV_ANIM_DOWN,
               RU_NAV_ANIM_LEFT, RU_NAV_ANIM_RIGHT, RU_NAV_ANIM_FADE } ru_nav_anim_t;

typedef struct { ru_nav_act_t act; uint16_t app, detail; int16_t page; ru_nav_anim_t anim; } ru_nav_action_t;
typedef struct { ru_nav_action_t a[RU_NAV_MAX_ACTIONS]; int n; bool rejected; } ru_nav_result_t;

typedef enum { RU_NAV_AT_HOME = 0, RU_NAV_AT_APP, RU_NAV_AT_DETAIL } ru_nav_level_t;
typedef enum { RU_NAV_OVL_NONE = 0, RU_NAV_OVL_LAUNCHER, RU_NAV_OVL_QUICK } ru_nav_overlay_t;

typedef struct { uint16_t app, detail; bool is_detail; } ru_nav_entry_t;

typedef struct {
  ru_nav_entry_t stack[RU_NAV_MAX_DEPTH];
  int depth;
  ru_nav_overlay_t overlay;
  int page, n_pages;
  bool wrap;
} ru_nav_t;

void ru_nav_init(ru_nav_t *n, int n_pages, int start_page, bool wrap);
void ru_nav_handle(ru_nav_t *n, const ru_nav_input_t *in, ru_nav_result_t *out);

ru_nav_level_t ru_nav_level(const ru_nav_t *n);
ru_nav_overlay_t ru_nav_overlay(const ru_nav_t *n);
int ru_nav_page(const ru_nav_t *n);
int ru_nav_depth(const ru_nav_t *n);
bool ru_nav_current_app(const ru_nav_t *n, uint16_t *app); /* false on home */

/* Map a shell gesture to a nav input. Returns false if the shell does not handle it.
 * EDGE_SWIPE (BACK/QUICK/HOME), rim LONG_PRESS (home), rim crown drag steps (via ru_nav_crown). */
bool ru_nav_input_from_gesture(const ru_gesture_event_t *g, ru_nav_input_t *in);
/* Horizontal content SWIPE on the home level pages the pager (left = next). */
bool ru_nav_pager_swipe(const ru_nav_t *n, const ru_gesture_event_t *g, ru_nav_input_t *in);
