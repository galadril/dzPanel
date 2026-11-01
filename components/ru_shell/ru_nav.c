#include "ru_nav.h"
#include <string.h>

void ru_nav_init(ru_nav_t *n, int n_pages, int start_page, bool wrap) {
  memset(n, 0, sizeof *n);
  if (n_pages < 1) n_pages = 1;
  if (n_pages > RU_NAV_MAX_PAGES) n_pages = RU_NAV_MAX_PAGES;
  n->n_pages = n_pages;
  n->page = start_page < 0 ? 0 : start_page >= n_pages ? n_pages - 1 : start_page;
  n->wrap = wrap;
}

ru_nav_level_t ru_nav_level(const ru_nav_t *n) {
  return n->depth == 0 ? RU_NAV_AT_HOME : (n->stack[n->depth - 1].is_detail ? RU_NAV_AT_DETAIL : RU_NAV_AT_APP);
}
ru_nav_overlay_t ru_nav_overlay(const ru_nav_t *n) { return n->overlay; }
int ru_nav_page(const ru_nav_t *n) { return n->page; }
int ru_nav_depth(const ru_nav_t *n) { return n->depth; }
bool ru_nav_current_app(const ru_nav_t *n, uint16_t *app) {
  if (!n->depth) return false;
  if (app) *app = n->stack[n->depth - 1].app;
  return true;
}

static void add(ru_nav_result_t *r, ru_nav_act_t a, uint16_t app, uint16_t detail, int page, ru_nav_anim_t an) {
  if (r->n >= RU_NAV_MAX_ACTIONS) return;
  r->a[r->n++] = (ru_nav_action_t){ a, app, detail, (int16_t)page, an };
}

/* show whatever is on top of the stack */
static void show_top(ru_nav_t *n, ru_nav_result_t *r, ru_nav_anim_t an) {
  if (n->depth == 0) { add(r, RU_NAV_ACT_SHOW_HOME, 0, 0, n->page, an); return; }
  ru_nav_entry_t *e = &n->stack[n->depth - 1];
  add(r, e->is_detail ? RU_NAV_ACT_SHOW_DETAIL : RU_NAV_ACT_SHOW_APP, e->app, e->detail, 0, an);
}

static void close_overlay(ru_nav_t *n, ru_nav_result_t *r) {
  if (n->overlay != RU_NAV_OVL_NONE) { n->overlay = RU_NAV_OVL_NONE; add(r, RU_NAV_ACT_HIDE_OVERLAY, 0, 0, 0, RU_NAV_ANIM_FADE); }
}

static void page_to(ru_nav_t *n, int p, ru_nav_result_t *r, ru_nav_anim_t an) {
  n->page = p; add(r, RU_NAV_ACT_PAGER_TO, 0, 0, p, an);
}

