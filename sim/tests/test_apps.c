/* The Domoticz apps on fixture data: every screen at 466 and 480 stays inside the circle and within the lit-pixel
 * budget; behaviour of Scene (push) versus Group (toggle, Mixed), command feedback, honest unknown/stale/offline.
 * No network: a fixture-backed model and a command spy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "apps_demo.h"
#include "mini_test.h"
#include "ru_apps.h"

#ifndef SHOT_DIR
#define SHOT_DIR "."
#endif
#ifndef FIXTURE_DIR
#define FIXTURE_DIR "../tests/fixtures"
#endif

static const char *const TOKEN = "fixture-token-not-a-secret";

/* ---- helpers ---- */

static bool g_square; /* current run: square panel (full-panel PNGs, *_480sq.png) */
static bool init_sim(sim_t *s, int size) { return sim_init_ex(s, size, g_square); }
static const char *sfx(void) { return g_square ? "sq" : ""; }

static long count_bright(const sim_t *s) {
  long n = 0;
  for (int i = 0; i < s->size * s->size; i++) {
    uint32_t p = s->fb[i];
    if (((p >> 16) & 0xFF) >= 0x40 || ((p >> 8) & 0xFF) >= 0x40 || (p & 0xFF) >= 0x40) n++;
  }
  return n;
}

/* lit pixels outside the circle. Round: always zero tolerance. Square: only the declared corner back rectangle is exempt. */
static long outside_lit(const sim_t *s, ru_dzapp_t *a) {
  lv_area_t r;
  bool has = ru_dzapp_back_rect(a, &r);
  CHECK(!has || g_square); /* a round panel never declares an exception */
  return has ? sim_count_outside_lit_except(s, &r) : sim_count_outside_lit(s);
}

static bool tree_has_text(lv_obj_t *o, const char *needle) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
  if (lv_obj_check_type(o, &lv_label_class)) {
    const char *t = lv_label_get_text(o);
    if (t && strstr(t, needle)) return true;
  }
  uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; i++) if (tree_has_text(lv_obj_get_child(o, i), needle)) return true;
  return false;
}
static bool shows(ru_dzapp_t *a, const char *needle) { return tree_has_text(ru_dzapp_screen(a), needle); }

typedef struct { int n; dz_cmd_kind_t kind; char idx[12]; bool confirmed; int level; float sp; } spy_t;
static spy_t SPY;
static void spy_cb(const dz_cmd_req_t *r, void *ud) {
  (void)ud;
  SPY.n++;
  SPY.kind = r->kind;
  snprintf(SPY.idx, sizeof SPY.idx, "%s", r->idx);
  SPY.confirmed = r->confirmed;
  SPY.level = r->level;
  SPY.sp = r->setpoint;
}

static void hold_confirm(int size) {
  int c = size / 2, y = c + ru_u(13);
  sim_pointer(c, y, true);
  sim_advance(1600);
  sim_pointer(c, y, false);
  sim_advance(100);
}
static void click_at(int x, int y) {
  sim_pointer(x, y, true);
  sim_advance(80);
  sim_pointer(x, y, false);
  sim_advance(80);
}

static ru_dzapp_t *start(appfx_t *f, const char *dir, appfx_set_t set, uint32_t ttl, bool control) {
  CHECK(appfx_load(f, dir, set, ttl));
  ru_dzapp_t *a = ru_dzapp_create(&f->model, &f->client, control);
  CHECK(a != NULL);
  ru_dzapp_set_display_info(a, "sim", !g_square);
  ru_dzapp_set_home_section(a, RU_SEC_FAVS); /* these tests are about the Favourites grid; the Clock start page has its own tests */
  f->T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_dzapp_set_cmd_spy(a, spy_cb, NULL);
  memset(&SPY, 0, sizeof SPY);
  ru_dzapp_ui_tick(a, f->T);
  return a;
}
static void finish(appfx_t *f, ru_dzapp_t *a) {
  appfx_gestures_detach();
  ru_dzapp_destroy(a);
  appfx_unload(f);
}

/* the row of the given idx in the current section */
static int row_of(ru_dzapp_t *a, const char *idx) {
  ru_dzapp_row_info_t ri;
  for (size_t i = 0; ru_dzapp_row_info(a, i, &ri); i++) if (!strcmp(ri.idx, idx)) return (int)i;
  return -1;
}

static const char *SCENES_JSON =
  "{\"status\":\"OK\",\"result\":["
  "{\"idx\":\"1\",\"Name\":\"S1\",\"Type\":\"Scene\",\"Status\":\"Off\",\"Favorite\":1},"
  "{\"idx\":\"2\",\"Name\":\"S2\",\"Type\":\"Scene\",\"Status\":\"On\",\"Favorite\":1},"
  "{\"idx\":\"3\",\"Name\":\"G On\",\"Type\":\"Group\",\"Status\":\"On\",\"Favorite\":1},"
  "{\"idx\":\"4\",\"Name\":\"G Off\",\"Type\":\"Group\",\"Status\":\"Off\",\"Favorite\":1},"
  "{\"idx\":\"5\",\"Name\":\"G Mix\",\"Type\":\"Group\",\"Status\":\"Mixed\",\"Favorite\":1},"
  "{\"idx\":\"6\",\"Name\":\"G Unk\",\"Type\":\"Group\",\"Favorite\":1}]}";

/* ---- every screen, both sizes ---- */

static void all_screens(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  mkdir(SHOT_DIR, 0755);
  long area = (long)(3.14159f * (float)size * (float)size / 4);
  for (int i = 0; i < apps_demo_count; i++) {
    const char *nm = apps_demo_names[i];
    appfx_t f;
    ru_dzapp_t *a = apps_demo_show(&f, FIXTURE_DIR, nm);
    CHECK(a != NULL);
    if (!a) { appfx_unload(&f); continue; }
    sim_render();
    long out = outside_lit(&s, a), in = sim_count_lit(&s), br = count_bright(&s);
    printf("  %-20s@%d lit=%ld bright=%.1f%%\n", nm, size, in, 100.0 * (double)br / (double)area);
    {
      lv_area_t r;
      bool deep = ru_dzapp_view(a) != RU_VIEW_HOME;
      CHECK(ru_dzapp_back_rect(a, &r) == (g_square && deep));
      if (g_square && deep) CHECK(sim_count_lit_in(&s, &r) > 40); /* the corner chevron is drawn */
    }
    CHECK(out == 0);               /* nothing outside the circle (rim, tiles, labels) */
    CHECK(in > 300);               /* not blank */
    /* DESIGN.md: at most 12% lit. The colour and white-temperature light pages are a declared exception (25%): a hue wheel and its
     * glowing brightness ring are lit by definition. They are interactive pages, never an always-on face. */
    bool light_page = !strcmp(nm, "detail_ww") || !strncmp(nm, "detail_color", 12);
    CHECK(br < area * (light_page ? 25 : 12) / 100);
    CHECK(!shows(a, TOKEN));       /* the token is never on screen */
    char p[256];
    snprintf(p, sizeof p, "%s/app_%s_%d%s.png", SHOT_DIR, nm, size, sfx());
    CHECK(sim_save_png(&s, p, true));
    ru_dzapp_destroy(a);
    appfx_unload(&f);
  }
  sim_shutdown(&s);
}

/* ---- Scene is push only, Group toggles and can be Mixed ---- */

static void scene_and_group(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_MOCK, 600000, true);
  CHECK(appfx_apply_scenes(&f, SCENES_JSON));
  ru_dzapp_show_section(a, RU_SEC_SCENES);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_section(a) == RU_SEC_SCENES);
  CHECK(ru_dzapp_home_rows(a) == 6);

  /* indicators differ by shape: Scene = PUSH (no on/off), Group = ON / OFF / MIXED, no status = unknown */
  ru_dzapp_row_info_t ri;
  CHECK(ru_dzapp_row_info(a, (size_t)row_of(a, "1"), &ri) && ri.dot == RU_DOT_PUSH && !strcmp(ri.sub, "SCENE"));
  CHECK(ru_dzapp_row_info(a, (size_t)row_of(a, "2"), &ri) && ri.dot == RU_DOT_PUSH && !strcmp(ri.sub, "SCENE")); /* Status On: still no state shown */
  CHECK(ru_dzapp_row_info(a, (size_t)row_of(a, "3"), &ri) && ri.dot == RU_DOT_ON && !strcmp(ri.sub, "ON"));
  CHECK(ru_dzapp_row_info(a, (size_t)row_of(a, "4"), &ri) && ri.dot == RU_DOT_OFF && !strcmp(ri.sub, "OFF"));
  CHECK(ru_dzapp_row_info(a, (size_t)row_of(a, "5"), &ri) && ri.dot == RU_DOT_MIXED && !strcmp(ri.sub, "MIXED"));
  CHECK(ru_dzapp_row_info(a, (size_t)row_of(a, "6"), &ri) && ri.dot == RU_DOT_UNKNOWN && !strcmp(ri.sub, "--"));
  CHECK(shows(a, "MIXED") && shows(a, "SCENE"));
  sim_render();
  CHECK(sim_count_outside_lit(&s) == 0);

  struct { const char *idx; dz_cmd_kind_t want; const char *verb; } cases[] = {
    { "1", DZ_CMD_SCENE_ON, "ACTIVATE" },   /* Scene: always a push, whatever its Status says */
    { "2", DZ_CMD_SCENE_ON, "ACTIVATE" },
    { "3", DZ_CMD_SCENE_OFF, "SWITCH OFF" }, /* Group On  -> Off */
    { "4", DZ_CMD_SCENE_ON, "SWITCH ON" },   /* Group Off -> On */
    { "5", DZ_CMD_SCENE_ON, "SWITCH ON" },   /* Group Mixed -> On */
    { "6", DZ_CMD_SCENE_ON, "SWITCH ON" },   /* unknown state -> On, never a blind Off */
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    ru_dzapp_show_section(a, RU_SEC_SCENES);
    memset(&SPY, 0, sizeof SPY);
    int r = row_of(a, cases[i].idx);
    CHECK(r >= 0 && ru_dzapp_tap_row(a, (size_t)r));
    CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM);      /* both need a hold (dz_cmd_risk) */
    CHECK(SPY.n == 0);                               /* nothing sent by the tap itself */
    CHECK(ru_dzapp_confirm_kind(a) == cases[i].want);
    CHECK(!strcmp(ru_dzapp_confirm_verb(a), cases[i].verb));
    CHECK(shows(a, cases[i].verb));
    if (i == 4) CHECK(shows(a, "MIXED"));            /* the confirm screen says what the group is now */
    /* a short press does not send and does not leave the screen */
    click_at(size / 2, size / 2 + ru_u(13));
    CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM && SPY.n == 0);
    /* a full hold sends exactly the expected command, marked confirmed */
    hold_confirm(size);
    CHECK(SPY.n == 1 && SPY.kind == cases[i].want && !strcmp(SPY.idx, cases[i].idx) && SPY.confirmed);
    CHECK(SPY.kind != DZ_CMD_SCENE_OFF || !strcmp(cases[i].idx, "3")); /* Off only ever for a fully-On Group */
    CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_SCENES);
    CHECK(ru_dzapp_feedback(a)->st == RU_FB_PENDING && shows(a, "SENDING"));  /* pending, not success */
    CHECK(!shows(a, "DONE"));
  }

  /* the cancel button leaves without sending */
  ru_dzapp_show_section(a, RU_SEC_SCENES);
  memset(&SPY, 0, sizeof SPY);
  CHECK(ru_dzapp_tap_row(a, (size_t)row_of(a, "3")));
  click_at(size / 2, size / 2 + ru_u(42)); /* CANCEL */
  CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && SPY.n == 0);
  finish(&f, a);
  sim_shutdown(&s);
}

/* ---- command feedback: never success before CONFIRMED; unconfirmed is visible ---- */

