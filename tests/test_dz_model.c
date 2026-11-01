#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dz_model.h"
#include "mini_test.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "tests/fixtures"
#endif

#define CAP 4
static dz_entry_t ent[CAP];
static dz_device_t stg[CAP];
static dz_model_t M;

static void fresh(size_t cap_stage) {
  if (M.mu) dz_model_deinit(&M);
  dz_model_cfg_t cfg = {.ttl_ms = 10000, .old_update_s = 3600};
  CHECK(dz_model_init(&M, ent, CAP, stg, cap_stage, &cfg));
}

#define DEVS(...) "{\"ServerTime\":\"2026-10-31 12:00:00\",\"result\":[" __VA_ARGS__ "],\"status\":\"OK\"}"
#define SW(idx, st) "{\"idx\":\"" idx "\",\"Name\":\"n" idx "\",\"Type\":\"Light/Switch\",\"SwitchTypeVal\":0,\"Status\":\"" st "\"}"

static dz_err_t apply(const char *j, dz_commit_t mode, uint32_t now, int *ch) {
  return dz_model_apply(&M, j, strlen(j), mode, now, NULL, ch);
}

static void test_basic(void) {
  int ch;
  fresh(CAP);
  CHECK(apply(DEVS(SW("1", "Off") "," SW("2", "On")), DZ_COMMIT_FULL_DEVICES, 1000, &ch) == DZ_OK && ch == 2);
  CHECK(dz_model_count(&M) == 2 && dz_model_changes(&M) == 2 && dz_model_generation(&M) == 1);
  dz_device_t d; ru_val_t v; ru_val_state_t st;
  CHECK(dz_model_get(&M, "2", false, &d, &v, &st, 1000) && st == RU_VAL_OK && v.valid && v.v == 1.0f);
  CHECK(!dz_model_get(&M, "9", false, &d, &v, &st, 1000) && st == RU_VAL_UNKNOWN);
  /* identical re-read: no change */
  CHECK(apply(DEVS(SW("1", "Off") "," SW("2", "On")), DZ_COMMIT_FULL_DEVICES, 2000, &ch) == DZ_OK && ch == 0);
  CHECK(dz_model_generation(&M) == 1);
  /* change one */
  CHECK(apply(DEVS(SW("1", "On") "," SW("2", "On")), DZ_COMMIT_FULL_DEVICES, 3000, &ch) == DZ_OK && ch == 1);
  /* full read without idx 2 removes it */
  CHECK(apply(DEVS(SW("1", "On")), DZ_COMMIT_FULL_DEVICES, 4000, &ch) == DZ_OK && ch == 1 && dz_model_count(&M) == 1);
  CHECK(!dz_model_get(&M, "2", false, &d, &v, &st, 4000));
  dz_model_deinit(&M);
}

static void test_staging(void) {
  int ch;
  fresh(CAP);
  CHECK(apply(DEVS(SW("1", "On") "," SW("2", "On")), DZ_COMMIT_FULL_DEVICES, 1000, &ch) == DZ_OK);
  /* truncated: the callback fired for idx 1 before the failure was known */
  const char *bad = "{\"result\":[{\"idx\":\"1\",\"Name\":\"x\",\"Type\":\"Light/Switch\",\"SwitchTypeVal\":0,\"Status\":\"Off\"},{\"idx\":\"2\",\"Na";
  CHECK(apply(bad, DZ_COMMIT_FULL_DEVICES, 2000, &ch) == DZ_ERR_SYNTAX && ch == 0);
  dz_device_t d; ru_val_t v; ru_val_state_t st;
  CHECK(dz_model_get(&M, "1", false, &d, &v, &st, 2000) && !strcmp(d.status, "On"));
  CHECK(dz_model_count(&M) == 2 && M.aborts == 1);
  /* status ERR: well-formed but not OK */
  const char *err = "{\"result\":[" SW("1", "Off") "],\"status\":\"ERR\"}";
  CHECK(apply(err, DZ_COMMIT_FULL_DEVICES, 2000, &ch) == DZ_ERR_STATUS);
  CHECK(dz_model_get(&M, "1", false, &d, &v, &st, 2000) && !strcmp(d.status, "On"));
  /* HTML page */
  CHECK(apply("<html><body>login</body></html>", DZ_COMMIT_FULL_DEVICES, 2000, &ch) == DZ_ERR_SYNTAX);
  CHECK(dz_model_count(&M) == 2);
  dz_model_deinit(&M);
}

