#include "mini_test.h"
#include "ru_gesture.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#ifndef TRACE_DIR
#define TRACE_DIR "tests/traces/gesture"
#endif

static const char *dirname_(ru_dir_t d) { static const char *n[] = { "NONE", "LEFT", "RIGHT", "UP", "DOWN" }; return n[d]; }
static const char *actname(ru_edge_action_t a) { static const char *n[] = { "NONE", "BACK", "HOME", "QUICK", "CONTEXT" }; return n[a]; }
static const char *tname(ru_gtype_t t) {
  static const char *n[] = { "NONE", "TAP", "LONG_PRESS", "SWIPE", "EDGE_SWIPE", "DRAG_START", "DRAG_MOVE", "DRAG_END", "CANCEL" };
  return n[t];
}

typedef struct { char s[512]; int n; ru_gtype_t last; } log_t;
static void logev(log_t *l, const ru_gesture_event_t *e) {
  char tok[48];
  if (e->type == RU_G_DRAG_MOVE && l->last == RU_G_DRAG_MOVE) return; /* collapse */
  l->last = e->type;
  snprintf(tok, sizeof tok, "%s", tname(e->type));
  if (e->type == RU_G_EDGE_SWIPE) snprintf(tok + strlen(tok), 20, ":%s", actname(e->action));
  if (e->type == RU_G_SWIPE) snprintf(tok + strlen(tok), 20, ":%s", dirname_(e->dir));
  if (e->type == RU_G_LONG_PRESS && e->zone == RU_ZONE_RIM) snprintf(tok + strlen(tok), 20, ":%s", actname(e->action));
  if (e->zone == RU_ZONE_RIM && (e->type == RU_G_TAP || (e->type >= RU_G_DRAG_START && e->type <= RU_G_DRAG_END))) strcat(tok, "@RIM");
  if (l->n++) strcat(l->s, " ");
  strcat(l->s, tok);
}

static void feed(ru_gesture_t *g, log_t *l, const ru_touch_t *t) {
  ru_gesture_event_t ev[RU_GESTURE_MAX_EVENTS];
  int n = ru_gesture_tick(g, t->t_ms, ev, RU_GESTURE_MAX_EVENTS);
  for (int i = 0; i < n; i++) logev(l, &ev[i]);
  n = ru_gesture_feed(g, t, ev, RU_GESTURE_MAX_EVENTS);
  for (int i = 0; i < n; i++) logev(l, &ev[i]);
}

static int replay_file(const char *path, const char *name) {
  FILE *f = fopen(path, "r"); if (!f) return 0;
  char line[256], expect[512] = ""; int dia = 466; static ru_touch_t rows[4096]; int n = 0;
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, "# diameter:", 11)) dia = atoi(line + 11);
    else if (!strncmp(line, "# expect:", 9)) { char *p = line + 9; while (*p == ' ') p++; p[strcspn(p, "\r\n")] = 0; strcpy(expect, p); }
    else if (line[0] != '#') {
      unsigned t, dn, ct, sz; float x, y;
      if (sscanf(line, "%u,%f,%f,%u,%u,%u", &t, &x, &y, &dn, &ct, &sz) == 6 && n < 4096)
        rows[n++] = (ru_touch_t){ (int16_t)(x + (x < 0 ? -0.5f : 0.5f)), (int16_t)(y + (y < 0 ? -0.5f : 0.5f)), t, dn != 0, (uint8_t)ct, (uint16_t)sz };
    }
  }
  fclose(f);
  ru_gesture_t g; ru_gesture_init(&g, dia, NULL);
  log_t l; memset(&l, 0, sizeof l);
  for (int i = 0; i < n; i++) feed(&g, &l, &rows[i]);
  ru_gesture_event_t ev[4]; int k = ru_gesture_tick(&g, rows[n - 1].t_ms + 5000, ev, 4);
  for (int i = 0; i < k; i++) logev(&l, &ev[i]);
  int ok = !strcmp(l.s, expect);
  if (!ok) printf("trace %s: expected [%s] got [%s]\n", name, expect, l.s);
  return ok ? 1 : -1;
}

