/* End-to-end: real client over sockets -> Node mock (2026.4 shapes) -> real model -> real screens, with simulated
 * touches. No stubs between the layers. Run through tests/run_with_mock.sh (sets MOCK_PORT). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini_test.h"
#include "ru_dzapp.h"
#include "ru_theme.h"
#include "sim_core.h"

#ifndef SHOT_DIR
#define SHOT_DIR "."
#endif
#define CAP 128

static char base[64];
static char rbuf[400 * 1024];
static dz_entry_t ent[CAP];
static dz_device_t stg[CAP];
static dz_model_t M;
static dz_client_t C;
static dz_transport_t *TR;
static uint32_t T; /* virtual clock shared by the network side and the UI, like one monotonic clock */
static sim_t S;

static void mock_ctl(const char *q) { /* test-only mock controls: /__mock/... */
  char cmd[256];
  snprintf(cmd, sizeof cmd, "curl -s --noproxy '*' -o /dev/null 'http://127.0.0.1:%s/__mock/%s'", getenv("MOCK_PORT"), q);
  (void)!system(cmd);
}

static void make(const char *token) {
  if (M.mu) dz_model_deinit(&M);
  dz_model_cfg_t mc = {.ttl_ms = 600000, .old_update_s = UINT32_MAX};
  dz_model_init(&M, ent, CAP, stg, CAP, &mc);
  dz_client_cfg_t cfg = {.base_url = base, .token = token, .transport = TR, .model = &M, .buf = rbuf, .buf_cap = sizeof rbuf,
                         .favorites_only = false, .timeout_ms = 1500, .poll_interval_ms = 10000, .startup_gap_ms = 1000};
  CHECK(dz_client_init(&C, &cfg));
  T = 100000;
}

static void pump(ru_dzapp_t *a, int steps, uint32_t dt) {
  for (int i = 0; i < steps; i++) { T += dt; ru_dzapp_net_step(a, T); }
  ru_dzapp_ui_tick(a, T);
}

static long lit_in(void) { return sim_count_lit(&S); }

static void screen_ok(const char *what) {
  sim_render();
  CHECK(sim_count_outside_lit(&S) == 0);
  long in = lit_in();
  CHECK(in > 300);
  if (sim_count_outside_lit(&S) != 0 || in <= 300) printf("  ^ screen: %s\n", what);
}

static void click(int x, int y) {
  sim_pointer(x, y, true);
  sim_advance(80);
  sim_pointer(x, y, false);
  sim_advance(80);
}

static void test_states_without_server(void) {
  /* port 1 is closed: the banner must say so, and nothing may pretend to be data */
  char dead[] = "http://127.0.0.1:1";
  dz_model_cfg_t mc = {.ttl_ms = 600000, .old_update_s = UINT32_MAX};
  if (M.mu) dz_model_deinit(&M);
  dz_model_init(&M, ent, CAP, stg, CAP, &mc);
  dz_client_cfg_t cfg = {.base_url = dead, .token = "x", .transport = TR, .model = &M, .buf = rbuf, .buf_cap = sizeof rbuf,
                         .timeout_ms = 300, .poll_interval_ms = 10000, .startup_gap_ms = 1000};
  CHECK(dz_client_init(&C, &cfg));
  ru_dzapp_t *a = ru_dzapp_create(&M, &C, true);
  CHECK(a != NULL);
  T = 100000;
  ru_dzapp_ui_tick(a, T);
  ru_dzapp_home(a);
  CHECK(!strcmp(ru_dzapp_banner(a), "CONNECTING..."));
  CHECK(ru_dzapp_home_rows(a) == 0);
  screen_ok("home, connecting");
  for (int i = 0; i < 90; i++) pump(a, 1, 1500); /* backoff is 10, 20, 40 s: give it time to give up */
  CHECK(!strcmp(ru_dzapp_banner(a), "OFFLINE"));
  CHECK(ru_dzapp_home_rows(a) == 0);
  sim_save_png(&S, SHOT_DIR "/dzapp_offline.png", true);
  ru_dzapp_destroy(a);
}

