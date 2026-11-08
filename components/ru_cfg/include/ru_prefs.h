/* ru_prefs: the user's interface preferences, in the "ui" namespace of the kv store. Not secrets, not Domoticz data.
 *   "rot"    screen rotation in degrees: "0", "90", "180" or "270"
 *   "start"  start page: "clock" or "favs"
 * A missing or invalid value reads as the default (rotation 0, Clock): a corrupt store never leaves the panel unusable.
 * Rotation is applied when the display is brought up, so a change needs a restart on the device. */
#pragma once
#include <stdbool.h>

#include "ru_kv.h"

#define RU_PREFS_NS "ui"

typedef enum { RU_START_CLOCK = 0, RU_START_FAVS } ru_start_t;
typedef struct {
  int rotation;      /* 0, 90, 180 or 270 */
  ru_start_t start;
} ru_prefs_t;

void ru_prefs_defaults(ru_prefs_t *p);
bool ru_prefs_rotation_valid(int deg);
/* Always fills out with valid values. kv NULL or unreadable = defaults. */
void ru_prefs_load(ru_kv_t *kv, ru_prefs_t *out);
/* RU_KV_INVALID for a value that is not allowed (nothing written); the store's error otherwise. */
ru_kv_err_t ru_prefs_save_rotation(ru_kv_t *kv, int deg);
ru_kv_err_t ru_prefs_save_start(ru_kv_t *kv, ru_start_t s);
/* Factory reset: forget every preference. */
ru_kv_err_t ru_prefs_clear(ru_kv_t *kv);