static void feedback(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_MOCK, 600000, true);
  CHECK(appfx_apply_scenes(&f, SCENES_JSON));
  ru_dzapp_show_section(a, RU_SEC_SCENES);
  dz_cmd_result_t r;
  memset(&r, 0, sizeof r);
  strcpy(r.idx, "4");
  r.kind = DZ_CMD_SCENE_ON;

  /* a Mixed group after On: UNCONFIRMED, visibly, and never "DONE" */
  r.outcome = DZ_OUT_UNCONFIRMED;
  r.have_after = true;
  strcpy(r.after.status, "Mixed");
  r.after.cap = DZ_CAP_GROUP;
  ru_dzapp_inject_result(a, &r);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_feedback(a)->st == RU_FB_UNCONFIRMED);
  CHECK(shows(a, "STILL MIXED") && !shows(a, "DONE"));
  sim_render();
  sim_save_png(&s, SHOT_DIR "/app_fb_unconfirmed_check.png", true);

  r.outcome = DZ_OUT_CONFIRMED;
  ru_dzapp_inject_result(a, &r);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "DONE"));
  /* a Scene shows a short "ACTIVATED" on its tile while confirmed, and then nothing (it has no state) */
  strcpy(r.idx, "1");
  r.kind = DZ_CMD_SCENE_ON;
  ru_dzapp_inject_result(a, &r);
  ru_dzapp_ui_tick(a, f.T);
  ru_dzapp_row_info_t ri;
  CHECK(ru_dzapp_row_info(a, (size_t)row_of(a, "1"), &ri) && !strcmp(ri.sub, "ACTIVATED") && shows(a, "ACTIVATED"));
  f.T += RU_FB_CONFIRMED_SHOW_MS + 10;
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_row_info(a, (size_t)row_of(a, "1"), &ri) && !strcmp(ri.sub, "SCENE") && !shows(a, "ACTIVATED"));

  r.outcome = DZ_OUT_FAILED;
  r.fail = DZ_FAIL_FORBIDDEN;
  ru_dzapp_inject_result(a, &r);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "NO SWITCH RIGHTS") && !shows(a, "DONE"));
  /* a problem stays visible after a confirmation would have faded */
  f.T += RU_FB_CONFIRMED_SHOW_MS + 10;
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "NO SWITCH RIGHTS"));
  f.T += RU_FB_PROBLEM_SHOW_MS;
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_feedback(a)->st == RU_FB_IDLE);

  /* a lock goes through the same hold, then the detail screen shows the pending line */
  ru_dzapp_destroy(a);
  appfx_unload(&f);
  a = start(&f, FIXTURE_DIR, APPFX_MOCK, 600000, true);
  char idx[12];
  bool sc;
  CHECK(appfx_find(&f, DZ_CAP_LOCK, -1, idx, &sc));
  ru_dzapp_open(a, idx, false);
  CHECK(shows(a, "LOCKED") && shows(a, "UNLOCK"));   /* the state in words, and the button for the other state */
  click_at(size / 2, size / 2 + ru_u(14));           /* UNLOCK */
  CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM && SPY.n == 0);
  hold_confirm(size);
  CHECK(SPY.n == 1 && SPY.confirmed && !strcmp(SPY.idx, idx) && SPY.kind == DZ_CMD_OFF); /* a locked lock: unlock = Off */
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && shows(a, "SENDING") && !shows(a, "DONE"));
  finish(&f, a);
  sim_shutdown(&s);
}

/* ---- view only, stale data, unknown values ---- */