static void replay_all(void) {
  DIR *d = opendir(TRACE_DIR); CHECK(d != NULL); if (!d) return;
  struct dirent *e; int files = 0;
  while ((e = readdir(d))) {
    if (!strstr(e->d_name, ".csv")) continue;
    char p[512]; snprintf(p, sizeof p, "%s/%s", TRACE_DIR, e->d_name);
    int r = replay_file(p, e->d_name); files++; CHECK(r == 1);
  }
  closedir(d);
  CHECK(files >= 40);
  printf("replayed %d CSV traces\n", files);
}

/* ---- in-test synthetic generation ---- */
static unsigned rs = 7;
static float rnd(void) { rs = rs * 1664525u + 1013904223u; return (float)((rs >> 8) & 0xFFFF) / 65535.0f; }
static float rng(float a) { return (rnd() * 2 - 1) * a; }

static void tick_hold(ru_gesture_t *g, log_t *l, uint32_t t) {
  ru_gesture_event_t ev[4]; int n = ru_gesture_tick(g, t, ev, 4); for (int i = 0; i < n; i++) logev(l, &ev[i]);
}

static void tick_tests(int dia) {
  ru_gesture_t g; ru_gesture_init(&g, dia, NULL); log_t l; memset(&l, 0, sizeof l);
  int c = dia / 2;
  /* a single down sample then silence (controller reports only changes): long press via tick */
  feed(&g, &l, &(ru_touch_t){ (int16_t)c, (int16_t)c, 1000, true, 1, 8 });
  tick_hold(&g, &l, 1400); CHECK(l.n == 0);
  tick_hold(&g, &l, 1500); CHECK(!strcmp(l.s, "LONG_PRESS"));
  feed(&g, &l, &(ru_touch_t){ (int16_t)c, (int16_t)c, 1700, false, 0, 0 }); CHECK(l.n == 1); /* no TAP after long press */
  /* rim hold: needs 800 ms and gives HOME */
  memset(&l, 0, sizeof l);
  feed(&g, &l, &(ru_touch_t){ (int16_t)c, (int16_t)(dia - 6), 2000, true, 1, 8 });
  tick_hold(&g, &l, 2600); CHECK(l.n == 0);
  tick_hold(&g, &l, 2800); CHECK(!strcmp(l.s, "LONG_PRESS:HOME"));
  /* lost release mid drag: stale tick cancels, recogniser is usable again */
  memset(&l, 0, sizeof l);
  for (int i = 0; i < 6; i++) feed(&g, &l, &(ru_touch_t){ (int16_t)(c - 40 + i * 12), (int16_t)c, (uint32_t)(5000 + i * 10), true, 1, 8 });
  tick_hold(&g, &l, 5060 + 1600);
  CHECK(strstr(l.s, "CANCEL") != NULL);
  memset(&l, 0, sizeof l);
  feed(&g, &l, &(ru_touch_t){ (int16_t)c, (int16_t)c, 9000, true, 1, 8 });
  feed(&g, &l, &(ru_touch_t){ (int16_t)c, (int16_t)c, 9050, false, 0, 0 });
  CHECK(!strcmp(l.s, "TAP"));
  /* time going backwards is ignored, not a crash */
  feed(&g, &l, &(ru_touch_t){ (int16_t)c, (int16_t)c, 10000, true, 1, 8 });
  feed(&g, &l, &(ru_touch_t){ (int16_t)c, (int16_t)c, 9000, true, 1, 8 });
  CHECK(g.bad_samples == 1);
}

