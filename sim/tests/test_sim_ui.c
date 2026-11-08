/* UI tests: every demo screen at both sizes keeps all light inside the circle and has lit pixels;
 * widget behaviour (honest values, hold-to-confirm, toggle, selector, dimmer) with synthetic touch. */
#include "mini_test.h"
#include "sim_core.h"
#include "screens.h"
#include "ru_geom.h"
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

/* ---- rotation: LVGL stays upright, the output and the pointer turn ---- */

static int g_taps;
static void tap_cb(lv_obj_t *w, void *ud) { (void)w; (void)ud; g_taps++; }

/* the RGB bytes of a PNG written by sim_save_png (8 bit RGB, filter 0, stored deflate blocks): w*h*3 bytes, false on any surprise */
static bool read_png_rgb(const char *path, int w, int h, uint8_t *out) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc((size_t)n);
  uint8_t *z = malloc((size_t)n);
  bool ok = b && z && fread(b, 1, (size_t)n, f) == (size_t)n;
  fclose(f);
  size_t zn = 0, o = 8;
  while (ok && o + 12 <= (size_t)n) {
    uint32_t len = (uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | b[o + 3];
    if (!memcmp(b + o + 4, "IDAT", 4)) { memcpy(z + zn, b + o + 8, len); zn += len; }
    o += 12 + len;
  }
  size_t p = 2, rowb = (size_t)w * 3 + 1, got = 0;
  uint8_t *raw = malloc(rowb * (size_t)h);
  while (ok && raw && p + 5 <= zn && got < rowb * (size_t)h) {
    size_t len = z[p + 1] | (size_t)z[p + 2] << 8;
    if ((z[p] & 6) != 0 || got + len > rowb * (size_t)h) { ok = false; break; } /* not a stored block */
    memcpy(raw + got, z + p + 5, len);
    got += len;
    p += 5 + len;
  }
  ok = ok && raw && got == rowb * (size_t)h;
  for (int y = 0; ok && y < h; y++) {
    if (raw[(size_t)y * rowb] != 0) ok = false;
    else memcpy(out + (size_t)y * (size_t)w * 3, raw + (size_t)y * rowb + 1, (size_t)w * 3);
  }
  free(b); free(z); free(raw);
  return ok;
}

static void rotation(int size) {
  sim_t s;
  CHECK(sim_init_ex(&s, size, g_square));
  mkdir(SHOT_DIR, 0755);
  /* an asymmetric screen: a bright block near the top-left, a tile with a tap handler off to the right */
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, size, size);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_t *blk = lv_obj_create(scr);
  lv_obj_remove_style_all(blk);
  lv_obj_set_pos(blk, size * 3 / 10, size / 4);
  lv_obj_set_size(blk, size / 8, size / 12);
  lv_obj_set_style_bg_color(blk, lv_color_hex(0xFF8800), 0);
  lv_obj_set_style_bg_opa(blk, LV_OPA_COVER, 0);
  lv_obj_t *tile = ru_value_tile_create(scr, size * 3 / 10, size / 10, "TAP");
  lv_obj_set_pos(tile, size * 11 / 20, size * 11 / 20);
  ru_value_tile_set_text(tile, "OK", RU_VAL_OK);
  ru_value_tile_on_tap(tile, tap_cb, NULL);
  lv_screen_load(scr);
  sim_render();
  int tx, ty;
  centre_of(tile, &tx, &ty);
  uint32_t *base = malloc((size_t)size * (size_t)size * 4);
  memcpy(base, s.fb, (size_t)size * (size_t)size * 4);
  uint32_t *pan = malloc((size_t)size * (size_t)size * 4);
  uint8_t *rgb = malloc((size_t)size * (size_t)size * 3);
  CHECK(base && pan && rgb);
  CHECK(!sim_set_rotation(45) && !sim_set_rotation(-90) && !sim_set_rotation(360) && sim_rotation() == 0);
  static const int rots[4] = {0, 90, 180, 270};
  for (int r = 0; r < 4; r++) {
    CHECK(sim_set_rotation(rots[r]) && sim_rotation() == rots[r]);
    sim_render();
    /* LVGL did not turn: the logical framebuffer is the same, only the output differs */
    CHECK(!memcmp(base, s.fb, (size_t)size * (size_t)size * 4));
    /* the window image and the PNG are the framebuffer turned by ru_rot_to_panel, pixel for pixel, masked as before */
    sim_masked_copy(&s, pan);
    char p[256];
    snprintf(p, sizeof p, "%s/rotation_%d_r%d%s.png", SHOT_DIR, size, rots[r], g_square ? "sq" : "");
    CHECK(sim_save_png(&s, p, true));
    CHECK(read_png_rgb(p, size, size, rgb));
    long bad = 0, bad_png = 0, lit = 0;
    for (int y = 0; y < size; y++)
      for (int x = 0; x < size; x++) {
        int ox, oy;
        ru_rot_to_panel(rots[r], size, x, y, &ox, &oy);
        uint32_t want = (!g_square && sim_outside_circle(size, x, y)) ? 0 : (s.fb[y * size + x] & 0xFFFFFF);
        if (pan[oy * size + ox] != want) bad++;
        const uint8_t *q = &rgb[((size_t)oy * (size_t)size + (size_t)ox) * 3];
        if ((uint32_t)q[0] << 16 != (want & 0xFF0000) || (uint32_t)q[1] << 8 != (want & 0xFF00) || q[2] != (want & 0xFF)) bad_png++;
        if (want) lit++;
      }
    CHECK(bad == 0 && bad_png == 0 && lit > 100);
    /* the orange block is where the rotation puts it */
    {
      int lx = size * 3 / 10 + size / 16, ly = size / 4 + size / 24, ox, oy;
      ru_rot_to_panel(rots[r], size, lx, ly, &ox, &oy);
      const uint8_t *q = &rgb[((size_t)oy * (size_t)size + (size_t)ox) * 3];
      CHECK(q[0] == 0xFF && q[1] == 0x88 && q[2] == 0x00);
    }
    /* a finger at the PANEL position of the tile taps it; the same coordinates read as logical ones (rotation ignored) miss */
    int px, py;
    ru_rot_to_panel(rots[r], size, tx, ty, &px, &py);
    g_taps = 0;
    sim_pointer_panel(px, py, true); sim_advance(80); sim_pointer_panel(px, py, false); sim_advance(80);
    CHECK(g_taps == 1);
    if (rots[r] != 0 && (px != tx || py != ty)) {
      g_taps = 0;
      sim_pointer_panel(tx, ty, true); sim_advance(80); sim_pointer_panel(tx, ty, false); sim_advance(80);
      lv_area_t ar;
      lv_obj_get_coords(tile, &ar);
      int lx, ly;
      ru_rot_to_logical(rots[r], size, tx, ty, &lx, &ly);
      bool hit = lx >= ar.x1 && lx <= ar.x2 && ly >= ar.y1 && ly <= ar.y2;
      CHECK(g_taps == (hit ? 1 : 0));
      CHECK(!hit); /* the tile is placed so that no rotation maps its own centre onto itself */
    }
  }
  sim_set_rotation(0);
  free(base); free(pan); free(rgb);
  sim_shutdown(&s);
}

int main(void) {
  rotation(466);
  rotation(480);
  g_square = true;
  rotation(480);
  g_square = false;
  screens_at(466);
  screens_at(480);
  behaviour(466);
  behaviour(480);
  g_square = true;
  screens_at(480);
  behaviour(480);
  MT_DONE();
}