static void honesty(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;

  /* viewer token: no confirm screen, no command */
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_MOCK, 600000, false);
  CHECK(appfx_apply_scenes(&f, SCENES_JSON));
  ru_dzapp_show_section(a, RU_SEC_SCENES);
  CHECK(ru_dzapp_tap_row(a, (size_t)row_of(a, "3")));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && SPY.n == 0 && shows(a, "VIEW ONLY"));
  finish(&f, a);

  /* stale model: no hold for nothing, and the refusal says so */
  a = start(&f, FIXTURE_DIR, APPFX_MOCK, 1000, true);
  f.T += 5000;
  ru_dzapp_ui_tick(a, f.T);
  ru_dzapp_show_section(a, RU_SEC_SCENES);
  CHECK(ru_dzapp_tap_row(a, 0));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && SPY.n == 0 && shows(a, "DATA IS STALE"));
  ru_dzapp_show_section(a, RU_SEC_FAVS); /* old data is told by the rim dot only: no subtitle, the connection banner stays empty */
  CHECK(!shows(a, "STALE DATA") && !strcmp(ru_dzapp_banner(a), ""));
  finish(&f, a);

  /* unknown values are "--", never 0: a temperature with no reading, a power meter with none */
  const char *devs =
    "{\"status\":\"OK\",\"result\":["
    "{\"idx\":\"1\",\"Name\":\"NoTemp\",\"Type\":\"Temp\",\"SubType\":\"LaCrosse TX3\",\"Favorite\":1},"
    "{\"idx\":\"2\",\"Name\":\"NoPower\",\"Type\":\"P1 Smart Meter\",\"SubType\":\"Energy\",\"Favorite\":1},"
    "{\"idx\":\"3\",\"Name\":\"RealZero\",\"Type\":\"Temp\",\"SubType\":\"LaCrosse TX3\",\"Data\":\"0.0 C\",\"Temp\":0.0,\"Favorite\":1}]}";
  a = start(&f, FIXTURE_DIR, APPFX_EMPTY, 600000, true);
  CHECK(dz_model_apply(&f.model, devs, strlen(devs), DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  ru_dzapp_show_section(a, RU_SEC_TEMP);
  ru_dzapp_row_info_t ri;
  CHECK(ru_dzapp_row_info(a, (size_t)row_of(a, "1"), &ri) && !strcmp(ri.sub, "--"));
  ru_dzapp_show_section(a, RU_SEC_UTILITY);
  CHECK(ru_dzapp_row_info(a, (size_t)row_of(a, "2"), &ri) && !strcmp(ri.sub, "--"));
  CHECK(shows(a, "--"));
  sim_render();
  CHECK(sim_count_outside_lit(&s) == 0);
  ru_dzapp_show_section(a, RU_SEC_ENERGY); /* a meter with no reading still has an energy screen, with "--" */
  if (ru_dzapp_section(a) == RU_SEC_ENERGY) CHECK(shows(a, "--") && !shows(a, " W"));
  ru_dzapp_destroy(a);
  appfx_unload(&f);
  sim_shutdown(&s);
}

/* ---- energy, weather, settings, sections ---- */

static void sections(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_REAL, 600000, true);

  /* energy: power in W with its unit, today with the unit the server reported */
  CHECK(ru_dzapp_section_available(a, RU_SEC_ENERGY));
  ru_dzapp_show_section(a, RU_SEC_ENERGY);
  CHECK(ru_dzapp_section(a) == RU_SEC_ENERGY);
  CHECK(shows(a, "15 W") || (shows(a, "15") && shows(a, "W")));
  CHECK(shows(a, "TODAY 7.4 kWh") || shows(a, "TODAY"));
  ru_dzapp_destroy(a);
  appfx_unload(&f);

  /* kW and export direction from a literal document */
  const char *devs =
    "{\"status\":\"OK\",\"result\":["
    "{\"idx\":\"7\",\"Name\":\"Meter\",\"Type\":\"P1 Smart Meter\",\"SubType\":\"Energy\",\"Usage\":\"0 Watt\","
    "\"UsageDeliv\":\"1200 Watt\",\"CounterToday\":\"3.250 kWh\",\"Favorite\":1}]}";
  a = start(&f, FIXTURE_DIR, APPFX_EMPTY, 600000, true);
  CHECK(dz_model_apply(&f.model, devs, strlen(devs), DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  ru_dzapp_show_section(a, RU_SEC_ENERGY);
  CHECK(shows(a, "EXPORT") && shows(a, "1.2") && shows(a, "kW") && shows(a, "TODAY 3.2") && shows(a, "kWh"));
  CHECK(!ru_dzapp_section_available(a, RU_SEC_WEATHER)); /* no weather device: the app is hidden */
  ru_dzapp_show_section(a, RU_SEC_WEATHER);
  CHECK(ru_dzapp_section(a) == RU_SEC_FAVS);
  CHECK(!ru_dzapp_section_available(a, RU_SEC_SCENES));
  ru_dzapp_destroy(a);
  appfx_unload(&f);

  /* nothing at all: energy hidden too, home says so honestly */
  a = start(&f, FIXTURE_DIR, APPFX_EMPTY, 600000, true);
  CHECK(!ru_dzapp_section_available(a, RU_SEC_ENERGY));
  ru_dzapp_show_section(a, RU_SEC_FAVS);
  CHECK(shows(a, "NO FAVOURITES"));
  ru_dzapp_destroy(a);
  appfx_unload(&f);

  /* the weather page from the real fixture; the pager skips hidden sections; settings never show the token */
  a = start(&f, FIXTURE_DIR, APPFX_REAL, 600000, true);
  CHECK(ru_dzapp_section_available(a, RU_SEC_WEATHER));
  ru_dzapp_show_section(a, RU_SEC_WEATHER);
  CHECK(shows(a, "WEATHER") && ru_dzapp_home_rows(a) > 0);
  ru_dzapp_show_section(a, RU_SEC_SETTINGS);
  CHECK(shows(a, "CONNECTED") && shows(a, "SET") && shows(a, "****") && shows(a, "CONTROL") && !shows(a, TOKEN));
  CHECK(!shows(a, "fixture"));
  ru_dzapp_open_sub(a, RU_SUB_CONNECTION);
  CHECK(!shows(a, TOKEN));
  ru_dzapp_open_sub(a, RU_SUB_ABOUT);
  CHECK(shows(a, "LVGL 9.4") && !shows(a, TOKEN));
  ru_dzapp_show_section(a, RU_SEC_FAVS);
  int laps = 0;
  while (ru_dzapp_step_section(a, 1)) {
    CHECK(ru_dzapp_section_available(a, ru_dzapp_section(a)));
    laps++;
  }
  CHECK(ru_dzapp_section(a) == RU_SEC_SETTINGS && laps >= 3); /* no wrap: Settings is the end */
  while (ru_dzapp_step_section(a, -1)) {}
  CHECK(ru_dzapp_section(a) == RU_SEC_CLOCK);                 /* and the Clock the other end */

  /* a swipe left pages to the next section, through the same recogniser as the device */
  ru_dzapp_show_section(a, RU_SEC_FAVS);
  int c = size / 2;
  appfx_gestures_attach(a, size, g_square);
  appfx_drag(c + 90, c - 100, c - 90, c - 100, 8);
  CHECK(ru_dzapp_section(a) == RU_SEC_LIGHTS);
  appfx_gestures_detach();

  /* a switch device detail: tapping the toggle sends a plain On/Off (safe: no hold), pending afterwards */
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  char idx[12];
  bool sc;
  CHECK(appfx_find(&f, DZ_CAP_ONOFF, -1, idx, &sc));
  ru_dzapp_open(a, idx, false);
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL);
  click_at(c, c);
  CHECK(SPY.n == 1 && (SPY.kind == DZ_CMD_ON || SPY.kind == DZ_CMD_OFF) && !strcmp(SPY.idx, idx));
  CHECK(ru_dzapp_feedback(a)->st == RU_FB_PENDING);

  /* connection states are honest on home */
  ru_dzapp_show_section(a, RU_SEC_FAVS);
  ru_dzapp_set_conn(a, DZ_CS_OFFLINE);   ru_dzapp_ui_tick(a, f.T); CHECK(!strcmp(ru_dzapp_banner(a), "OFFLINE") && shows(a, "OFFLINE"));
  ru_dzapp_set_conn(a, DZ_CS_TOKEN_REJECTED); ru_dzapp_ui_tick(a, f.T); CHECK(!strcmp(ru_dzapp_banner(a), "TOKEN REJECTED"));
  ru_dzapp_set_conn(a, DZ_CS_CONNECTING); ru_dzapp_ui_tick(a, f.T); CHECK(!strcmp(ru_dzapp_banner(a), "CONNECTING..."));
  ru_dzapp_set_conn(a, DZ_CS_OK);        ru_dzapp_ui_tick(a, f.T); CHECK(!strcmp(ru_dzapp_banner(a), ""));

  /* the eight apps register with the runtime and show their section */
  ru_heap_t heap;
  ru_heap_init(&heap, 0);
  ru_runtime_t rt;
  ru_runtime_init(&rt, &heap, NULL, NULL);
  CHECK(ru_apps_register(&rt, a) == RU_OK);
  CHECK(ru_runtime_go_home(&rt) == RU_OK && ru_dzapp_section(a) == RU_SEC_FAVS);
  CHECK(ru_runtime_switch_to(&rt, RU_APP_ID_SCENES) == RU_OK && ru_dzapp_section(a) == RU_SEC_SCENES);
  CHECK(ru_runtime_switch_to(&rt, RU_APP_ID_ENERGY) == RU_OK && ru_dzapp_section(a) == RU_SEC_ENERGY);
  CHECK(ru_runtime_switch_to(&rt, RU_APP_ID_SETTINGS) == RU_OK && ru_dzapp_section(a) == RU_SEC_SETTINGS);
  CHECK(!strcmp(ru_runtime_foreground(&rt)->id, RU_APP_ID_SETTINGS));
  CHECK(ru_runtime_switch_to(&rt, RU_APP_ID_TEMP) == RU_OK && ru_dzapp_section(a) == RU_SEC_TEMP);
  CHECK(ru_runtime_switch_to(&rt, RU_APP_ID_UTILITY) == RU_OK && ru_dzapp_section(a) == RU_SEC_UTILITY);
  CHECK(ru_runtime_switch_to(&rt, RU_APP_ID_LIGHTS) == RU_OK && ru_dzapp_section(a) == RU_SEC_LIGHTS);
  CHECK(ru_apps_available(RU_SEC_FAVS) && ru_apps_available(RU_SEC_WEATHER));
  ru_runtime_shutdown(&rt);
  ru_apps_unbind();
  CHECK(heap.live == 0); /* every app state was freed */
  finish(&f, a);
  sim_shutdown(&s);
}


/* ---- climate: every family renders from readings, units come from getconfig, setpoints are honest ---- */

#define DEGC "\xC2\xB0" "C"
#define DEGF "\xC2\xB0" "F"

static void screen_ok(sim_t *s, ru_dzapp_t *a, const char *what, int size) {
  long area = (long)(3.14159f * (float)size * (float)size / 4);
  sim_render();
  long out = outside_lit(s, a), in = sim_count_lit(s), br = count_bright(s);
  if (out != 0 || in <= 300 || br >= area * 12 / 100) printf("  BAD %s@%d out=%ld lit=%ld bright=%.1f%%\n", what, size, out, in, 100.0 * (double)br / (double)area);
  CHECK(out == 0 && in > 300 && br < area * 12 / 100);
  CHECK(!shows(a, TOKEN) && !shows(a, "HEATING"));
}

static const char *const CL_IDX[] = {"100", "101", "102", "103", "104", "105", "106", "107", "108", "109", "110", "111", "112",
                                    "113", "114", "115", "116", "117", "118", "119", "120", "121", "122", "123", "124", "125"};
#define CL_N ((int)(sizeof CL_IDX / sizeof CL_IDX[0]))

static void climate_units(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  static const struct { appfx_set_t set; const char *yes[3]; const char *no[3]; } runs[] = {
    {APPFX_CLIMATE_METRIC, {DEGC, "m/s", NULL}, {DEGF, "mph", NULL}},
    {APPFX_CLIMATE_IMPERIAL, {DEGF, "mph", NULL}, {DEGC, "m/s", NULL}},
    {APPFX_CLIMATE_NOUNITS, {"21.5", "3.4", NULL}, {DEGF, DEGC, "mph"}},
  };
  static const ru_section_t pages[] = {RU_SEC_FAVS, RU_SEC_TEMP, RU_SEC_WEATHER, RU_SEC_UTILITY};
  for (size_t r = 0; r < 3; r++) {
    appfx_t f;
    ru_dzapp_t *a = start(&f, FIXTURE_DIR, runs[r].set, 600000, true);
    CHECK(ru_dzapp_section_available(a, RU_SEC_WEATHER) && ru_dzapp_section_available(a, RU_SEC_TEMP));
    bool saw[3] = {false, false, false};
    /* the detail page of every device follows the unit rules (the readings page of a sensor) */
    for (int k = 0; k < CL_N; k++) {
      ru_dzapp_open(a, CL_IDX[k], false);
      ru_dzapp_ui_tick(a, f.T);
      screen_ok(&s, a, CL_IDX[k], size);
      for (int u = 0; u < 3; u++) {
        if (runs[r].yes[u] && shows(a, runs[r].yes[u])) saw[u] = true;
        if (runs[r].no[u]) CHECK(!shows(a, runs[r].no[u]));
      }
      if (runs[r].set == APPFX_CLIMATE_NOUNITS) CHECK(!shows(a, "m/s") && !shows(a, DEGC));
    }
    CHECK(saw[0] && saw[1] && (runs[r].yes[2] == NULL || saw[2]));
    /* every page of the home grid and the lists: no hard-coded unit either */
    for (size_t p = 0; p < sizeof pages / sizeof pages[0]; p++) {
      ru_dzapp_show_section(a, RU_SEC_SETTINGS); /* a different section first: the page starts at 0 */
      ru_dzapp_show_section(a, pages[p]);
      int np = ru_dzapp_pages(a);
      for (int pg = 0; pg < np; pg++) {
        for (int u = 0; u < 3; u++) if (runs[r].no[u]) CHECK(!shows(a, runs[r].no[u]));
        screen_ok(&s, a, "page", size);
        if (pg + 1 < np) CHECK(ru_dzapp_swipe(a, RU_DIR_UP, size / 2, size / 2));
      }
    }
    finish(&f, a);
  }
  sim_shutdown(&s);
}

static void climate_pages(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 600000, true);
  /* the Temperature page: temperature-tab sensors, 6 tiles a page; a tap opens the readings page */
  ru_dzapp_show_section(a, RU_SEC_TEMP);
  CHECK(ru_dzapp_pages(a) == 2 && ru_dzapp_home_rows(a) == 6 && shows(a, "TEMPERATURE") && shows(a, "1/2"));
  { ru_dzapp_row_info_t r0; CHECK(ru_dzapp_row_info(a, 0, &r0) && !strcmp(r0.idx, "100")); } /* the name is cut with dots to the tile */
  CHECK(ru_dzapp_tap_row(a, 0));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && shows(a, "21.5" DEGC) && shows(a, "TEMP"));
  CHECK(ru_dzapp_back(a) && ru_dzapp_section(a) == RU_SEC_TEMP);
  /* a device on two tabs (Temp + Humidity + Baro) is on both pages; Wind TFA too */
  CHECK(row_of(a, "103") >= 0 || (ru_dzapp_swipe(a, RU_DIR_UP, size / 2, size / 2) && row_of(a, "103") >= 0));
  ru_dzapp_show_section(a, RU_SEC_WEATHER);
  CHECK(shows(a, "WEATHER"));
  bool on_weather = row_of(a, "103") >= 0 || (ru_dzapp_swipe(a, RU_DIR_UP, size / 2, size / 2) && row_of(a, "103") >= 0);
  CHECK(on_weather);
  /* readings pages (detail) of the sensors, as before */
  ru_dzapp_open(a, "104", false);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "WIND SENSOR") && shows(a, "3.4") && shows(a, "m/s") && shows(a, "NW") && shows(a, "GUST 6.1 m/s"));
  CHECK(shows(a, "SENSOR TEMP 12.0" DEGC)); /* its own thermometer, labelled as such */
  ru_dzapp_open(a, "103", false);
  CHECK(shows(a, "HUMIDITY 52%") && shows(a, "NORMAL") && shows(a, "1009 hPa") && shows(a, "SUNNY") && shows(a, "DEW POINT 10.0" DEGC));
  ru_dzapp_open(a, "101", false);
  CHECK(shows(a, "71%") && shows(a, "WET"));
  ru_dzapp_open(a, "105", false);
  CHECK(shows(a, "12.5") && shows(a, "RATE 0.4") && !shows(a, "mm"));   /* Domoticz gives rain no unit in the field: none shown */
  ru_dzapp_open(a, "106", false);
  CHECK(shows(a, "UV INDEX") && shows(a, "MODERATE") && shows(a, "SENSOR TEMP"));
  ru_dzapp_open(a, "107", false);
  CHECK(shows(a, "123") && shows(a, "Lux"));
  ru_dzapp_open(a, "108", false);
  CHECK(shows(a, "850") && shows(a, "ppm") && shows(a, "FAIR"));
  /* a type nobody has seen still shows its number with its unit (it is on no page: a favourite on Home only) */
  ru_dzapp_open(a, "116", false);
  CHECK(shows(a, "UNSEEN SENSOR") && shows(a, "7.5") && shows(a, "zorp"));
  ru_dzapp_open(a, "113", false);
  CHECK(shows(a, "All good"));
  finish(&f, a);

  /* a wind sensor's own temperature is never shown as a weather temperature: wind and UV rows show their own reading */
  const char *devs =
    "{\"status\":\"OK\",\"result\":["
    "{\"idx\":\"1\",\"Name\":\"W\",\"Type\":\"Wind\",\"SubType\":\"TFA\",\"Speed\":\"2.0\",\"Gust\":\"3.0\",\"Direction\":90,\"DirectionStr\":\"E\",\"Temp\":12.5,\"Chill\":10.0,\"Favorite\":1},"
    "{\"idx\":\"2\",\"Name\":\"U\",\"Type\":\"UV\",\"SubType\":\"UV3\",\"UVI\":\"3\",\"Temp\":19.5,\"Favorite\":1}]}";
  a = start(&f, FIXTURE_DIR, APPFX_EMPTY, 600000, true);
  CHECK(dz_model_apply(&f.model, devs, strlen(devs), DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  dz_units_t u = {.have_temp = true, .temp = "C", .have_wind = true, .wind = "m/s"};
  dz_model_set_units(&f.model, &u);
  ru_dzapp_ui_tick(a, f.T + 1);
  ru_dzapp_show_section(a, RU_SEC_FAVS);
  CHECK(!shows(a, "12.5") && !shows(a, "19.5"));
  ru_dzapp_show_section(a, RU_SEC_WEATHER);
  CHECK(!shows(a, "19.5" DEGC) && row_of(a, "2") >= 0);
  finish(&f, a);

  /* stale sensor: marked, numbers dimmed, never silently shown as fresh */
  a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 300000, true);
  f.T += 420000;
  ru_dzapp_ui_tick(a, f.T);
  ru_dzapp_show_section(a, RU_SEC_TEMP);
  screen_ok(&s, a, "stale rows", size);
  ru_dzapp_open(a, "100", false);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "UPDATED") && shows(a, "MIN AGO"));
  screen_ok(&s, a, "stale", size);
  finish(&f, a);

  /* low battery: said so, only when known and low */
  const char *bat =
    "{\"status\":\"OK\",\"result\":["
    "{\"idx\":\"1\",\"Name\":\"Low\",\"Type\":\"Temp\",\"Temp\":20.0,\"BatteryLevel\":5},"
    "{\"idx\":\"2\",\"Name\":\"Full\",\"Type\":\"Temp\",\"Temp\":21.0,\"BatteryLevel\":100},"
    "{\"idx\":\"3\",\"Name\":\"None\",\"Type\":\"Temp\",\"Temp\":22.0,\"BatteryLevel\":255}]}";
  a = start(&f, FIXTURE_DIR, APPFX_EMPTY, 600000, true);
  CHECK(dz_model_apply(&f.model, bat, strlen(bat), DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  ru_dzapp_open(a, "1", false);
  CHECK(shows(a, "BATTERY LOW"));
  ru_dzapp_open(a, "2", false);
  CHECK(!shows(a, "BATTERY"));
  ru_dzapp_open(a, "3", false);
  CHECK(!shows(a, "BATTERY"));
  finish(&f, a);
  sim_shutdown(&s);
}

/* the + button of the dial, in screen pixels */
static void tap_plus(int size) { click_at(size / 2 + ru_u(14), size / 2 + ru_u(24)); }

static void climate_thermostat(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 600000, true);

  /* a plain setpoint has no temperature of its own: NOW comes from the single sensor of its plan */
  ru_dzapp_open(a, "118", false);
  CHECK(shows(a, "19.0" DEGC) && shows(a, "NOW 21.5" DEGC));
  screen_ok(&s, a, "setpoint", size);
  /* two sensors in its plan, or only a wind and a UV sensor: NOW stays unknown */
  ru_dzapp_open(a, "121", false);
  CHECK(shows(a, "NOW --") && !shows(a, "NOW 17") && !shows(a, "NOW 18"));
  ru_dzapp_open(a, "122", false);
  CHECK(shows(a, "NOW --") && !shows(a, "12.0") && !shows(a, "20.1"));
  /* a thermostat with its own temperature uses it */
  ru_dzapp_open(a, "117", false);
  CHECK(shows(a, "21.0" DEGC) && shows(a, "NOW 20.5" DEGC));
  screen_ok(&s, a, "thermostat6", size);

  /* tap + : nothing is sent by the tap; a short press does not send and stays on the hold screen; a hold sends once, confirmed */
  ru_dzapp_open(a, "118", false);
  memset(&SPY, 0, sizeof SPY);
  tap_plus(size);
  CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM && SPY.n == 0);
  CHECK(!strncmp(ru_dzapp_confirm_verb(a), "SET 19.5", 8) && shows(a, DEGC) && ru_dzapp_confirm_kind(a) == DZ_CMD_SETPOINT);
  click_at(size / 2, size / 2 + ru_u(13));
  CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM && SPY.n == 0);
  hold_confirm(size);
  CHECK(SPY.n == 1 && SPY.kind == DZ_CMD_SETPOINT && !strcmp(SPY.idx, "118") && SPY.confirmed && SPY.sp > 19.45f && SPY.sp < 19.55f);
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && shows(a, "SENDING") && !shows(a, "DONE"));
  /* cancel leaves without sending */
  memset(&SPY, 0, sizeof SPY);
  ru_dzapp_open(a, "118", false);
  tap_plus(size);
  click_at(size / 2, size / 2 + ru_u(42));
  CHECK(SPY.n == 0 && ru_dzapp_view(a) == RU_VIEW_DETAIL);

  /* no sane range (radiator, Evohome zone): read only, no + button, nothing can be sent */
  const char *ro[] = {"123", "124"};
  for (int i = 0; i < 2; i++) {
    memset(&SPY, 0, sizeof SPY);
    ru_dzapp_open(a, ro[i], false);
    CHECK(shows(a, "READ ONLY") && shows(a, "NO RANGE"));
    screen_ok(&s, a, ro[i], size);
    tap_plus(size);
    CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && SPY.n == 0);
  }
  ru_dzapp_open(a, "124", false);
  CHECK(shows(a, "NOW 19.5" DEGC)); /* the zone's own temperature */
  /* a viewer token: read only too */
  ru_dzapp_destroy(a);
  appfx_unload(&f);
  a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 600000, false);
  ru_dzapp_open(a, "118", false);
  CHECK(shows(a, "READ ONLY") && shows(a, "VIEW ONLY"));
  memset(&SPY, 0, sizeof SPY);
  tap_plus(size);
  CHECK(SPY.n == 0 && ru_dzapp_view(a) == RU_VIEW_DETAIL);
  finish(&f, a);

  /* Fahrenheit: the dial speaks F, with F range */
  a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_IMPERIAL, 600000, true);
  ru_dzapp_open(a, "118", false);
  CHECK(shows(a, "66.2" DEGF) && shows(a, "NOW 70.7" DEGF) && !shows(a, DEGC));
  memset(&SPY, 0, sizeof SPY);
  tap_plus(size);
  /* the MCP setpoint tool takes Celsius only: a Fahrenheit server's setpoints are read-only, no hold opens, nothing is sent */
  CHECK(shows(a, "READ ONLY") && shows(a, "FAHRENHEIT") && ru_dzapp_view(a) == RU_VIEW_DETAIL && SPY.n == 0);
  ru_dzapp_pick_setpoint(a, 67.0f);
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && SPY.n == 0);
  finish(&f, a);

  /* units unknown: bare numbers on the dial; the device's own vunit is still its word, but NOW has no symbol */
  a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_NOUNITS, 600000, true);
  ru_dzapp_open(a, "118", false);
  CHECK(shows(a, "NOW 21.5") && !shows(a, "NOW 21.5" DEGC) && !shows(a, "NOW 21.5" DEGF));
  finish(&f, a);

  /* thermostat mode: the modes are shown, but the MCP endpoint has no tool for them, so the screen says VIEW ONLY and a pick
   * (the test hook) is refused: no hold, no command */
  a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 600000, true);
  ru_dzapp_open(a, "125", false);
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && shows(a, "MODE: VIEW ONLY"));
  screen_ok(&s, a, "mode", size);
  memset(&SPY, 0, sizeof SPY);
  ru_dzapp_pick_mode(a, 2);
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && SPY.n == 0);
  /* a mode slot that does not exist sends nothing */
  memset(&SPY, 0, sizeof SPY);
  ru_dzapp_open(a, "125", false);
  ru_dzapp_pick_mode(a, 9);
  CHECK(SPY.n == 0 && ru_dzapp_view(a) == RU_VIEW_DETAIL);
  finish(&f, a);
  sim_shutdown(&s);
}