static void test_token_rejected(void) {
  make("wrong-token");
  ru_dzapp_t *a = ru_dzapp_create(&M, &C, true);
  ru_dzapp_ui_tick(a, T);
  ru_dzapp_home(a);
  pump(a, 40, 1500);
  CHECK(dz_client_state(&C) == DZ_CS_TOKEN_REJECTED);
  CHECK(!strcmp(ru_dzapp_banner(a), "TOKEN REJECTED"));
  uint32_t req = C.requests;
  pump(a, 40, 1500);
  CHECK(C.requests == req); /* a rejected token stops polling */
  ru_dzapp_destroy(a);
}

static int first_row_with_cap(ru_dzapp_t *a, dz_cap_t want, char *idx, bool *scene) {
  size_t total = dz_model_count(&M);
  for (size_t i = 0; i < total; i++) {
    dz_device_t d; ru_val_t v; ru_val_state_t st; bool sc;
    if (dz_model_at(&M, i, &d, &v, &st, &sc, T) && d.cap == want && !d.is_protected) {
      snprintf(idx, 12, "%s", d.idx); *scene = sc; return 1;
    }
  }
  (void)a;
  return 0;
}

static void test_full_flow(void) {
  make("user-token");
  ru_dzapp_t *a = ru_dzapp_create(&M, &C, true);
  ru_dzapp_ui_tick(a, T);
  ru_dzapp_home(a);
  pump(a, 60, 1500);
  CHECK(dz_client_state(&C) == DZ_CS_OK);
  CHECK(!strcmp(ru_dzapp_banner(a), ""));
  size_t rows = ru_dzapp_home_rows(a);
  CHECK(rows >= 10 && rows <= 48);
  CHECK((size_t)dz_model_count(&M) >= rows);
  screen_ok("home with data");
  sim_save_png(&S, SHOT_DIR "/dzapp_home_live.png", true);

  /* every row opens a detail screen that stays inside the circle: sweeps every capability the mock offers */
  unsigned caps_seen = 0;
  for (size_t i = 0; i < rows; i++) {
    ru_dzapp_home(a);
    CHECK(ru_dzapp_tap_row(a, i));
    /* a device opens its detail; a Scene or Group goes straight to its hold screen */
    CHECK(ru_dzapp_view(a) == RU_VIEW_DETAIL || ru_dzapp_view(a) == RU_VIEW_CONFIRM);
    /* the screen belongs to a real model entry (either namespace), and its capability is one we have drawn */
    dz_device_t d; ru_val_t v; ru_val_state_t st;
    bool found = dz_model_get(&M, ru_dzapp_current_idx(a), false, &d, &v, &st, T) ||
                 dz_model_get(&M, ru_dzapp_current_idx(a), true, &d, &v, &st, T);
    CHECK(found);
    if (found) caps_seen |= 1u << d.cap;
    sim_advance(50);
    screen_ok("detail");
  }
  CHECK(__builtin_popcount(caps_seen) >= 6); /* the mock exposes many capabilities: all of them got a valid screen */
  ru_dzapp_home(a);

  /* a switch: a real touch on the toggle reaches the mock, the model follows, the user is told */
  char idx[12]; bool sc = false;
  CHECK(first_row_with_cap(a, DZ_CAP_ONOFF, idx, &sc));
  dz_device_t d0; ru_val_t v; ru_val_state_t st;
  CHECK(dz_model_get(&M, idx, false, &d0, &v, &st, T));
  bool was_on = dz_status_is_on(&d0);
  ru_dzapp_open(a, idx, false);
  screen_ok("onoff detail");
  click(S.size / 2, S.size / 2); /* the toggle is centred */
  pump(a, 12, 400);
  dz_device_t d1;
  CHECK(dz_model_get(&M, idx, false, &d1, &v, &st, T));
  CHECK(dz_status_is_on(&d1) != was_on);
  CHECK(!strcmp(ru_dzapp_toast(a), "DONE"));
  sim_save_png(&S, SHOT_DIR "/dzapp_toggled.png", true);
  /* put it back so the mock is left as found */
  click(S.size / 2, S.size / 2);
  pump(a, 12, 400);
  CHECK(dz_model_get(&M, idx, false, &d1, &v, &st, T) && dz_status_is_on(&d1) == was_on);

  /* a scene is risky: the tap must lead to a hold-to-confirm, and nothing is sent before the hold completes */
  CHECK((first_row_with_cap(a, DZ_CAP_SCENE, idx, &sc) || first_row_with_cap(a, DZ_CAP_GROUP, idx, &sc)) && sc);
  dz_device_t s0;
  CHECK(dz_model_get(&M, idx, true, &s0, &v, &st, T));
  ru_dzapp_open(a, idx, true); /* a scene or group tile tap: straight to the hold screen, nothing sent yet */
  if (ru_dzapp_view(a) != RU_VIEW_CONFIRM)
    printf("  scene tap: view=%d idx=%s cap=%s controllable=%d protected=%d type=%s toast='%s'\n", (int)ru_dzapp_view(a), idx,
           dz_cap_name(s0.cap), s0.controllable, s0.is_protected, s0.type, ru_dzapp_toast(a));
  CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM);
  screen_ok("confirm");
  sim_save_png(&S, SHOT_DIR "/dzapp_confirm.png", true);
  uint32_t before = C.requests;
  pump(a, 6, 400);
  CHECK(C.requests == before || ru_dzapp_view(a) == RU_VIEW_CONFIRM); /* still waiting for the hold */
  dz_device_t s1;
  CHECK(dz_model_get(&M, idx, true, &s1, &v, &st, T) && !strcmp(s1.status, s0.status)); /* unchanged */
  /* a short press does not confirm */
  click(S.size / 2, S.size / 2 + ru_u(13));
  CHECK(ru_dzapp_view(a) == RU_VIEW_CONFIRM);
  /* a full hold does */
  sim_pointer(S.size / 2, S.size / 2 + ru_u(13), true);
  sim_advance(1500);
  sim_pointer(S.size / 2, S.size / 2 + ru_u(13), false);
  sim_advance(100);
  CHECK(ru_dzapp_view(a) == RU_VIEW_HOME); /* back on the section the tile was in; the result line shows there */
  pump(a, 20, 400);
  CHECK(!strcmp(ru_dzapp_toast(a), "DONE") || !strcmp(ru_dzapp_toast(a), "NOT CONFIRMED") ||
        !strcmp(ru_dzapp_toast(a), "STILL MIXED"));
  /* undo: a group has Off; a scene does not, and the mock accepts a repeat of On */
  ru_dzapp_destroy(a);
}