static void test_scenes_namespace(void) {
  int ch;
  fresh(CAP);
  CHECK(apply(DEVS(SW("5", "On")), DZ_COMMIT_FULL_DEVICES, 1000, &ch) == DZ_OK);
  const char *sc = DEVS("{\"idx\":\"5\",\"Name\":\"Scene\",\"Type\":\"Group\",\"Status\":\"Off\"}");
  CHECK(apply(sc, DZ_COMMIT_FULL_SCENES, 1000, &ch) == DZ_OK && ch == 1);
  dz_device_t d; ru_val_t v; ru_val_state_t st;
  CHECK(dz_model_count(&M) == 2);
  CHECK(dz_model_get(&M, "5", false, &d, &v, &st, 1000) && d.cap == DZ_CAP_ONOFF);
  CHECK(dz_model_get(&M, "5", true, &d, &v, &st, 1000) && d.cap == DZ_CAP_GROUP); /* Type "Group" */
  /* a device full read must not remove scene entries, and vice versa */
  CHECK(apply(DEVS(SW("5", "On")), DZ_COMMIT_FULL_DEVICES, 2000, &ch) == DZ_OK && dz_model_count(&M) == 2);
  CHECK(apply(DEVS(""), DZ_COMMIT_FULL_SCENES, 2000, &ch) == DZ_OK && dz_model_count(&M) == 1);
  dz_model_deinit(&M);
}

static void test_overflow(void) {
  int ch;
  fresh(CAP);
  const char *six = DEVS(SW("1", "On") "," SW("2", "On") "," SW("3", "On") "," SW("4", "On") "," SW("5", "On") "," SW("6", "On"));
  CHECK(apply(six, DZ_COMMIT_FULL_DEVICES, 1000, &ch) == DZ_OK);
  CHECK(dz_model_count(&M) == CAP && dz_model_overflow(&M) == 2);
  /* a read that overflowed the stage must never remove what it could not list */
  fresh(2);
  CHECK(apply(DEVS(SW("1", "On") "," SW("2", "On")), DZ_COMMIT_FULL_DEVICES, 1000, &ch) == DZ_OK && dz_model_count(&M) == 2);
  CHECK(apply(DEVS(SW("3", "On") "," SW("4", "On") "," SW("5", "On")), DZ_COMMIT_FULL_DEVICES, 2000, &ch) == DZ_OK);
  CHECK(dz_model_overflow(&M) >= 1);
  dz_device_t d; ru_val_t v; ru_val_state_t st;
  CHECK(dz_model_get(&M, "1", false, &d, &v, &st, 2000) || dz_model_count(&M) == CAP);
  dz_model_deinit(&M);
}