/* ---- device path: dz_service is the backend; the app never runs its own net step ---- */

static struct { int n; dz_cmd_kind_t kind; char idx[12]; bool confirmed; dz_conn_state_t cs; } BK;
static uint32_t bk_submit(const dz_cmd_req_t *r, void *ud) {
  (void)ud;
  BK.n++;
  BK.kind = r->kind;
  snprintf(BK.idx, sizeof BK.idx, "%s", r->idx);
  BK.confirmed = r->confirmed;
  return 41u;
}
static uint32_t bk_full(const dz_cmd_req_t *r, void *ud) { (void)r; (void)ud; return 0; }
static dz_conn_state_t bk_conn(void *ud) { (void)ud; return BK.cs; }
static const ru_dzapp_backend_t BACKEND = { bk_submit, bk_conn };
static const ru_dzapp_backend_t BACKEND_FULL = { bk_full, bk_conn };

static void backend_path(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_MOCK, 600000, true);
  CHECK(appfx_apply_scenes(&f, SCENES_JSON));
  memset(&BK, 0, sizeof BK);
  BK.cs = DZ_CS_OFFLINE;
  ru_dzapp_set_backend(a, &BACKEND, NULL);
  ru_dzapp_show_section(a, RU_SEC_SCENES);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_banner(a)[0] != 0);                 /* connection state comes from the backend */
  BK.cs = DZ_CS_OK;
  /* a Mixed group: tap + short press sends nothing, a hold goes through the backend as SCENE_ON, confirmed */
  CHECK(ru_dzapp_tap_row(a, (size_t)row_of(a, "5")));
  click_at(size / 2, size / 2 + ru_u(13));
  CHECK(BK.n == 0 && ru_dzapp_view(a) == RU_VIEW_CONFIRM);
  hold_confirm(size);
  CHECK(BK.n == 1 && BK.kind == DZ_CMD_SCENE_ON && !strcmp(BK.idx, "5") && BK.confirmed);
  CHECK(ru_dzapp_feedback(a)->st == RU_FB_PENDING);
  /* the Group is still Mixed in the model: the service reports UNCONFIRMED and the app says so */
  f.T += 100;
  ru_dzapp_post_cmd_event(a, 41u, DZ_OUT_UNCONFIRMED, DZ_FAIL_NONE, DZ_CMD_SCENE_ON, DZ_CMD_ACCEPTED, -1);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_feedback(a)->st == RU_FB_UNCONFIRMED && !strcmp(ru_dzapp_feedback(a)->text, "STILL MIXED"));
  CHECK(!shows(a, "DONE"));
  /* a refusal from the service (stale) shows as a failure, never as success */
  ru_dzapp_post_cmd_event(a, 42u, DZ_OUT_FAILED, DZ_FAIL_NONE, DZ_CMD_SCENE_ON, DZ_CMDE_STALE, -1);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_feedback(a)->st == RU_FB_FAILED);
  /* full queue: BUSY, no pending */
  ru_dzapp_set_backend(a, &BACKEND_FULL, NULL);
  ru_dzapp_show_section(a, RU_SEC_SCENES);
  CHECK(ru_dzapp_tap_row(a, (size_t)row_of(a, "3")));
  hold_confirm(size);
  CHECK(ru_dzapp_feedback(a)->st == RU_FB_FAILED && !strcmp(ru_dzapp_feedback(a)->text, "BUSY"));
  finish(&f, a);
  sim_shutdown(&s);
}

/* the clickable 56 px corner box of the current screen (square panels) */
static lv_obj_t *find_corner_box(lv_obj_t *o) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_x(o) == 8 && lv_obj_get_y(o) == 8 && lv_obj_get_width(o) == 56) return o;
  uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t *r = find_corner_box(lv_obj_get_child(o, i));
    if (r) return r;
  }
  return NULL;
}

/* A swipe from the corner chevron: the shell's BACK fires, then the finger lifts inside the old chevron and LVGL
 * delivers CLICKED to it before the old screen is freed. That must not be a second step back. */
static void back_double_fire(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 600000, true);
  if (g_square) {
    ru_dzapp_show_section(a, RU_SEC_UTILITY);
    ru_dzapp_open(a, "118", false);
    sim_render();
    lv_obj_t *old_chev = find_corner_box(ru_dzapp_screen(a));
    CHECK(old_chev != NULL);
    CHECK(ru_dzapp_back(a)); /* what shell_gesture does */
    CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_UTILITY);
    if (old_chev) lv_obj_send_event(old_chev, LV_EVENT_CLICKED, NULL); /* the old screen is still alive: no timer ran */
    CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_UTILITY); /* one step, not two */
    /* a click on the live chevron still works */
    ru_dzapp_open(a, "118", false);
    sim_render();
    lv_obj_t *chev = find_corner_box(ru_dzapp_screen(a));
    CHECK(chev != NULL);
    if (chev) lv_obj_send_event(chev, LV_EVENT_CLICKED, NULL);
    CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_UTILITY);
  }
  finish(&f, a);
  sim_shutdown(&s);
}

/* The Lights page is the Domoticz Switches tab: one device per switch class (fixtures/switch_types.json), all of them, in
 * server order, 6 tiles per page, each with a non-empty tile; a tap opens a detail (read-only ones the read-only detail). */
static void switch_lights(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  char path[512];
  snprintf(path, sizeof path, "%s/switch_types.json", FIXTURE_DIR);
  FILE *fp = fopen(path, "rb");
  CHECK(fp != NULL);
  if (!fp) { sim_shutdown(&s); return; }
  static char buf[65536];
  size_t n = fread(buf, 1, sizeof buf - 1, fp);
  fclose(fp);
  buf[n] = 0;
  appfx_t f;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_EMPTY, 600000, true);
  CHECK(dz_model_apply(&f.model, buf, n, DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  int seen = 0;
  for (int pg = 0; pg < 6; pg++) {
    ru_dzapp_row_info_t ri;
    for (size_t i = 0; ru_dzapp_row_info(a, i, &ri); i++) {
      seen++;
      CHECK(atoi(ri.idx) == seen);
      CHECK(ri.sub[0] != 0 || ri.dot != RU_DOT_NONE);
    }
    screen_ok(&s, a, "switch lights page", size);
    if (pg < 5) CHECK(ru_dzapp_swipe(a, RU_DIR_UP, size / 2, size / 2));
  }
  CHECK(seen == 32 && ru_dzapp_pages(a) == 6);
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  static const char *const open_idx[] = { "2", "4", "13", "17", "19", "25", "26" }; /* dimmer, blinds, push, lock, contact, siren, media */
  for (size_t i = 0; i < sizeof open_idx / sizeof open_idx[0]; i++) {
    ru_dzapp_open(a, open_idx[i], false);
    sim_render();
    CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL);
    screen_ok(&s, a, open_idx[i], size);
    CHECK(ru_dzapp_back(a) && ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_LIGHTS);
  }
  finish(&f, a);
  sim_shutdown(&s);
}

/* Temperature and Weather tiles with 36-character names and long values: the name area (cut with dots) and the value area
 * never overlap, both stay inside the tile, nothing is lit outside the circle, and temperature + humidity share one tile. */
static bool rename_dev(char *buf, size_t *n, size_t cap, const char *old, const char *neu) {
  char key[96], rep[96];
  snprintf(key, sizeof key, "\"Name\": \"%s\"", old);
  snprintf(rep, sizeof rep, "\"Name\": \"%s\"", neu);
  char *p = strstr(buf, key);
  if (!p) return false;
  size_t kl = strlen(key), rl = strlen(rep), tail = *n - (size_t)(p - buf) - kl;
  if (*n - kl + rl >= cap) return false;
  memmove(p + rl, p + kl, tail);
  memcpy(p, rep, rl);
  *n = *n - kl + rl;
  buf[*n] = 0;
  return true;
}

static int overlap_tiles(lv_obj_t *o, int *tiles) {
  int bad = 0;
  if (lv_obj_get_child_count(o) == 4 && lv_obj_check_type(lv_obj_get_child(o, 1), &lv_label_class) &&
      lv_obj_check_type(lv_obj_get_child(o, 2), &lv_label_class) && lv_obj_check_type(lv_obj_get_child(o, 3), &lv_label_class)) {
    lv_area_t t, nm, sb, vl;
    lv_obj_get_coords(o, &t);
    lv_obj_get_coords(lv_obj_get_child(o, 1), &nm);
    lv_obj_get_coords(lv_obj_get_child(o, 2), &sb);
    lv_obj_get_coords(lv_obj_get_child(o, 3), &vl);
    bool val_shown = !lv_obj_has_flag(lv_obj_get_child(o, 3), LV_OBJ_FLAG_HIDDEN);
    (*tiles)++;
    if (nm.x1 < t.x1 || nm.x2 > t.x2 || nm.y1 < t.y1 || nm.y2 > t.y2) bad++;
    if (sb.x1 < t.x1 || sb.x2 > t.x2 || sb.y2 > t.y2) bad++;
    if (val_shown) {
      lv_area_t x;
      if (vl.x1 < t.x1 || vl.x2 > t.x2 || vl.y1 < t.y1 || vl.y2 > t.y2) bad++;
      if (_lv_area_intersect(&x, &nm, &vl)) bad++;
      if (_lv_area_intersect(&x, &sb, &vl)) bad++;
      if (_lv_area_intersect(&x, &nm, &sb)) bad++;
    }
  }
  /* a weather tile: name, icon, value, unit, small line, dot (ru_wx_tile): nothing touches, everything inside the tile */
  if (lv_obj_get_child_count(o) == 6 && lv_obj_check_type(lv_obj_get_child(o, 0), &lv_label_class) && lv_obj_check_type(lv_obj_get_child(o, 4), &lv_label_class)) {
    lv_area_t t;
    lv_obj_get_coords(o, &t);
    (*tiles)++;
    for (uint32_t i = 0; i < 5; i++) {
      lv_obj_t *a = lv_obj_get_child(o, (int32_t)i);
      if (lv_obj_has_flag(a, LV_OBJ_FLAG_HIDDEN) || !lv_label_get_text(a)[0]) continue;
      lv_area_t ca, x;
      lv_obj_get_coords(a, &ca);
      if (ca.x1 < t.x1 || ca.x2 > t.x2 || ca.y1 < t.y1 || ca.y2 > t.y2) bad++;
      for (uint32_t j = i + 1; j < 5; j++) {
        lv_obj_t *b = lv_obj_get_child(o, (int32_t)j);
        if (lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN) || !lv_label_get_text(b)[0]) continue;
        lv_area_t cb;
        lv_obj_get_coords(b, &cb);
        if (_lv_area_intersect(&x, &ca, &cb)) bad++;
      }
    }
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) bad += overlap_tiles(lv_obj_get_child(o, i), tiles);
  return bad;
}