void ru_nav_handle(ru_nav_t *n, const ru_nav_input_t *in, ru_nav_result_t *r) {
  memset(r, 0, sizeof *r);
  switch (in->type) {
  case RU_NAV_IN_OPEN_APP: {
    close_overlay(n, r);
    /* opening an app already in the stack pops back to it (no cycles) */
    for (int i = 0; i < n->depth; i++)
      if (n->stack[i].app == in->app && !n->stack[i].is_detail) {
        n->depth = i + 1; show_top(n, r, RU_NAV_ANIM_POP); return;
      }
    /* from home/launcher the stack is empty so this is the first entry; from an app it is pushed */
    if (n->depth >= RU_NAV_MAX_DEPTH) { r->rejected = true; return; }
    n->stack[n->depth++] = (ru_nav_entry_t){ in->app, 0, false };
    add(r, RU_NAV_ACT_SHOW_APP, in->app, 0, 0, RU_NAV_ANIM_PUSH);
    return;
  }
  case RU_NAV_IN_PUSH_DETAIL:
    if (n->depth == 0 || n->depth >= RU_NAV_MAX_DEPTH) { r->rejected = true; return; }
    close_overlay(n, r);
    n->stack[n->depth] = (ru_nav_entry_t){ n->stack[n->depth - 1].app, in->detail, true };
    n->depth++;
    add(r, RU_NAV_ACT_SHOW_DETAIL, n->stack[n->depth - 1].app, in->detail, 0, RU_NAV_ANIM_PUSH);
    return;
  case RU_NAV_IN_BACK:
    if (n->overlay != RU_NAV_OVL_NONE) { close_overlay(n, r); return; }
    if (n->depth == 0) { r->rejected = true; return; } /* root: nothing to go back to */
    n->depth--;
    show_top(n, r, RU_NAV_ANIM_POP);
    return;
  case RU_NAV_IN_HOME_ACTION:
    if (n->overlay == RU_NAV_OVL_QUICK) { close_overlay(n, r); return; }
    if (n->overlay == RU_NAV_OVL_LAUNCHER) { close_overlay(n, r); return; }
    if (n->depth == 0) { n->overlay = RU_NAV_OVL_LAUNCHER; add(r, RU_NAV_ACT_SHOW_LAUNCHER, 0, 0, 0, RU_NAV_ANIM_UP); return; }
    n->depth = 0;
    add(r, RU_NAV_ACT_SHOW_HOME, 0, 0, n->page, RU_NAV_ANIM_FADE);
    return;
  case RU_NAV_IN_GO_HOME: {
    bool was_home = n->depth == 0;
    n->depth = 0;
    if (n->overlay != RU_NAV_OVL_NONE) close_overlay(n, r);
    if (!was_home) add(r, RU_NAV_ACT_SHOW_HOME, 0, 0, n->page, RU_NAV_ANIM_FADE);
    return;
  }
  case RU_NAV_IN_LAUNCHER:
    if (n->overlay == RU_NAV_OVL_LAUNCHER) { close_overlay(n, r); return; }
    if (n->depth != 0) { n->depth = 0; close_overlay(n, r); add(r, RU_NAV_ACT_SHOW_HOME, 0, 0, n->page, RU_NAV_ANIM_FADE); }
    n->overlay = RU_NAV_OVL_LAUNCHER; add(r, RU_NAV_ACT_SHOW_LAUNCHER, 0, 0, 0, RU_NAV_ANIM_UP);
    return;
  case RU_NAV_IN_QUICK:
    if (n->overlay == RU_NAV_OVL_QUICK) { close_overlay(n, r); return; }
    close_overlay(n, r);
    n->overlay = RU_NAV_OVL_QUICK; add(r, RU_NAV_ACT_SHOW_QUICK, 0, 0, 0, RU_NAV_ANIM_DOWN);
    return;
  case RU_NAV_IN_PAGE_STEP:
  case RU_NAV_IN_PAGE_SET: {
    if (n->depth != 0 || n->overlay != RU_NAV_OVL_NONE) { r->rejected = true; return; } /* pager lives on home only */
    int p = in->type == RU_NAV_IN_PAGE_SET ? in->n : n->page + in->n;
    if (n->wrap && in->type == RU_NAV_IN_PAGE_STEP) p = ((p % n->n_pages) + n->n_pages) % n->n_pages;
    if (p < 0 || p >= n->n_pages) { r->rejected = true; return; }
    if (p == n->page) return;
    ru_nav_anim_t an = p > n->page ? RU_NAV_ANIM_LEFT : RU_NAV_ANIM_RIGHT;
    if (n->wrap && in->type == RU_NAV_IN_PAGE_STEP) an = in->n > 0 ? RU_NAV_ANIM_LEFT : RU_NAV_ANIM_RIGHT;
    page_to(n, p, r, an);
    return;
  }
  default: r->rejected = true; return;
  }
}

bool ru_nav_input_from_gesture(const ru_gesture_event_t *g, ru_nav_input_t *in) {
  memset(in, 0, sizeof *in);
  if (g->type == RU_G_EDGE_SWIPE) {
    switch (g->action) {
    case RU_ACT_BACK: in->type = RU_NAV_IN_BACK; return true;
    case RU_ACT_QUICK: in->type = RU_NAV_IN_QUICK; return true;
    case RU_ACT_HOME: in->type = RU_NAV_IN_HOME_ACTION; return true;
    default: return false; /* CONTEXT: per-app, not navigation */
    }
  }
  if (g->type == RU_G_LONG_PRESS && g->zone == RU_ZONE_RIM) { in->type = RU_NAV_IN_GO_HOME; return true; }
  return false;
}

bool ru_nav_pager_swipe(const ru_nav_t *n, const ru_gesture_event_t *g, ru_nav_input_t *in) {
  memset(in, 0, sizeof *in);
  if (g->type != RU_G_SWIPE || g->zone != RU_ZONE_CONTENT || n->depth != 0 || n->overlay != RU_NAV_OVL_NONE) return false;
  if (g->dir != RU_DIR_LEFT && g->dir != RU_DIR_RIGHT) return false;
  in->type = RU_NAV_IN_PAGE_STEP; in->n = g->dir == RU_DIR_LEFT ? 1 : -1;
  return true;
}