static void test_freshness(void) {
  int ch;
  fresh(CAP);
  const char *j = DEVS(
      "{\"idx\":\"1\",\"Name\":\"t\",\"Type\":\"Temp\",\"SubType\":\"x\",\"Temp\":21.5,\"HaveTimeout\":false,\"LastUpdate\":\"2026-10-31 11:59:00\"},"
      "{\"idx\":\"2\",\"Name\":\"t\",\"Type\":\"Temp\",\"SubType\":\"x\",\"Temp\":8.1,\"HaveTimeout\":true,\"LastUpdate\":\"2026-10-31 11:59:00\"},"
      "{\"idx\":\"3\",\"Name\":\"t\",\"Type\":\"Temp\",\"SubType\":\"x\",\"Temp\":3.0,\"LastUpdate\":\"2026-10-30 07:12:00\"},"
      "{\"idx\":\"4\",\"Name\":\"t\",\"Type\":\"Temp\",\"SubType\":\"x\",\"LastUpdate\":\"2026-10-31 11:59:00\"}");
  CHECK(apply(j, DZ_COMMIT_FULL_DEVICES, 1000, &ch) == DZ_OK);
  dz_device_t d; ru_val_t v; ru_val_state_t st;
  CHECK(dz_model_get(&M, "1", false, &d, &v, &st, 1000) && st == RU_VAL_OK && v.valid);
  CHECK_NEAR(v.v, 21.5, 1e-4);
  CHECK(dz_model_get(&M, "2", false, &d, &v, &st, 1000) && st == RU_VAL_STALE);   /* HaveTimeout */
  CHECK(ru_val_state(&v, 1000) == RU_VAL_STALE);                                   /* the ru_val_t itself says so */
  CHECK(dz_model_get(&M, "3", false, &d, &v, &st, 1000) && st == RU_VAL_STALE);   /* old LastUpdate */
  CHECK(dz_model_get(&M, "4", false, &d, &v, &st, 1000) && st == RU_VAL_UNKNOWN && !v.valid); /* missing, never 0 */
  /* server stops answering: everything ages to STALE after ttl */
  CHECK(dz_model_get(&M, "1", false, &d, &v, &st, 1000 + 10001) && st == RU_VAL_STALE);
  /* a delta poll that returns nothing still confirms unchanged devices */
  CHECK(apply("{\"ServerTime\":\"2026-10-31 12:00:30\",\"status\":\"OK\"}", DZ_COMMIT_DELTA, 9000, &ch) == DZ_OK && ch == 0);
  CHECK(dz_model_get(&M, "1", false, &d, &v, &st, 9000 + 5000) && st == RU_VAL_OK);
  CHECK(dz_model_get(&M, "2", false, &d, &v, &st, 9000) && st == RU_VAL_STALE);   /* timeout stays stale */
  /* LastUpdate ages with the server clock even while deltas stay empty */
  CHECK(dz_model_get(&M, "1", false, &d, &v, &st, 9000 + 3700 * 1000u) && st == RU_VAL_STALE);
  /* clock wrap safe */
  CHECK(apply(DEVS("{\"idx\":\"1\",\"Name\":\"t\",\"Type\":\"Temp\",\"SubType\":\"x\",\"Temp\":1.0}"), DZ_COMMIT_PARTIAL, 0xFFFFFF00u, &ch) == DZ_OK);
  CHECK(dz_model_get(&M, "1", false, &d, &v, &st, 100) && st == RU_VAL_OK);
  dz_model_deinit(&M);
}

static void test_values(void) {
  int ch;
  fresh(CAP);
  const char *j = DEVS(
      "{\"idx\":\"1\",\"Name\":\"d\",\"Type\":\"Color Switch\",\"SwitchTypeVal\":7,\"Status\":\"Off\",\"Level\":29,\"MaxDimLevel\":100},"
      "{\"idx\":\"2\",\"Name\":\"d\",\"Type\":\"Light/Switch\",\"SwitchTypeVal\":7,\"Status\":\"Set Level: 50 %\",\"Level\":50,\"MaxDimLevel\":100},"
      "{\"idx\":\"3\",\"Name\":\"p\",\"Type\":\"Usage\",\"Data\":\"5.15 Watt\"},"
      "{\"idx\":\"4\",\"Name\":\"p\",\"Type\":\"P1 Smart Meter\",\"SubType\":\"Energy\",\"SwitchTypeVal\":0,\"Data\":\"1;2;3;4;15;0\"}");
  CHECK(apply(j, DZ_COMMIT_FULL_DEVICES, 1, &ch) == DZ_OK);
  dz_device_t d; ru_val_t v; ru_val_state_t st;
  CHECK(dz_model_get(&M, "1", false, &d, &v, &st, 1) && v.valid && v.v == 0.0f); /* Off: stored Level 29 is not shown */
  CHECK(dz_model_get(&M, "2", false, &d, &v, &st, 1) && v.valid && v.v == 50.0f && v.unit == RU_UNIT_PCT);
  CHECK(dz_model_get(&M, "3", false, &d, &v, &st, 1) && v.valid && v.unit == RU_UNIT_W);
  CHECK(dz_model_get(&M, "4", false, &d, &v, &st, 1) && d.cap == DZ_CAP_ENERGY && !v.valid); /* unparsable: unknown, not 0 */
  dz_model_deinit(&M);
}

static void test_time(void) {
  int64_t a, b;
  CHECK(dz_time_parse("2026-10-31 12:00:00", &a) && dz_time_parse("2026-10-31 12:01:05", &b) && b - a == 65);
  CHECK(dz_time_parse("2026-03-01 00:00:00", &a) && dz_time_parse("2026-02-28 00:00:00", &b) && a - b == 86400);
  CHECK(!dz_time_parse("2026-13-31 12:00:00", &a) && !dz_time_parse("garbage", &a) && !dz_time_parse("", &a) && !dz_time_parse(NULL, &a));
  CHECK(!dz_time_parse("2026-10-31 12:00:0x", &a));
}