static bool count_tab(size_t i, const dz_device_t *d, bool scene, ru_val_state_t st, uint16_t order, void *ud) {
  (void)i; (void)st; (void)order;
  int *c = ud;
  if (!scene && (d->tabs & c[0])) c[1]++;
  return true;
}

static void climate_tiles_long(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  char path[512];
  snprintf(path, sizeof path, "%s/mock_climate_metric.json", FIXTURE_DIR);
  FILE *fp = fopen(path, "rb");
  CHECK(fp != NULL);
  if (!fp) { sim_shutdown(&s); return; }
  static char buf[200000];
  size_t n = fread(buf, 1, sizeof buf - 1, fp);
  fclose(fp);
  buf[n] = 0;
  CHECK(rename_dev(buf, &n, sizeof buf, "Temp Hum Sensor", "LIVING ROOM WINDOW SENSOR NORTH WALL") &&
        rename_dev(buf, &n, sizeof buf, "Wind Sensor", "GARDEN ANEMOMETER ON THE ROOF TOP MAST") &&
        rename_dev(buf, &n, sizeof buf, "Weather Station", "WEATHER STATION BEHIND THE BARN DOOR"));
  appfx_t f;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 600000, true);
  CHECK(dz_model_apply(&f.model, buf, n, DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  static const ru_section_t secs[2] = { RU_SEC_TEMP, RU_SEC_WEATHER };
  bool saw_both = false, saw_wind = false, saw_dots = false;
  for (int k = 0; k < 2; k++) {
    ru_dzapp_show_section(a, secs[k]);
    int pages = ru_dzapp_pages(a);
    int cnt[2] = { secs[k] == RU_SEC_TEMP ? DZ_TAB_TEMP : DZ_TAB_WEATHER, 0 };
    dz_model_foreach(&f.model, f.T, count_tab, cnt);
    CHECK(pages == (cnt[1] + 5) / 6);
    for (int pg = 0; pg < pages; pg++) {
      sim_render();
      int tiles = 0;
      CHECK(overlap_tiles(ru_dzapp_screen(a), &tiles) == 0);
      int expect = cnt[1] - pg * 6 < 6 ? cnt[1] - pg * 6 : 6;
      if (tiles != expect) printf("section %d page %d: tiles %d expected %d\n", k, pg, tiles, expect);
      CHECK(tiles == expect);
      saw_both = saw_both || shows(a, "%"); /* the humidity line of a temperature tile */
      saw_wind = saw_wind || shows(a, "G "); /* the gust line of a wind tile */
      saw_dots = saw_dots || shows(a, "...");
      screen_ok(&s, a, "climate tiles", size);
      if (pg + 1 < pages) CHECK(ru_dzapp_swipe(a, RU_DIR_UP, size / 2, size / 2));
    }
  }
  CHECK(saw_both && saw_wind && saw_dots);
  finish(&f, a);
  sim_shutdown(&s);
}

/* A device name of 29 characters on the +40u dial pill: dots, nothing lit outside the circle. */
static void long_name_pill(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  char path[512];
  snprintf(path, sizeof path, "%s/mock_climate_metric.json", FIXTURE_DIR);
  FILE *fp = fopen(path, "rb");
  CHECK(fp != NULL);
  if (!fp) { sim_shutdown(&s); return; }
  static char buf[200000];
  size_t n = fread(buf, 1, sizeof buf - 1, fp);
  fclose(fp);
  buf[n] = 0;
  char *p = strstr(buf, "\"Name\": \"Plain Setpoint\"");
  CHECK(p != NULL);
  static char out[200000];
  if (p) {
    const char *newn = "\"Name\": \"LIVING ROOM CEILING LIGHT BIG\"";
    size_t head = (size_t)(p - buf), oldl = strlen("\"Name\": \"Plain Setpoint\"");
    memcpy(out, buf, head);
    size_t nl = strlen(newn);
    memcpy(out + head, newn, nl);
    memcpy(out + head + nl, p + oldl, n - head - oldl);
    size_t total = n - oldl + nl;
    appfx_t f;
    ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 600000, true);
    CHECK(dz_model_apply(&f.model, out, total, DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
    ru_dzapp_show_section(a, RU_SEC_UTILITY);
    ru_dzapp_open(a, "118", false);
    sim_render();
    CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL);
    CHECK(shows(a, "LIVING") && shows(a, "...") && !shows(a, "BIG")); /* cut with dots, not clipped mid-glyph */
    CHECK(outside_lit(&s, a) == 0);
    CHECK(sim_count_lit(&s) > 300);
    finish(&f, a);
  }
  sim_shutdown(&s);
}

/* ---- one way back: ru_dzapp_back and the visible back controls ---- */

static void nav_back_tests(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  int c = size / 2;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 600000, true);

  /* logic */
  ru_dzapp_home(a);
  CHECK(!ru_dzapp_back(a) && ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_FAVS); /* Home: nowhere to go */
  ru_dzapp_show_section(a, RU_SEC_TEMP);
  CHECK(ru_dzapp_back(a) && ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_FAVS);
  ru_dzapp_show_section(a, RU_SEC_UTILITY);
  ru_dzapp_open(a, "118", false);
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL);
  CHECK(ru_dzapp_back(a) && ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_UTILITY); /* detail -> its section */
  ru_dzapp_open(a, "118", false);
  tap_plus(size);
  CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM);
  CHECK(ru_dzapp_back(a) && ru_dzapp_view(a) == RU_VIEW_DETAIL && !strcmp(ru_dzapp_current_idx(a), "118")); /* confirm -> detail */
  CHECK(ru_dzapp_back(a) && ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_UTILITY);
  ru_dzapp_show_section(a, RU_SEC_SETTINGS);
  ru_dzapp_open_sub(a, RU_SUB_BRIGHTNESS);
  CHECK(ru_dzapp_view(a) == RU_VIEW_SUB);
  ru_dzapp_open(a, "118", false); /* aim a setpoint at a device, then raise its confirm from the sub screen */
  ru_dzapp_open_sub(a, RU_SUB_BRIGHTNESS);
  ru_dzapp_pick_setpoint(a, 22.0f);
  CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM);
  CHECK(ru_dzapp_back(a) && ru_dzapp_view(a) == RU_VIEW_SUB);                     /* confirm -> sub */
  CHECK(ru_dzapp_back(a) && ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_SETTINGS); /* sub -> Settings */
  CHECK(ru_dzapp_back(a) && ru_dzapp_section(a) == RU_SEC_FAVS);
  CHECK(!ru_dzapp_back(a));

  /* the other device's feedback line does not show on an open detail */
  {
    dz_cmd_result_t r;
    memset(&r, 0, sizeof r);
    strcpy(r.idx, "104");
    r.kind = DZ_CMD_SETPOINT;
    r.outcome = DZ_OUT_CONFIRMED;
    ru_dzapp_inject_result(a, &r);
    ru_dzapp_open(a, "118", false);
    CHECK(!shows(a, "DONE"));
    ru_dzapp_open(a, "104", false);
    CHECK(shows(a, "DONE"));
    ru_dzapp_home(a);
  }

  /* clicks: dial pages (name pill at +40u or -40u), the confirm pill, the square corner chevron */
  static const struct { const char *idx; float dy; } dial[] = { { "118", 40 }, { "125", 40 }, { "104", -40.5f } };
  for (size_t i = 0; i < 3; i++) {
    ru_dzapp_show_section(a, RU_SEC_UTILITY);
    ru_dzapp_open(a, dial[i].idx, false);
    sim_render();
    click_at(c, c + ru_u(dial[i].dy));
    CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_UTILITY);
    if (g_square) {
      ru_dzapp_open(a, dial[i].idx, false);
      sim_render();
      click_at(36, 36);
      CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_UTILITY);
    } else {
      lv_area_t r;
      CHECK(!ru_dzapp_back_rect(a, &r));
    }
  }
  ru_dzapp_show_section(a, RU_SEC_UTILITY);
  ru_dzapp_open(a, "118", false);
  tap_plus(size);
  CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM);
  click_at(c, c + ru_u(42)); /* CANCEL pill: back to the detail, not the section */
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL);
  if (g_square) {
    tap_plus(size);
    CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM);
    click_at(36, 36);
    CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL);
    click_at(36, 36);
    CHECK(ru_dzapp_view(a) == RU_VIEW_HOME);
  }
  /* a sub screen: the BACK pill leads to Settings */
  ru_dzapp_show_section(a, RU_SEC_SETTINGS);
  ru_dzapp_open_sub(a, RU_SUB_ABOUT);
  sim_render();
  click_at(c, c + ru_u(42));
  CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_SETTINGS);
  finish(&f, a);

  /* a non-dial detail: the BACK pill at +42u */
  a = start(&f, FIXTURE_DIR, APPFX_MOCK, 600000, true);
  char idx[12];
  bool sc;
  CHECK(appfx_find(&f, DZ_CAP_ONOFF, -1, idx, &sc));
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  ru_dzapp_open(a, idx, false);
  sim_render();
  click_at(c, c + ru_u(42));
  CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_LIGHTS);
  finish(&f, a);
  sim_shutdown(&s);
}


/* ---- the Clock page ---- */

