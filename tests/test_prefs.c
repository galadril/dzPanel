/* ru_prefs: defaults, round trip, invalid and missing values read as defaults, write failures, factory reset. */
#include <stdio.h>
#include <string.h>

#include "mini_test.h"
#include "ru_prefs.h"

static ru_kv_mem_t M;

int main(void) {
  ru_kv_mem_init(&M);
  ru_kv_t *kv = &M.base;
  ru_prefs_t p;

  /* nothing stored, or no store: the defaults */
  ru_prefs_load(kv, &p);
  CHECK(p.rotation == 0 && p.start == RU_START_CLOCK);
  p.rotation = 90; p.start = RU_START_FAVS;
  ru_prefs_load(NULL, &p);
  CHECK(p.rotation == 0 && p.start == RU_START_CLOCK);

  /* round trip, every angle and both start pages */
  static const int rots[] = {0, 90, 180, 270};
  for (size_t i = 0; i < 4; i++) {
    CHECK(ru_prefs_save_rotation(kv, rots[i]) == RU_KV_OK);
    ru_prefs_load(kv, &p);
    CHECK(p.rotation == rots[i]);
  }
  CHECK(ru_prefs_save_start(kv, RU_START_FAVS) == RU_KV_OK);
  ru_prefs_load(kv, &p);
  CHECK(p.start == RU_START_FAVS && p.rotation == 270); /* the two keys are independent */
  CHECK(ru_prefs_save_start(kv, RU_START_CLOCK) == RU_KV_OK);
  ru_prefs_load(kv, &p);
  CHECK(p.start == RU_START_CLOCK && p.rotation == 270);

  /* values that are not allowed are refused and nothing is written */
  size_t n = ru_kv_mem_count(&M, RU_PREFS_NS);
  CHECK(ru_prefs_save_rotation(kv, 45) == RU_KV_INVALID && ru_prefs_save_rotation(kv, -90) == RU_KV_INVALID &&
        ru_prefs_save_rotation(kv, 360) == RU_KV_INVALID && ru_prefs_save_rotation(NULL, 90) == RU_KV_INVALID);
  CHECK(ru_prefs_save_start(kv, (ru_start_t)7) == RU_KV_INVALID);
  CHECK(ru_kv_mem_count(&M, RU_PREFS_NS) == n);
  ru_prefs_load(kv, &p);
  CHECK(p.rotation == 270 && p.start == RU_START_CLOCK);

  /* corrupt stored values read as the defaults, never as a half-parsed number */
  static const char *const bad[] = {"", "45", "90x", " 90", "-90", "360", "ninety", "0090", "9999999999999999999"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    CHECK(ru_kv_set_str(kv, RU_PREFS_NS, "rot", bad[i]) == RU_KV_OK);
    CHECK(ru_kv_set_str(kv, RU_PREFS_NS, "start", bad[i]) == RU_KV_OK);
    ru_prefs_load(kv, &p);
    CHECK(p.rotation == 0 && p.start == RU_START_CLOCK);
  }
  CHECK(ru_kv_set_str(kv, RU_PREFS_NS, "start", "FAVS") == RU_KV_OK); /* case matters: not "favs" */
  ru_prefs_load(kv, &p);
  CHECK(p.start == RU_START_CLOCK);
  const unsigned char blob[] = {1, 2, 3}; /* a value of the wrong type */
  CHECK(ru_kv_set_blob(kv, RU_PREFS_NS, "rot", blob, sizeof blob) == RU_KV_OK);
  ru_prefs_load(kv, &p);
  CHECK(p.rotation == 0);

  /* a failing store: the error is reported, the stored value is unchanged */
  CHECK(ru_prefs_save_rotation(kv, 180) == RU_KV_OK);
  M.fail_sets_after = (int)M.set_calls; /* the next set fails */
  CHECK(ru_prefs_save_rotation(kv, 90) == RU_KV_IO);
  CHECK(ru_prefs_save_start(kv, RU_START_FAVS) == RU_KV_IO);
  M.fail_sets_after = -1;
  ru_prefs_load(kv, &p);
  CHECK(p.rotation == 180 && p.start == RU_START_CLOCK);

  /* a full store */
  ru_kv_mem_init(&M);
  char key[8];
  for (int i = 0; i < RU_KV_MEM_MAX_ENTRIES; i++) { snprintf(key, sizeof key, "k%d", i); CHECK(ru_kv_set_str(kv, "other", key, "v") == RU_KV_OK); }
  CHECK(ru_prefs_save_rotation(kv, 90) == RU_KV_FULL);
  ru_prefs_load(kv, &p);
  CHECK(p.rotation == 0);

  /* factory reset erases "ui" and nothing else */
  ru_kv_mem_init(&M);
  CHECK(ru_kv_set_str(kv, "wifi", "ssid", "x") == RU_KV_OK);
  CHECK(ru_prefs_save_rotation(kv, 90) == RU_KV_OK && ru_prefs_save_start(kv, RU_START_FAVS) == RU_KV_OK);
  CHECK(ru_prefs_clear(kv) == RU_KV_OK && ru_kv_mem_count(&M, RU_PREFS_NS) == 0 && ru_kv_mem_count(&M, "wifi") == 1);
  ru_prefs_load(kv, &p);
  CHECK(p.rotation == 0 && p.start == RU_START_CLOCK);
  CHECK(ru_prefs_clear(kv) == RU_KV_OK); /* nothing stored: still fine */
  CHECK(ru_prefs_clear(NULL) == RU_KV_INVALID);
  MT_DONE();
}
