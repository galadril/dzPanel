#include "ru_prefs.h"

#include <stdio.h>
#include <string.h>

void ru_prefs_defaults(ru_prefs_t *p) {
  p->rotation = 0;
  p->start = RU_START_CLOCK;
}

bool ru_prefs_rotation_valid(int deg) { return deg == 0 || deg == 90 || deg == 180 || deg == 270; }

void ru_prefs_load(ru_kv_t *kv, ru_prefs_t *out) {
  ru_prefs_defaults(out);
  if (!kv) return;
  char b[16];
  if (ru_kv_get_str(kv, RU_PREFS_NS, "rot", b, sizeof b) == RU_KV_OK) {
    if (!strcmp(b, "0")) out->rotation = 0;
    else if (!strcmp(b, "90")) out->rotation = 90;
    else if (!strcmp(b, "180")) out->rotation = 180;
    else if (!strcmp(b, "270")) out->rotation = 270;
  }
  if (ru_kv_get_str(kv, RU_PREFS_NS, "start", b, sizeof b) == RU_KV_OK && !strcmp(b, "favs")) out->start = RU_START_FAVS;
}

ru_kv_err_t ru_prefs_save_rotation(ru_kv_t *kv, int deg) {
  if (!kv || !ru_prefs_rotation_valid(deg)) return RU_KV_INVALID;
  char b[8];
  snprintf(b, sizeof b, "%d", deg);
  return ru_kv_set_str(kv, RU_PREFS_NS, "rot", b);
}

ru_kv_err_t ru_prefs_save_start(ru_kv_t *kv, ru_start_t s) {
  if (!kv || (s != RU_START_CLOCK && s != RU_START_FAVS)) return RU_KV_INVALID;
  return ru_kv_set_str(kv, RU_PREFS_NS, "start", s == RU_START_FAVS ? "favs" : "clock");
}

ru_kv_err_t ru_prefs_clear(ru_kv_t *kv) {
  if (!kv) return RU_KV_INVALID;
  ru_kv_err_t e = ru_kv_erase_ns(kv, RU_PREFS_NS);
  return e == RU_KV_NOT_FOUND ? RU_KV_OK : e;
}