static void clock_page(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  int c = size / 2;
  char hm[8], dt[12];
  appfx_t f;

  /* three gauges from the first numeric favourites, the server time from the last commit */
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_REAL, 600000, true);
  dz_units_t un;
  memset(&un, 0, sizeof un);
  un.have_temp = true;
  strcpy(un.temp, "C");
  dz_model_set_units(&f.model, &un);
  ru_dzapp_show_section(a, RU_SEC_CLOCK);
  CHECK(ru_dzapp_section(a) == RU_SEC_CLOCK && ru_dzapp_view(a) == RU_VIEW_HOME);
  CHECK(ru_dzapp_clock_text(a, hm, dt) && !strcmp(hm, "13:07") && !strcmp(dt, "SAT 31 OCT"));
  CHECK(shows(a, "SAT 31 OCT") && shows(a, "13:") && shows(a, "07"));
  CHECK(ru_dzapp_clock_gauges(a) == 3 && ru_dzapp_home_rows(a) == 3);
  screen_ok(&s, a, "clock", size);
  lv_obj_t *scr = ru_dzapp_screen(a);

  /* time moves on its own, in place: no rebuild of the screen */
  f.T += 61000;
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_clock_text(a, hm, dt) && !strcmp(hm, "13:08") && !strcmp(dt, "SAT 31 OCT"));
  CHECK(ru_dzapp_screen(a) == scr);
  f.T += 3 * 3600000u; /* the values go stale, the structure stays */
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_clock_text(a, hm, dt) && !strcmp(hm, "16:08"));

  /* a tap on a gauge opens that device, back returns to the clock */
  ru_dzapp_row_info_t ri;
  CHECK(ru_dzapp_row_info(a, 0, &ri) && ri.idx[0] && !ri.scene);
  char idx0[12];
  snprintf(idx0, sizeof idx0, "%s", ri.idx);
  CHECK(ru_dzapp_tap_row(a, 0) && ru_dzapp_view(a) == RU_VIEW_DETAIL && !strcmp(ru_dzapp_current_idx(a), idx0));
  CHECK(ru_dzapp_back(a) && ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_CLOCK);
  /* ... and a finger on the middle gauge does the same */
  CHECK(ru_dzapp_row_info(a, 1, &ri));
  sim_render();
  click_at(c, c + ru_u(20));
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && !strcmp(ru_dzapp_current_idx(a), ri.idx));
  finish(&f, a);

  /* no ServerTime yet: "--:--" and no date, never a time that was made up */
  a = start(&f, FIXTURE_DIR, APPFX_REAL_NOSYNC, 600000, true);
  ru_dzapp_show_section(a, RU_SEC_CLOCK);
  CHECK(ru_dzapp_clock_text(a, hm, dt) && !strcmp(hm, "--:--") && !dt[0]);
  CHECK(shows(a, "--:--") && !shows(a, "SAT") && !shows(a, "12:") && !shows(a, "13:"));
  screen_ok(&s, a, "clock_nosync", size);
  /* the first commit with a ServerTime fills it in, in place */
  scr = ru_dzapp_screen(a);
  const char *tick = "{\"status\":\"OK\",\"ServerTime\":\"2026-10-31 13:07:41\",\"result\":[]}";
  CHECK(dz_model_apply(&f.model, tick, strlen(tick), DZ_COMMIT_DELTA, f.T, NULL, NULL) == DZ_OK);
  f.T += 2000;
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_clock_text(a, hm, dt) && !strcmp(hm, "13:07") && !strcmp(dt, "SAT 31 OCT") && ru_dzapp_screen(a) == scr);
  finish(&f, a);

  /* two gauges, centred; values go into the existing gauges; the picked devices are the structure */
  a = start(&f, FIXTURE_DIR, APPFX_CLOCK_TWO, 600000, true);
  dz_model_set_units(&f.model, &un);
  ru_dzapp_show_section(a, RU_SEC_CLOCK);
  CHECK(ru_dzapp_clock_gauges(a) == 2 && shows(a, "19.5" DEGC) && shows(a, "640W"));
  screen_ok(&s, a, "clock_two", size);
  scr = ru_dzapp_screen(a);
  const char *upd = "{\"status\":\"OK\",\"ServerTime\":\"2026-10-31 13:08:41\",\"result\":["
    "{\"idx\":\"1\",\"Name\":\"Hallway\",\"Type\":\"Temp\",\"SubType\":\"LaCrosse TX3\",\"Temp\":20.5,\"Data\":\"20.5 C\",\"Favorite\":1,\"LastUpdate\":\"2026-10-31 13:08:00\"},"
    "{\"idx\":\"2\",\"Name\":\"Mains\",\"Type\":\"Usage\",\"SubType\":\"Electric\",\"Data\":\"2100 Watt\",\"Usage\":\"2100 Watt\",\"Favorite\":1,\"LastUpdate\":\"2026-10-31 13:08:00\"},"
    "{\"idx\":\"3\",\"Name\":\"Porch\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"SwitchType\":\"On/Off\",\"SwitchTypeVal\":0,\"Status\":\"On\",\"Data\":\"On\",\"Favorite\":1}]}";
  f.T += 5000;
  CHECK(dz_model_apply(&f.model, upd, strlen(upd), DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  f.T += 1600;
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_screen(a) == scr && shows(a, "20.5" DEGC) && shows(a, "2.1kW") && !shows(a, "640W"));
  CHECK(ru_dzapp_clock_text(a, hm, dt) && !strcmp(hm, "13:08"));
  /* a third numeric favourite changes the structure: the screen is rebuilt with three gauges */
  const char *third = "{\"status\":\"OK\",\"ServerTime\":\"2026-10-31 13:08:50\",\"result\":["
    "{\"idx\":\"4\",\"Name\":\"Cellar\",\"Type\":\"Humidity\",\"SubType\":\"LaCrosse TX3\",\"Humidity\":60,\"HumidityStatus\":\"Normal\",\"Data\":\"Humidity 60 %\",\"Favorite\":1}]}";
  CHECK(dz_model_apply(&f.model, third, strlen(third), DZ_COMMIT_PARTIAL, f.T, NULL, NULL) == DZ_OK);
  f.T += 1600;
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_clock_gauges(a) == 3 && shows(a, "60%"));
  finish(&f, a);

  /* no numeric favourite at all: the clock alone */
  a = start(&f, FIXTURE_DIR, APPFX_MOCK_NOFAV, 600000, true);
  ru_dzapp_show_section(a, RU_SEC_CLOCK);
  CHECK(ru_dzapp_clock_gauges(a) == 0 && ru_dzapp_home_rows(a) == 0 && ru_dzapp_clock_text(a, hm, dt) && !strcmp(hm, "12:00"));
  screen_ok(&s, a, "clock_none", size);

  /* the connection banner, and a pager that includes the clock */
  ru_dzapp_set_conn(a, DZ_CS_OFFLINE);
  f.T += 1000;
  ru_dzapp_ui_tick(a, f.T);
  CHECK(!strcmp(ru_dzapp_banner(a), "OFFLINE") && shows(a, "OFFLINE") && ru_dzapp_clock_text(a, hm, dt));
  screen_ok(&s, a, "clock_offline", size);
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_dzapp_ui_tick(a, f.T + 1000);
  CHECK(!shows(a, "OFFLINE"));
  finish(&f, a);
  sim_shutdown(&s);
}

/* ---- the start page: Clock by default, Favourites selectable; Home, back and the runtime's home follow it ---- */

static void start_page(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  CHECK(appfx_load(&f, FIXTURE_DIR, APPFX_REAL, 600000));
  ru_dzapp_t *a = ru_dzapp_create(&f.model, &f.client, true); /* no start() helper: this is the default */
  CHECK(a != NULL);
  ru_dzapp_set_display_info(a, "sim", !g_square);
  f.T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  CHECK(ru_dzapp_home_section(a) == RU_SEC_CLOCK && ru_dzapp_section(a) == RU_SEC_CLOCK);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_CLOCK && shows(a, "SAT 31 OCT"));
  /* Home is the Clock; back from Favourites (and from any page) lands on it; back on the Clock goes nowhere */
  ru_dzapp_show_section(a, RU_SEC_FAVS);
  CHECK(ru_dzapp_back(a) && ru_dzapp_section(a) == RU_SEC_CLOCK && ru_dzapp_view(a) == RU_VIEW_HOME);
  CHECK(!ru_dzapp_back(a) && ru_dzapp_section(a) == RU_SEC_CLOCK);
  ru_dzapp_show_section(a, RU_SEC_SETTINGS);
  ru_dzapp_home(a);
  CHECK(ru_dzapp_section(a) == RU_SEC_CLOCK);
  ru_dzapp_show_section(a, RU_SEC_FAVS);
  ru_dzapp_open_sub(a, RU_SUB_ABOUT);
  CHECK(ru_dzapp_back(a) && ru_dzapp_section(a) == RU_SEC_SETTINGS); /* sub -> Settings, then Home */
  CHECK(ru_dzapp_back(a) && ru_dzapp_section(a) == RU_SEC_CLOCK);
  /* the runtime's home app is the start page */
  ru_heap_t heap;
  ru_heap_init(&heap, 0);
  ru_runtime_t rt;
  ru_runtime_init(&rt, &heap, NULL, NULL);
  CHECK(ru_apps_register(&rt, a) == RU_OK);
  CHECK(ru_runtime_go_home(&rt) == RU_OK && ru_dzapp_section(a) == RU_SEC_CLOCK && !strcmp(ru_runtime_foreground(&rt)->id, RU_APP_ID_CLOCK));
  CHECK(ru_runtime_switch_to(&rt, RU_APP_ID_HOME) == RU_OK && ru_dzapp_section(a) == RU_SEC_FAVS);
  ru_runtime_shutdown(&rt);
  ru_apps_unbind();
  CHECK(heap.live == 0);

  /* Favourites as the start page */
  CHECK(!ru_dzapp_set_home_section(a, RU_SEC_LIGHTS) && !ru_dzapp_set_home_section(a, RU_SEC_COUNT) && ru_dzapp_home_section(a) == RU_SEC_CLOCK);
  CHECK(ru_dzapp_set_home_section(a, RU_SEC_FAVS) && ru_dzapp_home_section(a) == RU_SEC_FAVS);
  ru_dzapp_show_section(a, RU_SEC_CLOCK);
  CHECK(ru_dzapp_back(a) && ru_dzapp_section(a) == RU_SEC_FAVS); /* the Clock is now an ordinary page */
  CHECK(!ru_dzapp_back(a));
  ru_dzapp_show_section(a, RU_SEC_TEMP);
  ru_dzapp_home(a);
  CHECK(ru_dzapp_section(a) == RU_SEC_FAVS);
  ru_heap_init(&heap, 0);
  ru_runtime_init(&rt, &heap, NULL, NULL);
  CHECK(ru_apps_register(&rt, a) == RU_OK);
  CHECK(ru_runtime_go_home(&rt) == RU_OK && !strcmp(ru_runtime_foreground(&rt)->id, RU_APP_ID_HOME));
  ru_runtime_shutdown(&rt);
  ru_apps_unbind();
  ru_dzapp_destroy(a);
  appfx_unload(&f);
  sim_shutdown(&s);
}

/* ---- Settings: START PAGE and ROTATION ---- */

static struct { int n_start, n_rot, n_restart, last_start, last_rot; ru_pref_result_t rot_answer; } PH;
static ru_pref_result_t pref_cb(ru_pref_t p, int v, void *ud) {
  (void)ud;
  if (p == RU_PREF_START) { PH.n_start++; PH.last_start = v; return RU_PREF_APPLIED; }
  if (p == RU_PREF_RESTART) { PH.n_restart++; return RU_PREF_APPLIED; }
  PH.n_rot++;
  PH.last_rot = v;
  return PH.rot_answer;
}

