#include "mini_test.h"
#include "ru_nav.h"

static ru_nav_result_t go(ru_nav_t *n, ru_nav_in_t t, int app, int detail, int step) {
  ru_nav_input_t in = { t, (uint16_t)app, (uint16_t)detail, (int16_t)step }; ru_nav_result_t r; ru_nav_handle(n, &in, &r); return r;
}

int main(void) {
  ru_nav_t n; ru_nav_init(&n, 3, 1, false);
  ru_nav_result_t r;
  CHECK(ru_nav_level(&n) == RU_NAV_AT_HOME && ru_nav_page(&n) == 1);
  r = go(&n, RU_NAV_IN_BACK, 0, 0, 0); CHECK(r.rejected && r.n == 0); /* back at root */
  r = go(&n, RU_NAV_IN_PAGE_STEP, 0, 0, 1); CHECK(r.n == 1 && r.a[0].act == RU_NAV_ACT_PAGER_TO && r.a[0].page == 2);
  r = go(&n, RU_NAV_IN_PAGE_STEP, 0, 0, 1); CHECK(r.rejected); /* clamped */
  r = go(&n, RU_NAV_IN_PAGE_SET, 0, 0, 0); CHECK(ru_nav_page(&n) == 0 && r.a[0].anim == RU_NAV_ANIM_RIGHT);
  /* home -> app -> detail -> back -> back */
  r = go(&n, RU_NAV_IN_OPEN_APP, 7, 0, 0); CHECK(r.a[0].act == RU_NAV_ACT_SHOW_APP && r.a[0].app == 7 && ru_nav_level(&n) == RU_NAV_AT_APP);
  r = go(&n, RU_NAV_IN_PUSH_DETAIL, 0, 99, 0); CHECK(r.a[0].act == RU_NAV_ACT_SHOW_DETAIL && r.a[0].detail == 99 && r.a[0].app == 7);
  CHECK(ru_nav_depth(&n) == 2 && ru_nav_level(&n) == RU_NAV_AT_DETAIL);
  r = go(&n, RU_NAV_IN_PAGE_STEP, 0, 0, 1); CHECK(r.rejected); /* pager only on home */
  r = go(&n, RU_NAV_IN_BACK, 0, 0, 0); CHECK(r.a[0].act == RU_NAV_ACT_SHOW_APP && r.a[0].anim == RU_NAV_ANIM_POP);
  r = go(&n, RU_NAV_IN_BACK, 0, 0, 0); CHECK(r.a[0].act == RU_NAV_ACT_SHOW_HOME && r.a[0].page == 0);
  /* home action: launcher on home, closes it, goes home from an app */
  r = go(&n, RU_NAV_IN_HOME_ACTION, 0, 0, 0); CHECK(r.a[0].act == RU_NAV_ACT_SHOW_LAUNCHER && ru_nav_overlay(&n) == RU_NAV_OVL_LAUNCHER);
  r = go(&n, RU_NAV_IN_OPEN_APP, 3, 0, 0); CHECK(r.n == 2 && r.a[0].act == RU_NAV_ACT_HIDE_OVERLAY && r.a[1].act == RU_NAV_ACT_SHOW_APP);
  r = go(&n, RU_NAV_IN_HOME_ACTION, 0, 0, 0); CHECK(r.a[0].act == RU_NAV_ACT_SHOW_HOME && ru_nav_depth(&n) == 0);
  /* quick controls: from anywhere, back closes them first */
  go(&n, RU_NAV_IN_OPEN_APP, 3, 0, 0);
  r = go(&n, RU_NAV_IN_QUICK, 0, 0, 0); CHECK(r.a[0].act == RU_NAV_ACT_SHOW_QUICK);
  r = go(&n, RU_NAV_IN_BACK, 0, 0, 0); CHECK(r.a[0].act == RU_NAV_ACT_HIDE_OVERLAY && ru_nav_depth(&n) == 1);
  r = go(&n, RU_NAV_IN_QUICK, 0, 0, 0); r = go(&n, RU_NAV_IN_QUICK, 0, 0, 0); CHECK(r.a[0].act == RU_NAV_ACT_HIDE_OVERLAY);
  /* go-home (rim long press) from deep + overlay: one tidy result; no-op when already home */
  go(&n, RU_NAV_IN_PUSH_DETAIL, 0, 1, 0); go(&n, RU_NAV_IN_QUICK, 0, 0, 0);
  r = go(&n, RU_NAV_IN_GO_HOME, 0, 0, 0); CHECK(r.n == 2 && ru_nav_depth(&n) == 0 && ru_nav_overlay(&n) == RU_NAV_OVL_NONE);
  r = go(&n, RU_NAV_IN_GO_HOME, 0, 0, 0); CHECK(r.n == 0);
  /* re-opening an app in the stack pops back to it; depth is bounded */
  go(&n, RU_NAV_IN_OPEN_APP, 1, 0, 0); go(&n, RU_NAV_IN_OPEN_APP, 2, 0, 0); go(&n, RU_NAV_IN_OPEN_APP, 3, 0, 0);
  r = go(&n, RU_NAV_IN_OPEN_APP, 1, 0, 0); CHECK(ru_nav_depth(&n) == 1 && r.a[0].anim == RU_NAV_ANIM_POP);
  for (int i = 0; i < 20; i++) go(&n, RU_NAV_IN_PUSH_DETAIL, 0, (int)i, 0);
  CHECK(ru_nav_depth(&n) == RU_NAV_MAX_DEPTH);
  r = go(&n, RU_NAV_IN_PUSH_DETAIL, 0, 5, 0); CHECK(r.rejected);
  go(&n, RU_NAV_IN_GO_HOME, 0, 0, 0);
  /* wrap pager */
  ru_nav_init(&n, 3, 0, true);
  r = go(&n, RU_NAV_IN_PAGE_STEP, 0, 0, -1); CHECK(ru_nav_page(&n) == 2);
  r = go(&n, RU_NAV_IN_PAGE_STEP, 0, 0, 1); CHECK(ru_nav_page(&n) == 0);
  /* gesture mapping */
  ru_gesture_event_t g = { .type = RU_G_EDGE_SWIPE, .zone = RU_ZONE_RIM, .action = RU_ACT_BACK };
  ru_nav_input_t in;
  CHECK(ru_nav_input_from_gesture(&g, &in) && in.type == RU_NAV_IN_BACK);
  g.action = RU_ACT_QUICK; CHECK(ru_nav_input_from_gesture(&g, &in) && in.type == RU_NAV_IN_QUICK);
  g.action = RU_ACT_HOME; CHECK(ru_nav_input_from_gesture(&g, &in) && in.type == RU_NAV_IN_HOME_ACTION);
  g.action = RU_ACT_CONTEXT; CHECK(!ru_nav_input_from_gesture(&g, &in));
  g = (ru_gesture_event_t){ .type = RU_G_LONG_PRESS, .zone = RU_ZONE_RIM }; CHECK(ru_nav_input_from_gesture(&g, &in) && in.type == RU_NAV_IN_GO_HOME);
  g = (ru_gesture_event_t){ .type = RU_G_LONG_PRESS, .zone = RU_ZONE_CONTENT }; CHECK(!ru_nav_input_from_gesture(&g, &in));
  g = (ru_gesture_event_t){ .type = RU_G_SWIPE, .zone = RU_ZONE_CONTENT, .dir = RU_DIR_LEFT };
  CHECK(ru_nav_pager_swipe(&n, &g, &in) && in.n == 1);
  go(&n, RU_NAV_IN_OPEN_APP, 1, 0, 0); CHECK(!ru_nav_pager_swipe(&n, &g, &in));
  MT_DONE();
}
