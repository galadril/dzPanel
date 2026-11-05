/* UI tests: every demo screen at both sizes keeps all light inside the circle and has lit pixels;
 * widget behaviour (honest values, hold-to-confirm, toggle, selector, dimmer) with synthetic touch. */
#include "mini_test.h"
#include "sim_core.h"
#include "screens.h"
#include "ru_widgets.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int g_cb_hold_ok, g_cb_hold_cancel, g_cb_tog, g_cb_tog_val, g_cb_sel, g_cb_dim;

static void hold_cb(lv_obj_t *w, ru_hold_event_t ev, void *ud) { (void)w; (void)ud; if (ev == RU_HOLD_CONFIRMED) g_cb_hold_ok++; else g_cb_hold_cancel++; }
static void tog_cb(lv_obj_t *w, bool on, void *ud) { (void)w; (void)ud; g_cb_tog++; g_cb_tog_val = on; }
static void sel_cb(lv_obj_t *w, int i, bool fin, void *ud) { (void)w; (void)fin; (void)ud; g_cb_sel = i; }
static void dim_cb(lv_obj_t *w, int v, bool fin, void *ud) { (void)w; (void)ud; if (fin) g_cb_dim = v; }

static void centre_of(lv_obj_t *o, int *x, int *y) {
  lv_area_t a;
  lv_obj_update_layout(o);
  lv_obj_get_coords(o, &a);
  *x = (a.x1 + a.x2 + 1) / 2;
  *y = (a.y1 + a.y2 + 1) / 2;
}

/* pixels clearly lit (any channel >= 0x40): the AMOLED power-relevant figure; raised surfaces (#0E1013) do not count */
static bool g_square; /* square panel run: PNGs are unmasked, layout check stays against the inscribed circle */

static long count_bright(const sim_t *s) {
  long n = 0;
  for (int i = 0; i < s->size * s->size; i++) {
    uint32_t p = s->fb[i];
    if (((p >> 16) & 0xFF) >= 0x40 || ((p >> 8) & 0xFF) >= 0x40 || (p & 0xFF) >= 0x40) n++;
  }
  return n;
}