static void settings_prefs(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  int c = size / 2;
  appfx_t f;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_MOCK, 600000, true);
  memset(&PH, 0, sizeof PH);
  ru_dzapp_set_pref_hook(a, pref_cb, NULL);
  ru_dzapp_show_section(a, RU_SEC_SETTINGS);

  /* seven rows, all visible without scrolling, every one inside the circle */
  CHECK(ru_dzapp_home_rows(a) == 7);
  screen_ok(&s, a, "settings", size);
  CHECK(shows(a, "START PAGE") && shows(a, "ROTATION") && shows(a, "CLOCK") && shows(a, "0\xC2\xB0"));
  lv_obj_t *first = NULL, *last = NULL;
  {
    lv_obj_t *scr = ru_dzapp_screen(a);
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
      lv_obj_t *ch = lv_obj_get_child(scr, (int32_t)i);
      if (lv_obj_get_child_count(ch) == 7 && !lv_obj_has_flag(ch, LV_OBJ_FLAG_SCROLLABLE)) { first = lv_obj_get_child(ch, 0); last = lv_obj_get_child(ch, 6); }
    }
  }
  CHECK(first != NULL && last != NULL);
  if (first && last) {
    lv_area_t a0, a6;
    lv_obj_get_coords(first, &a0);
    lv_obj_get_coords(last, &a6);
    int R = size / 2;
    int xs[2] = { a0.x1, a0.x2 }, ys[2] = { a0.y1, a6.y2 };
    int xl[2] = { a6.x1, a6.x2 };
    for (int i = 0; i < 2; i++) {
      CHECK((xs[i] - c) * (xs[i] - c) + (ys[0] - c) * (ys[0] - c) <= R * R);
      CHECK((xl[i] - c) * (xl[i] - c) + (ys[1] - c) * (ys[1] - c) <= R * R);
    }
  }

  /* START PAGE: a tap toggles at once, the hook stores, the row shows it */
  CHECK(ru_dzapp_home_section(a) == RU_SEC_FAVS); /* start() chose Favourites for the grid tests */
  CHECK(shows(a, "FAVS"));
  CHECK(ru_dzapp_tap_row(a, 3) && ru_dzapp_home_section(a) == RU_SEC_CLOCK && PH.n_start == 1 && PH.last_start == 0 && shows(a, "CLOCK"));
  CHECK(ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_SETTINGS);
  CHECK(ru_dzapp_tap_row(a, 3) && ru_dzapp_home_section(a) == RU_SEC_FAVS && PH.n_start == 2 && PH.last_start == 1 && shows(a, "FAVS"));
  ru_dzapp_home(a);
  CHECK(ru_dzapp_section(a) == RU_SEC_FAVS);
  ru_dzapp_show_section(a, RU_SEC_SETTINGS);

  /* ROTATION: a device that can only do 0 refuses, says so, and stores nothing */
  ru_dzapp_set_rotation_info(a, 0, 1u);
  CHECK(ru_dzapp_tap_row(a, 4) && ru_dzapp_view(a) == RU_VIEW_SUB && ru_dzapp_home_rows(a) == 4 && shows(a, "NOW 0\xC2\xB0"));
  screen_ok(&s, a, "rotation", size);
  ru_dzapp_pick_rotation(a, 90);
  CHECK(PH.n_rot == 0 && ru_dzapp_rotation(a) == 0 && !strcmp(ru_dzapp_rotation_note(a), "NOT SUPPORTED ON THIS PANEL") && shows(a, "NOT SUPPORTED"));
  screen_ok(&s, a, "rotation_refused", size);
  ru_dzapp_pick_rotation(a, 0); /* the one in force: nothing to do, and the note goes */
  CHECK(PH.n_rot == 0 && !ru_dzapp_rotation_note(a)[0]);
  CHECK(ru_dzapp_back(a) && ru_dzapp_section(a) == RU_SEC_SETTINGS);

  /* a platform that turns the screen at once */
  ru_dzapp_set_rotation_info(a, 0, 0xFu);
  PH.rot_answer = RU_PREF_APPLIED;
  ru_dzapp_open_sub(a, RU_SUB_ROTATION);
  sim_render();
  /* the chips sit in a compass layout: top 0, left 90, bottom 180, right 270 */
  click_at(c - ru_u(25), c); /* the 90 chip */
  CHECK(PH.n_rot == 1 && PH.last_rot == 90 && ru_dzapp_rotation(a) == 90 && ru_dzapp_view(a) == RU_VIEW_SUB && shows(a, "NOW 90\xC2\xB0"));
  click_at(c, c + ru_u(21)); /* 180 */
  CHECK(PH.n_rot == 2 && PH.last_rot == 180 && ru_dzapp_rotation(a) == 180);
  click_at(c + ru_u(25), c); /* 270 */
  CHECK(PH.last_rot == 270 && ru_dzapp_rotation(a) == 270);
  click_at(c, c - ru_u(21)); /* 0 */
  CHECK(PH.last_rot == 0 && ru_dzapp_rotation(a) == 0 && PH.n_rot == 4);
  ru_dzapp_back(a);
  CHECK(shows(a, "0\xC2\xB0") && ru_dzapp_section(a) == RU_SEC_SETTINGS);

  /* the platform answers: unsupported */
  PH.rot_answer = RU_PREF_UNSUPPORTED;
  ru_dzapp_open_sub(a, RU_SUB_ROTATION);
  ru_dzapp_pick_rotation(a, 180);
  CHECK(ru_dzapp_rotation(a) == 0 && ru_dzapp_view(a) == RU_VIEW_SUB && !strcmp(ru_dzapp_rotation_note(a), "NOT SUPPORTED ON THIS PANEL"));

  /* the platform stores it for the next start: a hold, "RESTART TO APPLY"; the rotation in force does not change */
  PH.rot_answer = RU_PREF_NEEDS_RESTART;
  ru_dzapp_open_sub(a, RU_SUB_ROTATION);
  ru_dzapp_pick_rotation(a, 90);
  CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM && !strcmp(ru_dzapp_confirm_verb(a), "RESTART TO APPLY") && shows(a, "RESTART TO APPLY") && ru_dzapp_rotation(a) == 0);
  screen_ok(&s, a, "rotation_restart", size);
  click_at(c, c + ru_u(13)); /* a short press does not restart */
  CHECK(PH.n_restart == 0 && ru_dzapp_view(a) == RU_VIEW_CONFIRM);
  CHECK(ru_dzapp_back(a) && ru_dzapp_view(a) == RU_VIEW_SUB && shows(a, "RESTART TO APPLY") && shows(a, "NOW 0\xC2\xB0")); /* cancel: still pending, offered again */
  CHECK(SPY.n == 0);
  sim_render();
  click_at(c, c + ru_u(33)); /* the line itself offers the hold again */
  CHECK(PH.n_restart == 0 && ru_dzapp_view(a) == RU_VIEW_CONFIRM && !strcmp(ru_dzapp_confirm_verb(a), "RESTART TO APPLY"));
  hold_confirm(size);
  CHECK(PH.n_restart == 1 && SPY.n == 0); /* the hold restarts the device; it sends no Domoticz command */
  finish(&f, a);

  /* no hook at all: the start page lives in memory, rotation is unsupported */
  a = start(&f, FIXTURE_DIR, APPFX_MOCK, 600000, true);
  ru_dzapp_show_section(a, RU_SEC_SETTINGS);
  CHECK(ru_dzapp_tap_row(a, 3) && ru_dzapp_home_section(a) == RU_SEC_CLOCK);
  ru_dzapp_open_sub(a, RU_SUB_ROTATION);
  ru_dzapp_pick_rotation(a, 90);
  CHECK(ru_dzapp_rotation(a) == 0 && ru_dzapp_rotation_note(a)[0]);
  finish(&f, a);
  sim_shutdown(&s);
}

/* ---- pages: order, membership, hidden sections, paging, swipes, return to the page ---- */

static const char *const SEC_NAMES[RU_SEC_COUNT] = { "CLOCK", "FAVOURITES", "LIGHTS", "SCENES", "TEMPERATURE", "WEATHER", "UTILITY", "ENERGY", "SETTINGS" };

/* the sections the pager walks, forward from Home, as a bit mask */
static unsigned walk_mask(ru_dzapp_t *a) {
  ru_dzapp_home(a);
  while (ru_dzapp_step_section(a, -1)) {} /* the first section, whatever the start page is */
  unsigned m = 1u << ru_dzapp_section(a);
  while (ru_dzapp_step_section(a, 1)) m |= 1u << ru_dzapp_section(a);
  ru_dzapp_home(a);
  return m;
}

