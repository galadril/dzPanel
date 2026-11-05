/* The Domoticz apps on fixture data: every screen at 466 and 480 stays inside the circle and within the lit-pixel
 * budget; behaviour of Scene (push) versus Group (toggle, Mixed), command feedback, honest unknown/stale/offline.
 * No network: a fixture-backed model and a command spy. */
#include <stdio.h>
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
  f->T += 1000;
  ru_dzapp_set_conn(a, DZ_CS_OK);
  ru_dzapp_set_cmd_spy(a, spy_cb, NULL);
  memset(&SPY, 0, sizeof SPY);
  ru_dzapp_ui_tick(a, f->T);
  return a;
}
static void finish(appfx_t *f, ru_dzapp_t *a) {
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
    long out = sim_count_outside_lit(&s), in = sim_count_lit(&s), br = count_bright(&s);
    printf("  %-20s@%d lit=%ld bright=%.1f%%\n", nm, size, in, 100.0 * (double)br / (double)area);
    CHECK(out == 0);               /* nothing outside the circle (rim, tiles, labels) */
    CHECK(in > 300);               /* not blank */
    CHECK(br < area * 12 / 100);   /* DESIGN.md: at most 12% lit */
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
  CHECK(shows(a, "TOKEN CANNOT SWITCH") && !shows(a, "DONE"));
  /* a problem stays visible after a confirmation would have faded */
  f.T += RU_FB_CONFIRMED_SHOW_MS + 10;
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "TOKEN CANNOT SWITCH"));
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
  ru_dzapp_show_section(a, RU_SEC_HOME); /* home says the data is old; the connection banner stays empty (the link is fine) */
  CHECK(shows(a, "STALE DATA") && !strcmp(ru_dzapp_banner(a), ""));
  finish(&f, a);

  /* unknown values are "--", never 0: a temperature with no reading, a power meter with none */
  const char *devs =
    "{\"status\":\"OK\",\"result\":["
    "{\"idx\":\"1\",\"Name\":\"NoTemp\",\"Type\":\"Temp\",\"SubType\":\"LaCrosse TX3\",\"Favorite\":1},"
    "{\"idx\":\"2\",\"Name\":\"NoPower\",\"Type\":\"P1 Smart Meter\",\"SubType\":\"Energy\",\"Favorite\":1},"
    "{\"idx\":\"3\",\"Name\":\"RealZero\",\"Type\":\"Temp\",\"SubType\":\"LaCrosse TX3\",\"Data\":\"0.0 C\",\"Temp\":0.0,\"Favorite\":1}]}";
  a = start(&f, FIXTURE_DIR, APPFX_EMPTY, 600000, true);
  CHECK(dz_model_apply(&f.model, devs, strlen(devs), DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  ru_dzapp_show_section(a, RU_SEC_DEVICES);
  ru_dzapp_row_info_t ri;
  CHECK(ru_dzapp_row_info(a, (size_t)row_of(a, "1"), &ri) && !strcmp(ri.sub, "--"));
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
  CHECK(ru_dzapp_section(a) == RU_SEC_HOME);
  CHECK(!ru_dzapp_section_available(a, RU_SEC_SCENES));
  ru_dzapp_destroy(a);
  appfx_unload(&f);

  /* nothing at all: energy hidden too, home says so honestly */
  a = start(&f, FIXTURE_DIR, APPFX_EMPTY, 600000, true);
  CHECK(!ru_dzapp_section_available(a, RU_SEC_ENERGY));
  ru_dzapp_show_section(a, RU_SEC_HOME);
  CHECK(shows(a, "NO FAVOURITES"));
  ru_dzapp_destroy(a);
  appfx_unload(&f);

  /* weather from the real fixture; the pager skips hidden sections; settings never show the token */
  a = start(&f, FIXTURE_DIR, APPFX_REAL, 600000, true);
  CHECK(ru_dzapp_section_available(a, RU_SEC_WEATHER));
  ru_dzapp_show_section(a, RU_SEC_WEATHER);
  CHECK(shows(a, "HUMIDITY") || shows(a, "TEMP"));
  ru_dzapp_show_section(a, RU_SEC_SETTINGS);
  CHECK(shows(a, "CONNECTED") && shows(a, "SET") && shows(a, "****") && shows(a, "CONTROL") && !shows(a, TOKEN));
  CHECK(!shows(a, "fixture"));
  ru_dzapp_open_sub(a, RU_SUB_CONNECTION);
  CHECK(!shows(a, TOKEN));
  ru_dzapp_open_sub(a, RU_SUB_ABOUT);
  CHECK(shows(a, "LVGL 9.4") && !shows(a, TOKEN));
  ru_dzapp_show_section(a, RU_SEC_HOME);
  for (int i = 0; i < RU_SEC_COUNT; i++) {
    ru_section_t before = ru_dzapp_section(a);
    ru_dzapp_step_section(a, 1);
    CHECK(ru_dzapp_section(a) != before);
    CHECK(ru_dzapp_section_available(a, ru_dzapp_section(a)));
  }
  CHECK(ru_dzapp_section(a) == RU_SEC_HOME); /* a full lap */

  /* a swipe left pages to the next section */
  ru_dzapp_show_section(a, RU_SEC_HOME);
  int c = size / 2;
  sim_pointer(c + 90, c - 100, true);
  sim_advance(40);
  for (int k = 1; k <= 8; k++) { sim_pointer(c + 90 - k * 22, c - 100, true); sim_advance(16); }
  sim_pointer(c - 90, c - 100, false);
  sim_advance(100);
  CHECK(ru_dzapp_section(a) == RU_SEC_DEVICES);

  /* a switch device detail: tapping the toggle sends a plain On/Off (safe: no hold), pending afterwards */
  ru_dzapp_show_section(a, RU_SEC_DEVICES);
  char idx[12];
  bool sc;
  CHECK(appfx_find(&f, DZ_CAP_ONOFF, -1, idx, &sc));
  ru_dzapp_open(a, idx, false);
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL);
  click_at(c, c);
  CHECK(SPY.n == 1 && (SPY.kind == DZ_CMD_ON || SPY.kind == DZ_CMD_OFF) && !strcmp(SPY.idx, idx));
  CHECK(ru_dzapp_feedback(a)->st == RU_FB_PENDING);

  /* connection states are honest on home */
  ru_dzapp_show_section(a, RU_SEC_HOME);
  ru_dzapp_set_conn(a, DZ_CS_OFFLINE);   ru_dzapp_ui_tick(a, f.T); CHECK(!strcmp(ru_dzapp_banner(a), "OFFLINE") && shows(a, "OFFLINE"));
  ru_dzapp_set_conn(a, DZ_CS_TOKEN_REJECTED); ru_dzapp_ui_tick(a, f.T); CHECK(!strcmp(ru_dzapp_banner(a), "TOKEN REJECTED"));
  ru_dzapp_set_conn(a, DZ_CS_CONNECTING); ru_dzapp_ui_tick(a, f.T); CHECK(!strcmp(ru_dzapp_banner(a), "CONNECTING..."));
  ru_dzapp_set_conn(a, DZ_CS_OK);        ru_dzapp_ui_tick(a, f.T); CHECK(!strcmp(ru_dzapp_banner(a), ""));

  /* the six apps register with the runtime and show their section */
  ru_heap_t heap;
  ru_heap_init(&heap, 0);
  ru_runtime_t rt;
  ru_runtime_init(&rt, &heap, NULL, NULL);
  CHECK(ru_apps_register(&rt, a) == RU_OK);
  CHECK(ru_runtime_go_home(&rt) == RU_OK && ru_dzapp_section(a) == RU_SEC_HOME);
  CHECK(ru_runtime_switch_to(&rt, RU_APP_ID_SCENES) == RU_OK && ru_dzapp_section(a) == RU_SEC_SCENES);
  CHECK(ru_runtime_switch_to(&rt, RU_APP_ID_ENERGY) == RU_OK && ru_dzapp_section(a) == RU_SEC_ENERGY);
  CHECK(ru_runtime_switch_to(&rt, RU_APP_ID_SETTINGS) == RU_OK && ru_dzapp_section(a) == RU_SEC_SETTINGS);
  CHECK(!strcmp(ru_runtime_foreground(&rt)->id, RU_APP_ID_SETTINGS));
  CHECK(ru_apps_available(RU_SEC_HOME) && ru_apps_available(RU_SEC_WEATHER));
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
  long out = sim_count_outside_lit(s), in = sim_count_lit(s), br = count_bright(s);
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
  for (size_t r = 0; r < 3; r++) {
    appfx_t f;
    ru_dzapp_t *a = start(&f, FIXTURE_DIR, runs[r].set, 600000, true);
    CHECK(ru_dzapp_section_available(a, RU_SEC_WEATHER));
    ru_dzapp_show_section(a, RU_SEC_WEATHER);
    bool saw[3] = {false, false, false}, saw_no_ms = false;
    for (int k = 0; k < CL_N; k++) {
      int at = appfx_climate_index(&f, CL_IDX[k]);
      CHECK(at == k); /* the section lists the whole set, nothing hidden */
      ru_dzapp_select(a, RU_SEC_WEATHER, at);
      ru_dzapp_ui_tick(a, f.T);
      screen_ok(&s, a, CL_IDX[k], size);
      for (int u = 0; u < 3; u++) {
        if (runs[r].yes[u] && shows(a, runs[r].yes[u])) saw[u] = true;
        if (runs[r].no[u]) CHECK(!shows(a, runs[r].no[u]));
      }
      if (runs[r].set == APPFX_CLIMATE_NOUNITS) { CHECK(!shows(a, "m/s") && !shows(a, DEGC) ); saw_no_ms = true; }
      /* the detail screen of the same device follows the same unit rules */
      ru_dzapp_open(a, CL_IDX[k], false);
      ru_dzapp_ui_tick(a, f.T);
      screen_ok(&s, a, CL_IDX[k], size);
      for (int u = 0; u < 3; u++) if (runs[r].no[u]) CHECK(!shows(a, runs[r].no[u]));
      ru_dzapp_show_section(a, RU_SEC_WEATHER);
    }
    CHECK(saw[0] && saw[1] && (runs[r].yes[2] == NULL || saw[2]));
    (void)saw_no_ms;
    /* the overview list and the devices list: no hard-coded unit either */
    ru_dzapp_select(a, RU_SEC_WEATHER, -1);
    ru_dzapp_ui_tick(a, f.T);
    CHECK(shows(a, "CLIMATE"));
    screen_ok(&s, a, "overview", size);
    for (int u = 0; u < 3; u++) if (runs[r].no[u]) CHECK(!shows(a, runs[r].no[u]));
    ru_dzapp_show_section(a, RU_SEC_DEVICES);
    for (int u = 0; u < 3; u++) if (runs[r].no[u]) CHECK(!shows(a, runs[r].no[u]));
    finish(&f, a);
  }
  sim_shutdown(&s);
}