static void screens_at(int size) {
  sim_t s;
  CHECK(sim_init_ex(&s, size, g_square));
  mkdir(SHOT_DIR, 0755);
  for (int i = 0; i < sim_screen_count; i++) {
    lv_obj_t *scr = sim_screen_build(sim_screen_names[i], 500000);
    CHECK(scr != NULL);
    if (!scr) continue;
    lv_screen_load(scr);
    sim_render();
    long out = sim_count_outside_lit(&s), in = sim_count_lit(&s);
    long area = (long)(3.14159f * (float)size * (float)size / 4);
    if (out || !in) printf("  %s@%d: outside-lit=%ld inside-lit=%ld\n", sim_screen_names[i], size, out, in);
    if (out) for (int yy = 0; yy < size; yy++) for (int xx = 0; xx < size; xx++) if ((s.fb[yy*size+xx]&0xFFFFFF) && sim_outside_circle(size, xx, yy)) { printf("   px %d,%d = %06x\n", xx, yy, s.fb[yy*size+xx]&0xFFFFFF); yy = size; break; }
    CHECK(out == 0);                 /* nothing outside the circle of radius R, unmasked */
    CHECK(in > 500);                 /* the screen is not blank */
    long br = count_bright(&s);
    printf("  %-10s@%d lit-in-circle=%ld bright=%.1f%%\n", sim_screen_names[i], size, in, 100.0 * (double)br / (double)area);
    CHECK(br < area * 12 / 100);     /* DESIGN.md: at most 12% lit on active screens */
    char p[256];
    snprintf(p, sizeof p, "%s/%s_%d%s.png", SHOT_DIR, sim_screen_names[i], size, g_square ? "sq" : "");
    CHECK(sim_save_png(&s, p, true));
    FILE *f = fopen(p, "rb");
    unsigned char sig[8] = { 0 };
    CHECK(f && fread(sig, 1, 8, f) == 8 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G');
    if (f) fclose(f);
    lv_obj_t *old = lv_screen_active();
    lv_screen_load(lv_obj_create(NULL));
    lv_obj_delete(old);
  }
  sim_shutdown(&s);
}

static void behaviour(int size) {
  g_cb_hold_ok = g_cb_hold_cancel = g_cb_tog = g_cb_tog_val = g_cb_sel = g_cb_dim = 0;
  sim_t s;
  CHECK(sim_init_ex(&s, size, g_square));
  lv_obj_t *scr = ru_screen_new();
  lv_screen_load(scr);
  const ru_theme_t *th = ru_theme();
  (void)th;

  /* value tile: unknown is "--" never 0; valid 0 is "0"; old value is STALE */
  lv_obj_t *tile = ru_value_tile_create(scr, ru_u(60), ru_u(12), "X");
  ru_val_t u = ru_val_unknown();
  ru_value_tile_set(tile, &u, 1000);
  CHECK(ru_value_tile_state(tile) == RU_VAL_UNKNOWN);
  CHECK(strcmp(lv_label_get_text(lv_obj_get_child(tile, 1)), "--") == 0);
  ru_val_t z = ru_val_make(0, RU_UNIT_W, 1000, 60000);
  ru_value_tile_set(tile, &z, 2000);
  CHECK(ru_value_tile_state(tile) == RU_VAL_OK);
  CHECK(strcmp(lv_label_get_text(lv_obj_get_child(tile, 1)), "0 W") == 0);
  ru_value_tile_set(tile, &z, 1000 + 61000);
  CHECK(ru_value_tile_state(tile) == RU_VAL_STALE);
  CHECK(!lv_obj_has_flag(lv_obj_get_child(tile, 2), LV_OBJ_FLAG_HIDDEN)); /* stale marker visible */
  lv_obj_delete(tile);

  /* hold to confirm */
  lv_obj_t *h = ru_hold_confirm_create(scr, ru_u(38), "HOLD", 1000);
  ru_hold_confirm_on_event(h, hold_cb, NULL);
  int hx, hy;
  centre_of(h, &hx, &hy);
  sim_pointer(hx, hy, true);
  sim_advance(500);
  CHECK(g_cb_hold_ok == 0);
  CHECK(ru_hold_confirm_progress(h) > 350 && ru_hold_confirm_progress(h) < 650);
  sim_pointer(hx, hy, false);
  sim_advance(50);
  CHECK(g_cb_hold_cancel == 1 && g_cb_hold_ok == 0 && ru_hold_confirm_progress(h) == 0);
  sim_pointer(hx, hy, true);
  sim_advance(1200);
  CHECK(g_cb_hold_ok == 1);
  sim_pointer(hx, hy, false);
  sim_advance(50);
  CHECK(g_cb_hold_ok == 1 && g_cb_hold_cancel == 1); /* release after confirm is not a cancel */
  lv_obj_delete(h);

  /* toggle: asks for the opposite state; never acts while UNKNOWN */
  lv_obj_t *tg = ru_toggle_create(scr, ru_u(50));
  ru_toggle_on_change(tg, tog_cb, NULL);
  ru_toggle_set(tg, RU_ONOFF_ON);
  centre_of(tg, &hx, &hy);
  sim_pointer(hx, hy, true); sim_advance(50); sim_pointer(hx, hy, false); sim_advance(50);
  CHECK(g_cb_tog == 1 && g_cb_tog_val == 0);
  ru_toggle_set(tg, RU_ONOFF_UNKNOWN);
  sim_pointer(hx, hy, true); sim_advance(50); sim_pointer(hx, hy, false); sim_advance(50);
  CHECK(g_cb_tog == 1);
  lv_obj_delete(tg);

  /* selector: touching the label angle picks that item */
  static const char *const names[] = { "OFF", "LOW", "MED", "HIGH" };
  lv_obj_t *sel = ru_selector_create(scr, ru_u(86), names, 4);
  ru_selector_on_change(sel, sel_cb, NULL);
  ru_selector_set(sel, 0);
  centre_of(sel, &hx, &hy);
  float r = (float)ru_u(86) / 2.0f - (float)ru_u(2.2f) - (float)ru_u(5.5f);
  float a = (170.0f + 200.0f * 3.0f / 3.0f) * 3.14159265f / 180.0f; /* last item */
  sim_pointer(hx + (int)(r * cosf(a)), hy + (int)(r * sinf(a)), true);
  sim_advance(50);
  sim_pointer(hx, hy, false);
  sim_advance(50);
  CHECK(ru_selector_get(sel) == 3 && g_cb_sel == 3);
  lv_obj_delete(sel);

  /* dimmer: touching the top of the ring is about half */
  lv_obj_t *dm = ru_dimmer_create(scr, ru_u(80), 100, "DIM");
  ru_dimmer_on_change(dm, dim_cb, NULL);
  ru_dimmer_set(dm, 10);
  centre_of(dm, &hx, &hy);
  int ry = hy - (ru_u(80) / 2 - ru_u(2.6f) / 2);
  sim_pointer(hx, ry, true); sim_advance(60); sim_pointer(hx, ry, false); sim_advance(60);
  CHECK(abs(ru_dimmer_get(dm) - 50) <= 6);
  CHECK(abs(g_cb_dim - 50) <= 6);
  ru_dimmer_set_unknown(dm);
  CHECK(strcmp(lv_label_get_text(lv_obj_get_child(dm, 5)), "--") == 0);
  lv_obj_delete(dm);

  sim_shutdown(&s);
}

int main(void) {
  screens_at(466);
  screens_at(480);
  behaviour(466);
  behaviour(480);
  g_square = true;
  screens_at(480);
  behaviour(480);
  MT_DONE();
}