static void page_model(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  const unsigned ALL = (1u << RU_SEC_COUNT) - 1;
  const unsigned BASE = 1u << RU_SEC_CLOCK | 1u << RU_SEC_FAVS | 1u << RU_SEC_SETTINGS;
  CHECK(!strcmp(ru_section_name(RU_SEC_FAVS), "FAVOURITES") && !strcmp(ru_section_name(RU_SEC_TEMP), "TEMPERATURE"));
  for (int i = 0; i < RU_SEC_COUNT; i++) CHECK(!strcmp(ru_section_name((ru_section_t)i), SEC_NAMES[i]));

  /* order and hide-when-empty: nothing at all, one light, the full mock */
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_EMPTY, 600000, true);
  CHECK(walk_mask(a) == BASE);
  const char *one = "{\"status\":\"OK\",\"result\":[{\"idx\":\"1\",\"Name\":\"L\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"Status\":\"Off\",\"Favorite\":0}]}";
  CHECK(dz_model_apply(&f.model, one, strlen(one), DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  CHECK(walk_mask(a) == (BASE | 1u << RU_SEC_LIGHTS));
  ru_dzapp_home(a);
  CHECK(shows(a, "NO FAVOURITES") && ru_dzapp_home_rows(a) == 0); /* not a favourite: Home stays empty */
  finish(&f, a);
  a = start(&f, FIXTURE_DIR, APPFX_MOCK, 600000, true);
  CHECK(walk_mask(a) == ALL);
  /* order of the walk is the Domoticz order */
  ru_dzapp_home(a);
  int last = 0;
  while (ru_dzapp_step_section(a, 1)) { CHECK((int)ru_dzapp_section(a) > last); last = (int)ru_dzapp_section(a); }
  CHECK(last == RU_SEC_SETTINGS);
  for (int i = 0; i < RU_SEC_COUNT; i++) {
    ru_dzapp_show_section(a, (ru_section_t)i);
    CHECK(i == RU_SEC_SETTINGS ? shows(a, "SETTINGS") : shows(a, SEC_NAMES[i]) || i == RU_SEC_ENERGY || i == RU_SEC_CLOCK); /* the clock has no title */
    screen_ok(&s, a, SEC_NAMES[i], size);
  }
  finish(&f, a);

  /* the Home grid: dashboard order scenes, lights, temperature, weather, utility, others; 6 tiles a page */
  a = start(&f, FIXTURE_DIR, APPFX_MOCK, 600000, true);
  static const struct { const char *idx; bool scene; } want[] = {
    {"1", true}, {"2", true}, {"3", true}, {"4", true}, {"1", false}, {"2", false}, {"3", false}, {"4", false}, {"5", false},
    {"6", false}, {"7", false}, {"11", false}, {"12", false}, {"30", false}, {"13", false}, {"14", false}, {"20", false}, {"10", false}};
  ru_dzapp_home(a);
  CHECK(ru_dzapp_pages(a) == 3 && ru_dzapp_page(a) == 0 && shows(a, "1/3"));
  size_t k = 0;
  for (int pg = 0; pg < 3; pg++) {
    CHECK(ru_dzapp_home_rows(a) == (size_t)(pg < 2 ? 6 : 6));
    ru_dzapp_row_info_t ri;
    for (size_t r = 0; ru_dzapp_row_info(a, r, &ri); r++, k++) {
      CHECK(k < sizeof want / sizeof want[0]);
      if (k < sizeof want / sizeof want[0]) CHECK(!strcmp(ri.idx, want[k].idx) && ri.scene == want[k].scene);
    }
    screen_ok(&s, a, "home page", size);
    if (pg < 2) CHECK(ru_dzapp_swipe(a, RU_DIR_UP, size / 2, size / 2));
  }
  CHECK(k == sizeof want / sizeof want[0]);
  CHECK(!ru_dzapp_swipe(a, RU_DIR_UP, size / 2, size / 2)); /* the last page is an end */
  CHECK(ru_dzapp_page(a) == 2 && shows(a, "3/3"));
  finish(&f, a);

  /* no favourites: the fallback text, no page indicator, no tiles */
  a = start(&f, FIXTURE_DIR, APPFX_MOCK_NOFAV, 600000, true);
  ru_dzapp_home(a);
  CHECK(shows(a, "NO FAVOURITES") && ru_dzapp_home_rows(a) == 0 && ru_dzapp_pages(a) == 1);
  CHECK(!ru_dzapp_swipe(a, RU_DIR_UP, size / 2, size / 2));
  screen_ok(&s, a, "nofav", size);
  finish(&f, a);

  /* classification decides membership: THB on Temperature AND Weather, a thermostat on no page (Home only) */
  const char *cls =
    "{\"status\":\"OK\",\"result\":["
    "{\"idx\":\"1\",\"Name\":\"THB\",\"Type\":\"Temp + Humidity + Baro\",\"SubType\":\"THB1 - BTHR918\",\"Temp\":20.0,\"Humidity\":50,\"Barometer\":1010,\"Data\":\"20.0 C, 50 %, 1010 hPa\",\"Favorite\":1},"
    "{\"idx\":\"2\",\"Name\":\"Thermo\",\"Type\":\"Thermostat\",\"SubType\":\"SetPoint\",\"SetPoint\":\"21.5\",\"Data\":\"21.5\",\"Favorite\":1},"
    "{\"idx\":\"3\",\"Name\":\"Lamp\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"Status\":\"On\",\"Favorite\":0},"
    "{\"idx\":\"4\",\"Name\":\"Meter\",\"Type\":\"General\",\"SubType\":\"kWh\",\"Data\":\"1.5 kWh\",\"Favorite\":0}]}";
  a = start(&f, FIXTURE_DIR, APPFX_EMPTY, 600000, true);
  CHECK(dz_model_apply(&f.model, cls, strlen(cls), DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  static const struct { ru_section_t sec; const char *in[2], *out[3]; } cm[] = {
    { RU_SEC_FAVS, {"1", "2"}, {"3", "4", NULL} },
    { RU_SEC_LIGHTS, {"3", NULL}, {"1", "2", "4"} },
    { RU_SEC_TEMP, {"1", NULL}, {"2", "3", "4"} },
    { RU_SEC_WEATHER, {"1", NULL}, {"2", "3", "4"} },
    { RU_SEC_UTILITY, {"4", NULL}, {"1", "2", "3"} },
  };
  for (size_t c = 0; c < sizeof cm / sizeof cm[0]; c++) {
    ru_dzapp_show_section(a, cm[c].sec);
    for (int i = 0; i < 2; i++) if (cm[c].in[i]) CHECK(row_of(a, cm[c].in[i]) >= 0);
    for (int i = 0; i < 3; i++) if (cm[c].out[i]) CHECK(row_of(a, cm[c].out[i]) < 0);
  }
  finish(&f, a);

  /* the order is dz_model_order, not the slot in the model: a second full response reorders, delta additions go last */
  a = start(&f, FIXTURE_DIR, APPFX_EMPTY, 600000, true);
  const char *o1 = "{\"status\":\"OK\",\"result\":["
    "{\"idx\":\"1\",\"Name\":\"A\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"Status\":\"Off\"},"
    "{\"idx\":\"2\",\"Name\":\"B\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"Status\":\"Off\"},"
    "{\"idx\":\"3\",\"Name\":\"C\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"Status\":\"Off\"}]}";
  const char *o2 = "{\"status\":\"OK\",\"result\":["
    "{\"idx\":\"3\",\"Name\":\"C\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"Status\":\"Off\"},"
    "{\"idx\":\"1\",\"Name\":\"A\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"Status\":\"Off\"},"
    "{\"idx\":\"2\",\"Name\":\"B\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"Status\":\"Off\"}]}";
  CHECK(dz_model_apply(&f.model, o1, strlen(o1), DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  ru_dzapp_row_info_t ri;
  CHECK(ru_dzapp_row_info(a, 0, &ri) && !strcmp(ri.idx, "1") && ru_dzapp_row_info(a, 2, &ri) && !strcmp(ri.idx, "3"));
  CHECK(dz_model_apply(&f.model, o2, strlen(o2), DZ_COMMIT_FULL_DEVICES, f.T + 10, NULL, NULL) == DZ_OK);
  ru_dzapp_ui_tick(a, f.T + 10);
  CHECK(ru_dzapp_row_info(a, 0, &ri) && !strcmp(ri.idx, "1")); /* a background reorder is rate limited (one rebuild per 2 s) */
  ru_dzapp_ui_tick(a, f.T + 2600);
  CHECK(ru_dzapp_row_info(a, 0, &ri) && !strcmp(ri.idx, "3") && ru_dzapp_row_info(a, 1, &ri) && !strcmp(ri.idx, "1") &&
        ru_dzapp_row_info(a, 2, &ri) && !strcmp(ri.idx, "2"));
  finish(&f, a);
  sim_shutdown(&s);
}

/* centre of the n-th tile (0..5) of the 3x2 grid, screen px */
static void tile_centre(int n, int size, int *x, int *y) {
  int c = size / 2, tw = ru_u(25), th = ru_u(21), gap = ru_u(1.5f);
  int col = n % 3, row = n / 3;
  *x = c - (3 * tw + 2 * gap) / 2 + col * (tw + gap) + tw / 2;
  *y = c + ru_u(-1.0f) - (2 * th + gap) / 2 + row * (th + gap) + th / 2;
}

static void paging(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  int c = size / 2;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_MANY, 600000, true);

  /* more than 64 devices: no cap. 100 lights = 17 pages of 6; the last page has the 4 left over */
  CHECK(ru_dzapp_section_available(a, RU_SEC_LIGHTS));
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  CHECK(ru_dzapp_pages(a) == 17 && ru_dzapp_page(a) == 0 && ru_dzapp_home_rows(a) == 6 && shows(a, "1/17") && shows(a, "LIGHTS"));
  ru_dzapp_row_info_t ri;
  CHECK(ru_dzapp_row_info(a, 0, &ri) && !strcmp(ri.idx, "1") && ru_dzapp_row_info(a, 5, &ri) && !strcmp(ri.idx, "6"));
  CHECK(!ru_dzapp_swipe(a, RU_DIR_DOWN, c, c)); /* the first page is an end */
  CHECK(ru_dzapp_swipe(a, RU_DIR_UP, c, c) && ru_dzapp_page(a) == 1 && shows(a, "2/17"));
  CHECK(ru_dzapp_row_info(a, 0, &ri) && !strcmp(ri.idx, "7"));
  CHECK(ru_dzapp_swipe(a, RU_DIR_DOWN, c, c) && ru_dzapp_page(a) == 0 && shows(a, "1/17"));
  /* the indicator is a button too: next page */
  /* a real tap on the n/N label, found in the tree */
  for (int i = 0; i < 16; i++) CHECK(ru_dzapp_swipe(a, RU_DIR_UP, c, c));
  CHECK(ru_dzapp_page(a) == 16 && ru_dzapp_home_rows(a) == 4 && shows(a, "17/17"));
  CHECK(ru_dzapp_row_info(a, 3, &ri) && !strcmp(ri.idx, "100"));
  screen_ok(&s, a, "last page", size);
  CHECK(!ru_dzapp_swipe(a, RU_DIR_UP, c, c) && ru_dzapp_page(a) == 16);
  /* objects per screen stay small: one page of tiles, never one per device */
  CHECK(ru_dzapp_home_rows(a) <= 8);

  /* tap -> detail -> back lands on the same section and page */
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  for (int i = 0; i < 16; i++) CHECK(ru_dzapp_swipe(a, RU_DIR_DOWN, c, c));
  for (int i = 0; i < 2; i++) CHECK(ru_dzapp_swipe(a, RU_DIR_UP, c, c));
  CHECK(ru_dzapp_page(a) == 2 && ru_dzapp_row_info(a, 1, &ri) && !strcmp(ri.idx, "14"));
  int tx, ty;
  tile_centre(1, size, &tx, &ty);
  sim_render();
  click_at(tx, ty);
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && !strcmp(ru_dzapp_current_idx(a), "14"));
  CHECK(ru_dzapp_back(a) && ru_dzapp_view(a) == RU_VIEW_HOME && ru_dzapp_section(a) == RU_SEC_LIGHTS && ru_dzapp_page(a) == 2);
  CHECK(ru_dzapp_row_info(a, 0, &ri) && !strcmp(ri.idx, "13"));
  /* leaving the section forgets the page; a new visit starts on page 1 */
  ru_dzapp_show_section(a, RU_SEC_TEMP);
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  CHECK(ru_dzapp_page(a) == 0);

  /* Temperature tiles: 30 sensors, 6 a page, 5 pages; the Utility page has the 20 meters */
  ru_dzapp_show_section(a, RU_SEC_TEMP);
  CHECK(ru_dzapp_pages(a) == 5 && ru_dzapp_home_rows(a) == 6 && shows(a, "1/5") && shows(a, "TEMP 101"));
  screen_ok(&s, a, "temp rows", size);
  ru_dzapp_show_section(a, RU_SEC_UTILITY);
  CHECK(ru_dzapp_pages(a) == 5 && shows(a, "1/5") && shows(a, "METER 131"));
  screen_ok(&s, a, "utility rows", size);
  /* Home: 8 favourites = 2 pages (6 + 2); a short last page is centred, nothing outside the circle */
  ru_dzapp_home(a);
  CHECK(ru_dzapp_pages(a) == 2 && ru_dzapp_home_rows(a) == 6);
  CHECK(ru_dzapp_swipe(a, RU_DIR_UP, c, c) && ru_dzapp_home_rows(a) == 2);
  screen_ok(&s, a, "home short page", size);
  /* the n/N indicator is a button */
  {
    ru_dzapp_home(a);
    sim_render();
    int ix = c, iy = c + ru_u(26.5f);
    click_at(ix, iy);
    CHECK(ru_dzapp_page(a) == 1 && shows(a, "2/2"));
    click_at(ix, iy);
    CHECK(ru_dzapp_page(a) == 0 && shows(a, "1/2")); /* round to the first */
  }
  finish(&f, a);
  sim_shutdown(&s);
}

static void swipes(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  int c = size / 2, R = size / 2;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_MOCK, 600000, true);
  appfx_gestures_attach(a, size, g_square);
  ru_dzapp_home(a);
  sim_render();

  /* finger left = next section, finger right = previous; the Clock has no previous (no wrap) */
  appfx_drag(c + 90, c - 100, c - 90, c - 100, 8);
  CHECK(ru_dzapp_section(a) == RU_SEC_LIGHTS);
  appfx_drag(c + 90, c - 100, c - 90, c - 100, 8);
  CHECK(ru_dzapp_section(a) == RU_SEC_SCENES); /* one step per swipe, never two */
  appfx_drag(c - 90, c - 100, c + 90, c - 100, 8);
  CHECK(ru_dzapp_section(a) == RU_SEC_LIGHTS);
  appfx_drag(c - 90, c - 100, c + 90, c - 100, 8);
  CHECK(ru_dzapp_section(a) == RU_SEC_FAVS);
  appfx_drag(c - 90, c - 100, c + 90, c - 100, 8);
  CHECK(ru_dzapp_section(a) == RU_SEC_CLOCK);
  appfx_drag(c - 90, c - 100, c + 90, c - 100, 8);
  CHECK(ru_dzapp_section(a) == RU_SEC_CLOCK); /* no wrap to Settings */
  /* to the last section and no further */
  for (int i = 0; i < 12; i++) appfx_drag(c + 90, c - 100, c - 90, c - 100, 8);
  CHECK(ru_dzapp_section(a) == RU_SEC_SETTINGS);

  /* vertical swipes: pages of a list (Home has 3 pages) */
  ru_dzapp_home(a);
  appfx_drag(c, c + 60, c, c - 60, 8);
  CHECK(ru_dzapp_section(a) == RU_SEC_FAVS && ru_dzapp_page(a) == 1);
  appfx_drag(c, c - 60, c, c + 60, 8);
  CHECK(ru_dzapp_page(a) == 0);
  appfx_drag(c, c - 60, c, c + 60, 8);
  CHECK(ru_dzapp_page(a) == 0 && ru_dzapp_section(a) == RU_SEC_FAVS);

  /* a drag that starts on a tile and ends inside it is a swipe, not a tap */
  int tx, ty;
  tile_centre(0, size, &tx, &ty);
  ru_dzapp_home(a);
  sim_render();
  appfx_drag(tx + 30, ty, tx - 30, ty, 6);
  CHECK(ru_dzapp_view(a) == RU_VIEW_HOME);
  ru_dzapp_home(a);

  /* a swipe that starts in the rim is not a content swipe: refused by ru_dzapp_swipe itself, even if forwarded */
  int rx = c + (int)(0.95f * (float)R);
  CHECK(!ru_dzapp_swipe(a, RU_DIR_LEFT, rx, c) && ru_dzapp_section(a) == RU_SEC_FAVS);
  CHECK(!ru_dzapp_swipe(a, RU_DIR_UP, c, c - (int)(0.92f * (float)R)));
  CHECK(ru_dzapp_swipe(a, RU_DIR_LEFT, c + (int)(0.80f * (float)R), c));
  ru_dzapp_home(a);

  /* the left rim swipe is BACK (the shell): from a section it goes Home ONCE, the section does not also step */
  ru_dzapp_show_section(a, RU_SEC_TEMP);
  sim_render();
  int lx = c - (int)(0.96f * (float)R);
  appfx_drag(lx, c, lx + 130, c, 8);
  CHECK(ru_dzapp_section(a) == RU_SEC_FAVS && ru_dzapp_view(a) == RU_VIEW_HOME);
  /* and on Home it does nothing at all */
  appfx_drag(lx, c, lx + 130, c, 8);
  CHECK(ru_dzapp_section(a) == RU_SEC_FAVS);

  /* a swipe on a detail steps to the next or previous device of the list (it never leaves the detail or the section); the hold screen
   * does not swipe */
  char idx[12];
  bool sc;
  CHECK(appfx_find(&f, DZ_CAP_ONOFF, -1, idx, &sc));
  ru_dzapp_show_section(a, RU_SEC_LIGHTS);
  ru_dzapp_open(a, idx, false);
  sim_render();
  {
    bool moved = ru_dzapp_swipe(a, RU_DIR_LEFT, c, c);
    CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && ru_dzapp_section(a) == RU_SEC_LIGHTS);
    CHECK(moved == (strcmp(ru_dzapp_current_idx(a), idx) != 0));
  }
  appfx_drag(c + 90, c - 100, c - 90, c - 100, 8);
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL && ru_dzapp_section(a) == RU_SEC_LIGHTS);
  CHECK(appfx_find(&f, DZ_CAP_SCENE, -1, idx, &sc));
  ru_dzapp_show_section(a, RU_SEC_SCENES);
  CHECK(ru_dzapp_tap_row(a, 0) && ru_dzapp_view(a) == RU_VIEW_CONFIRM);
  CHECK(!ru_dzapp_swipe(a, RU_DIR_LEFT, c, c) && !ru_dzapp_swipe(a, RU_DIR_UP, c, c) && ru_dzapp_view(a) == RU_VIEW_CONFIRM);
  finish(&f, a);
  sim_shutdown(&s);
}

int main(void) {
  static const struct { int size; bool square; } runs[3] = { { 466, false }, { 480, false }, { 480, true } };
  for (int i = 0; i < 3; i++) {
    int size = runs[i].size;
    g_square = runs[i].square;
    printf("== %d%s ==\n", size, sfx());
    all_screens(size);
    scene_and_group(size);
    feedback(size);
    honesty(size);
    sections(size);
    backend_path(size);
    clock_page(size);
    start_page(size);
    settings_prefs(size);
    page_model(size);
    paging(size);
    swipes(size);
    climate_units(size);
    climate_pages(size);
    climate_thermostat(size);
    nav_back_tests(size);
    back_double_fire(size);
    long_name_pill(size);
    switch_lights(size);
    climate_tiles_long(size);
  }
  MT_DONE();
}