static void test_hostile(void) {
  static dz_entry_t e2[64];
  static dz_device_t s2[64];
  static dz_model_t m2;
  char path[256];
  snprintf(path, sizeof path, "%s/real_getdevices.json", FIXTURE_DIR);
  FILE *f = fopen(path, "rb");
  CHECK(f != NULL);
  if (!f) return;
  static char src[200000], mut[200000];
  size_t n = fread(src, 1, sizeof src - 1, f);
  fclose(f);
  src[n] = 0;
  dz_model_cfg_t cfg = {0};
  CHECK(dz_model_init(&m2, e2, 64, s2, 64, &cfg));
  CHECK(dz_model_apply(&m2, src, n, DZ_COMMIT_FULL_DEVICES, 1, NULL, NULL) == DZ_OK);
  size_t good = dz_model_count(&m2);
  uint32_t gen = dz_model_generation(&m2);
  CHECK(good > 20);
  unsigned seed = 12345;
  int syntax = 0, ok = 0;
  for (int it = 0; it < 1500; it++) {
    memcpy(mut, src, n);
    size_t len = n;
    seed = seed * 1103515245u + 12345u;
    int kind = (int)((seed >> 16) % 4);
    seed = seed * 1103515245u + 12345u;
    size_t pos = (seed >> 8) % n;
    if (kind == 0) len = pos;                                /* truncate anywhere */
    else if (kind == 1) { mut[pos] = (char)(seed >> 3); }     /* flip a byte */
    else if (kind == 2) { for (int k = 0; k < 8 && pos + (size_t)k < n; k++) mut[pos + (size_t)k] = (char)(seed >> k); }
    else { memmove(mut + pos, mut + pos + 1, n - pos - 1); len = n - 1; } /* delete a byte */
    dz_err_t e = dz_model_apply(&m2, mut, len, DZ_COMMIT_FULL_DEVICES, 1, NULL, NULL);
    if (e != DZ_OK) {
      syntax++;
      /* the whole point: a rejected response leaves the store exactly as it was */
      if (dz_model_count(&m2) != good || dz_model_generation(&m2) != gen) { CHECK(0); break; }
    } else {
      ok++;
      /* a lucky mutation may change a value; restore and carry on */
      dz_model_apply(&m2, src, n, DZ_COMMIT_FULL_DEVICES, 1, NULL, NULL);
      good = dz_model_count(&m2);
      gen = dz_model_generation(&m2);
    }
  }
  CHECK(syntax > 100);
  printf("hostile: %d rejected, %d accepted\n", syntax, ok);
  /* nesting bombs, giant numbers, lone surrogates, NULs */
  static char bomb[100000];
  size_t b = 0;
  b += (size_t)snprintf(bomb, sizeof bomb, "{\"result\":[{\"idx\":\"1\",\"x\":");
  for (int i = 0; i < 30000; i++) bomb[b++] = '[';
  bomb[b] = 0;
  CHECK(dz_model_apply(&m2, bomb, b, DZ_COMMIT_FULL_DEVICES, 1, NULL, NULL) == DZ_ERR_SYNTAX && dz_model_count(&m2) == good);
  const char *h[] = {
    "{\"result\":[{\"idx\":\"1\",\"Level\":1e999,\"MaxDimLevel\":-1,\"Name\":\"\\ud800\"}],\"status\":\"OK\"}",
    "{\"result\":[{\"idx\":\"99999999999999999999\"}],\"status\":\"OK\"}",
    "{\"result\":[{\"idx\":\"7\",\"Name\":\"a\\u0000b\",\"Temp\":\"NaN\",\"Type\":\"Temp\"}],\"status\":\"OK\"}",
    "{\"result\":\"not an array\",\"status\":\"OK\"}",
    "{\"status\":\"OK\",\"result\":[null,1,\"x\",[],{}]}",
    "", "{", "[]", "null", "\"status\"",
  };
  for (size_t i = 0; i < sizeof h / sizeof h[0]; i++) dz_model_apply(&m2, h[i], strlen(h[i]), DZ_COMMIT_PARTIAL, 1, NULL, NULL);
  CHECK(dz_model_apply(&m2, NULL, 0, DZ_COMMIT_PARTIAL, 1, NULL, NULL) == DZ_ERR_SYNTAX);
  dz_model_deinit(&m2);
}

int main(void) {
  test_hostile();
  test_basic();
  test_staging();
  test_scenes_namespace();
  test_overflow();
  test_freshness();
  test_values();
  test_time();
  MT_DONE();
}
