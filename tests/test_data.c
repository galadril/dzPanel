#include "mini_test.h"
#include "ru_data.h"
#include <string.h>

static const char *fmt(const ru_val_t *v) { static char b[32]; ru_val_format(v, b, sizeof b); return b; }

int main(void) {
  ru_val_t v;
  CHECK(ru_val_parse("1234 Watt", 100, 5000, &v) && v.valid && v.unit == RU_UNIT_W && v.v == 1234.0f);
  CHECK(ru_val_parse("  21.5 C ", 100, 5000, &v) && v.unit == RU_UNIT_C && v.v == 21.5f);
  CHECK(ru_val_parse("50 %", 0, 1, &v) && v.unit == RU_UNIT_PCT);
  CHECK(ru_val_parse("3.2 kWh", 0, 1, &v) && v.unit == RU_UNIT_KWH);
  CHECK(ru_val_parse("-120 Watt", 0, 1, &v) && v.v == -120.0f);
  CHECK(ru_val_parse("7", 0, 1, &v) && v.unit == RU_UNIT_NONE);
  /* rejects: result must be unknown, never 0 */
  const char *bad[] = {"", "   ", "Watt", "nan", "inf", "-inf", "0x10 W", "12abc", "12 parsecs", "1e999 W", "--", "1.2.3 W", NULL};
  for (int i = 0; bad[i]; i++) { CHECK(!ru_val_parse(bad[i], 0, 1, &v)); CHECK(!v.valid); }
  CHECK(!ru_val_parse(NULL, 0, 1, &v) && !v.valid);

  /* unknown never prints as 0 */
  ru_val_t u = ru_val_unknown();
  CHECK(strcmp(fmt(&u), "--") == 0);
  ru_val_t z = ru_val_make(0, RU_UNIT_W, 0, 1000);
  CHECK(strcmp(fmt(&z), "0 W") == 0);
  CHECK(strcmp(fmt(&z), "--") != 0);
  ru_val_t nan_v = ru_val_make(NAN, RU_UNIT_W, 0, 1000);
  CHECK(!nan_v.valid);

  /* freshness, including 32-bit wrap */
  ru_val_t f = ru_val_make(5, RU_UNIT_W, 1000, 500);
  CHECK(ru_val_state(&f, 1500) == RU_VAL_OK);
  CHECK(ru_val_state(&f, 1501) == RU_VAL_STALE);
  CHECK(ru_val_state(&u, 0) == RU_VAL_UNKNOWN);
  ru_val_t w = ru_val_make(5, RU_UNIT_W, 0xFFFFFF00u, 1000);
  CHECK(ru_val_state(&w, 0x100u) == RU_VAL_OK);   /* 512 ms elapsed across wrap */
  CHECK(ru_val_state(&w, 0x400u) == RU_VAL_STALE);

  /* formatting */
  ru_val_t t = ru_val_make(21.54f, RU_UNIT_C, 0, 1); CHECK(strcmp(fmt(&t), "21.5°C") == 0);
  t = ru_val_make(1234.4f, RU_UNIT_W, 0, 1); CHECK(strcmp(fmt(&t), "1234 W") == 0);
  char b[4]; ru_val_format(&t, b, sizeof b); CHECK(strlen(b) == 3); /* truncates, terminates */

  char a[24];
  ru_age_format(5000, a, sizeof a);       CHECK(strcmp(a, "just now") == 0);
  ru_age_format(12 * 60000, a, sizeof a); CHECK(strcmp(a, "12 min ago") == 0);
  ru_age_format(3 * 3600000u, a, sizeof a); CHECK(strcmp(a, "3 h ago") == 0);
  MT_DONE();
}