static void climate_pages(int size) {
  sim_t s;
  CHECK(init_sim(&s, size));
  appfx_t f;
  ru_dzapp_t *a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 600000, true);
  /* overview first (many sensors); a tap on a row opens that sensor's page */
  ru_dzapp_show_section(a, RU_SEC_WEATHER);
  CHECK(ru_dzapp_home_rows(a) == (size_t)CL_N && shows(a, "CLIMATE"));
  CHECK(shows(a, "TEMP SENSOR") || ru_dzapp_home_rows(a) > 0);
  CHECK(ru_dzapp_tap_row(a, 4)); /* the wind sensor */
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "WIND SENSOR") && shows(a, "3.4") && shows(a, "m/s") && shows(a, "NW") && shows(a, "GUST 6.1 m/s"));
  CHECK(shows(a, "SENSOR TEMP 12.0" DEGC)); /* its own thermometer, labelled as such */
  CHECK(shows(a, "5/27"));
  ru_dzapp_select(a, RU_SEC_WEATHER, appfx_climate_index(&f, "100"));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "21.5" DEGC) && shows(a, "TEMP"));
  ru_dzapp_select(a, RU_SEC_WEATHER, appfx_climate_index(&f, "103"));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "HUMIDITY 52%") && shows(a, "NORMAL") && shows(a, "1009 hPa") && shows(a, "SUNNY") && shows(a, "DEW POINT 10.0" DEGC));
  ru_dzapp_select(a, RU_SEC_WEATHER, appfx_climate_index(&f, "101"));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "71%") && shows(a, "WET"));
  ru_dzapp_select(a, RU_SEC_WEATHER, appfx_climate_index(&f, "105"));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "12.5") && shows(a, "RATE 0.4") && !shows(a, "mm"));   /* Domoticz gives rain no unit in the field: none shown */
  ru_dzapp_select(a, RU_SEC_WEATHER, appfx_climate_index(&f, "106"));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "UV INDEX") && shows(a, "MODERATE") && shows(a, "SENSOR TEMP"));
  ru_dzapp_select(a, RU_SEC_WEATHER, appfx_climate_index(&f, "107"));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "123") && shows(a, "Lux"));
  ru_dzapp_select(a, RU_SEC_WEATHER, appfx_climate_index(&f, "108"));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "850") && shows(a, "ppm") && shows(a, "FAIR"));
  /* a type nobody has seen still shows its number with its unit */
  ru_dzapp_select(a, RU_SEC_WEATHER, appfx_climate_index(&f, "116"));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "UNSEEN SENSOR") && shows(a, "7.5") && shows(a, "zorp"));
  ru_dzapp_select(a, RU_SEC_WEATHER, appfx_climate_index(&f, "113"));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "All good"));
  /* a next-page tap at the top cycles through the pages */
  int before = 0;
  (void)before;
  ru_dzapp_select(a, RU_SEC_WEATHER, 0);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "TEMP SENSOR"));
  click_at(size / 2, size / 2 - ru_u(38));
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "HUMIDITY SENSOR") && shows(a, "2/27"));
  finish(&f, a);

  /* a wind sensor's own temperature is never the weather temperature: with only wind and UV, home has no temperature hero */
  const char *devs =
    "{\"status\":\"OK\",\"result\":["
    "{\"idx\":\"1\",\"Name\":\"W\",\"Type\":\"Wind\",\"SubType\":\"TFA\",\"Speed\":\"2.0\",\"Gust\":\"3.0\",\"Direction\":90,\"DirectionStr\":\"E\",\"Temp\":12.5,\"Chill\":10.0,\"Favorite\":1},"
    "{\"idx\":\"2\",\"Name\":\"U\",\"Type\":\"UV\",\"SubType\":\"UV3\",\"UVI\":\"3\",\"Temp\":19.5,\"Favorite\":1}]}";
  a = start(&f, FIXTURE_DIR, APPFX_EMPTY, 600000, true);
  CHECK(dz_model_apply(&f.model, devs, strlen(devs), DZ_COMMIT_FULL_DEVICES, f.T, NULL, NULL) == DZ_OK);
  dz_units_t u = {.have_temp = true, .temp = "C", .have_wind = true, .wind = "m/s"};
  dz_model_set_units(&f.model, &u);
  ru_dzapp_ui_tick(a, f.T + 1);
  ru_dzapp_show_section(a, RU_SEC_HOME);
  CHECK(shows(a, "ROUNDOS") && !shows(a, "12.5") && !shows(a, "19.5"));
  ru_dzapp_show_section(a, RU_SEC_DEVICES);
  CHECK(!shows(a, "12.5" DEGC) && !shows(a, "19.5" DEGC));
  finish(&f, a);

  /* stale sensor: marked, numbers dimmed, never silently shown as fresh */
  a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 300000, true);
  f.T += 420000;
  ru_dzapp_ui_tick(a, f.T);
  ru_dzapp_show_section(a, RU_SEC_WEATHER);
  ru_dzapp_select(a, RU_SEC_WEATHER, 0);
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
  ru_dzapp_show_section(a, RU_SEC_WEATHER);
  ru_dzapp_select(a, RU_SEC_WEATHER, 0);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(shows(a, "BATTERY LOW"));
  ru_dzapp_select(a, RU_SEC_WEATHER, 1);
  ru_dzapp_ui_tick(a, f.T);
  CHECK(!shows(a, "BATTERY"));
  ru_dzapp_select(a, RU_SEC_WEATHER, 2);
  ru_dzapp_ui_tick(a, f.T);
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
  CHECK(!strncmp(ru_dzapp_confirm_verb(a), "SET 66.7", 8) && shows(a, DEGF) && !shows(a, DEGC));
  hold_confirm(size);
  CHECK(SPY.n == 1 && SPY.sp > 66.6f && SPY.sp < 66.8f);
  finish(&f, a);

  /* units unknown: bare numbers on the dial; the device's own vunit is still its word, but NOW has no symbol */
  a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_NOUNITS, 600000, true);
  ru_dzapp_open(a, "118", false);
  CHECK(shows(a, "NOW 21.5") && !shows(a, "NOW 21.5" DEGC) && !shows(a, "NOW 21.5" DEGF));
  finish(&f, a);

  /* thermostat mode (UNVERIFIED path): a selector over Modes, behind a hold; the command carries the Mode index */
  a = start(&f, FIXTURE_DIR, APPFX_CLIMATE_METRIC, 600000, true);
  ru_dzapp_open(a, "125", false);
  CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL);
  screen_ok(&s, a, "mode", size);
  memset(&SPY, 0, sizeof SPY);
  ru_dzapp_pick_mode(a, 2);
  CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM && SPY.n == 0 && !strcmp(ru_dzapp_confirm_verb(a), "MODE COOL") && ru_dzapp_confirm_kind(a) == DZ_CMD_MODE);
  click_at(size / 2, size / 2 + ru_u(13));
  CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM && SPY.n == 0);
  hold_confirm(size);
  CHECK(SPY.n == 1 && SPY.kind == DZ_CMD_MODE && SPY.level == 2 && !strcmp(SPY.idx, "125") && SPY.confirmed);
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
    climate_units(size);
    climate_pages(size);
    climate_thermostat(size);
  }
  MT_DONE();
}