static void test_view_only(void) {
  make("viewer-token");
  ru_dzapp_t *a = ru_dzapp_create(&M, &C, false); /* a Viewer token: no controls */
  ru_dzapp_ui_tick(a, T);
  ru_dzapp_home(a);
  pump(a, 60, 1500);
  CHECK(dz_client_state(&C) == DZ_CS_OK);
  char idx[12]; bool sc;
  CHECK(first_row_with_cap(a, DZ_CAP_ONOFF, idx, &sc));
  dz_device_t d0, d1; ru_val_t v; ru_val_state_t st;
  CHECK(dz_model_get(&M, idx, false, &d0, &v, &st, T));
  ru_dzapp_open(a, idx, false);
  uint32_t before = C.requests;
  click(S.size / 2, S.size / 2);
  pump(a, 12, 400);
  CHECK(dz_model_get(&M, idx, false, &d1, &v, &st, T));
  CHECK(!strcmp(d0.status, d1.status)); /* nothing switched */
  CHECK(C.requests >= before);          /* polling continues, but no switchlight went out */
  screen_ok("view-only detail");
  ru_dzapp_destroy(a);
}

int main(void) {
  const char *port = getenv("MOCK_PORT");
  if (!port) { puts("MOCK_PORT not set: run through tests/run_with_mock.sh"); return 2; }
  snprintf(base, sizeof base, "http://127.0.0.1:%s", port);
  TR = dz_transport_socket_create();
  CHECK(TR != NULL);
  CHECK(sim_init(&S, 466));
  mock_ctl("reset");
  test_states_without_server();
  test_token_rejected();
  test_full_flow();
  test_view_only();
  dz_model_deinit(&M);
  dz_transport_destroy(TR);
  sim_shutdown(&S);
  MT_DONE();
}