static void velocity_tests(int dia) {
  ru_gesture_t g; ru_gesture_init(&g, dia, NULL); int c = dia / 2;
  ru_gesture_event_t ev[4]; int n; float vx = 0;
  /* 600 px/s to the right: 4.8 px per 8 ms */
  for (int i = 0; i < 20; i++) ru_gesture_feed(&g, &(ru_touch_t){ (int16_t)(c - 50 + (int)(i * 4.8f)), (int16_t)c, (uint32_t)(100 + i * 8), true, 1, 8 }, ev, 4);
  n = ru_gesture_feed(&g, &(ru_touch_t){ 0, 0, 268, false, 0, 0 }, ev, 4);
  CHECK(n == 2 && ev[0].type == RU_G_DRAG_END && ev[1].type == RU_G_SWIPE && ev[1].dir == RU_DIR_RIGHT);
  vx = ev[0].vx; CHECK(vx > 450 && vx < 750); CHECK_NEAR(ev[0].vy, 0, 1);
  /* scaled screens agree on thresholds: same relative swipe works at 466 and 480 */
}

/* random noisy and adversarial: invariants only */
static void fuzz(int dia) {
  ru_gesture_t g; ru_gesture_init(&g, dia, NULL);
  ru_gesture_event_t ev[RU_GESTURE_MAX_EVENTS];
  int open_drag = 0, bad = 0; uint32_t t = 1000;
  for (int i = 0; i < 200000; i++) {
    ru_touch_t s;
    t += (uint32_t)(rnd() * 40);
    s.t_ms = t; s.down = rnd() < 0.9f; s.contacts = rnd() < 0.05f ? 3 : 1; s.size = rnd() < 0.02f ? 500 : 12;
    s.x = (int16_t)(rnd() * (dia + 40) - 20); s.y = (int16_t)(rnd() * (dia + 40) - 20);
    if (rnd() < 0.6f) { s.x = (int16_t)(dia / 2 + rng(60)); s.y = (int16_t)(dia / 2 + rng(60)); }
    int n = rnd() < 0.1f ? ru_gesture_tick(&g, t + (uint32_t)(rnd() * 3000), ev, RU_GESTURE_MAX_EVENTS) : ru_gesture_feed(&g, &s, ev, RU_GESTURE_MAX_EVENTS);
    if (n < 0 || n > RU_GESTURE_MAX_EVENTS) bad++;
    for (int k = 0; k < n; k++) {
      if (ev[k].type == RU_G_DRAG_START) open_drag++;
      if (ev[k].type == RU_G_DRAG_END || ev[k].type == RU_G_CANCEL) open_drag--;
      if (open_drag < 0 || open_drag > 1) bad++;
    }
  }
  CHECK(bad == 0);
}

/* jittery taps and holds never become drags; noisy swipes still swipe. Both diameters. */
static void noise_tests(int dia) {
  int c = dia / 2, taps = 0, swipes = 0, drags_on_tap = 0, N = 300;
  for (int k = 0; k < N; k++) {
    ru_gesture_t g; ru_gesture_init(&g, dia, NULL); log_t l; memset(&l, 0, sizeof l);
    float px = c + rng(80), py = c + rng(80); uint32_t t = 100;
    for (int i = 0; i < 12; i++, t += 8) feed(&g, &l, &(ru_touch_t){ (int16_t)(px + rng(3)), (int16_t)(py + rng(3)), t, true, 1, 8 });
    feed(&g, &l, &(ru_touch_t){ 0, 0, t, false, 0, 0 });
    if (!strcmp(l.s, "TAP")) taps++; else if (strstr(l.s, "DRAG")) drags_on_tap++;
    /* swipe, random direction among 4, with noise and random speed */
    memset(&l, 0, sizeof l); ru_gesture_reset(&g);
    int dir = k % 4; float len = 60 + rnd() * 60, ms = 100 + rnd() * 150;
    float ux = dir == 0 ? -1 : dir == 1 ? 1 : 0, uy = dir == 2 ? -1 : dir == 3 ? 1 : 0;
    int steps = (int)(ms / 8);
    for (int i = 0; i <= steps; i++) {
      float f = (float)i / steps;
      feed(&g, &l, &(ru_touch_t){ (int16_t)(c - ux * len / 2 + ux * len * f + rng(2.5f)), (int16_t)(c - uy * len / 2 + uy * len * f + rng(2.5f)), 5000 + (uint32_t)i * 8, true, 1, 8 });
    }
    feed(&g, &l, &(ru_touch_t){ 0, 0, 5000 + (uint32_t)(steps + 1) * 8, false, 0, 0 });
    static const char *want[] = { "SWIPE:LEFT", "SWIPE:RIGHT", "SWIPE:UP", "SWIPE:DOWN" };
    if (strstr(l.s, want[dir])) swipes++;
  }
  printf("noise d=%d: taps %d/%d, drags-on-tap %d, swipes %d/%d\n", dia, taps, N, drags_on_tap, swipes, N);
  CHECK(taps == N && drags_on_tap == 0);
  CHECK(swipes >= N * 95 / 100);
}

/* every direction of every sector, at several start angles, yields the right action */
static void sectors(int dia) {
  ru_gesture_t g; ru_gesture_init(&g, dia, NULL); float R = dia / 2.0f;
  struct { float b; ru_edge_action_t a; } cases[] = {
    { 0, RU_ACT_QUICK }, { 30, RU_ACT_QUICK }, { 330, RU_ACT_QUICK }, { 60, RU_ACT_CONTEXT }, { 90, RU_ACT_CONTEXT }, { 120, RU_ACT_CONTEXT },
    { 150, RU_ACT_HOME }, { 180, RU_ACT_HOME }, { 210, RU_ACT_HOME }, { 240, RU_ACT_BACK }, { 270, RU_ACT_BACK }, { 300, RU_ACT_BACK } };
  for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
    float a = cases[i].b * 3.14159265f / 180.0f, sx = sinf(a), sy = -cosf(a);
    log_t l; memset(&l, 0, sizeof l);
    for (int s = 0; s <= 12; s++) { /* from 0.96R inward along the radius, plus a little angular noise */
      float r = 0.96f * R - s * 8.0f;
      feed(&g, &l, &(ru_touch_t){ (int16_t)(R + r * sx + rng(1)), (int16_t)(R + r * sy + rng(1)), 100 + (uint32_t)s * 10, true, 1, 8 });
    }
    feed(&g, &l, &(ru_touch_t){ 0, 0, 300, false, 0, 0 });
    char want[40]; snprintf(want, sizeof want, "EDGE_SWIPE:%s", actname(cases[i].a));
    if (strcmp(l.s, want)) printf("sector %g: want %s got [%s]\n", cases[i].b, want, l.s);
    CHECK(!strcmp(l.s, want));
  }
}

/* just inside the annulus boundary is content (apps get it); just outside the circle is rejected */
static void boundaries(int dia) {
  ru_gesture_t g; ru_gesture_init(&g, dia, NULL); float R = dia / 2.0f;
  CHECK(ru_gesture_zone_of(&g, R, R - 0.89f * R) == RU_ZONE_CONTENT);
  CHECK(ru_gesture_zone_of(&g, R, R - 0.91f * R) == RU_ZONE_RIM);
  ru_gesture_event_t ev[4]; int n = 0;
  n += ru_gesture_feed(&g, &(ru_touch_t){ (int16_t)(R - 0.89f * R), (int16_t)R, 100, true, 1, 8 }, ev, 4);
  n += ru_gesture_feed(&g, &(ru_touch_t){ (int16_t)(R - 0.89f * R), (int16_t)R, 140, false, 0, 0 }, ev, 4);
  CHECK(n == 1 && ev[0].type == RU_G_TAP && ev[0].zone == RU_ZONE_CONTENT);
  n = ru_gesture_feed(&g, &(ru_touch_t){ (int16_t)(dia + 3), (int16_t)R, 200, true, 1, 8 }, ev, 4);
  n += ru_gesture_feed(&g, &(ru_touch_t){ (int16_t)(dia + 3), (int16_t)R, 240, false, 0, 0 }, ev, 4);
  CHECK(n == 0 && g.rejected_touches == 1);
}

int main(void) {
  replay_all();
  static const int dias[] = { 466, 480 };
  for (int i = 0; i < 2; i++) { tick_tests(dias[i]); velocity_tests(dias[i]); noise_tests(dias[i]); sectors(dias[i]); boundaries(dias[i]); fuzz(dias[i]); }
  MT_DONE();
}
