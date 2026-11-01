#include "ru_data.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const struct { const char *name; ru_unit_t unit; } k_units[] = {
  {"w", RU_UNIT_W}, {"watt", RU_UNIT_W}, {"kw", RU_UNIT_KW}, {"wh", RU_UNIT_WH}, {"kwh", RU_UNIT_KWH},
  {"c", RU_UNIT_C}, {"°c", RU_UNIT_C}, {"%", RU_UNIT_PCT}, {"v", RU_UNIT_V}, {"a", RU_UNIT_A},
  {"hpa", RU_UNIT_HPA}, {"", RU_UNIT_NONE},
};
static const char *k_sym[RU_UNIT_COUNT] = {"", "W", "kW", "Wh", "kWh", "°C", "%", "V", "A", "hPa"};

const char *ru_unit_symbol(ru_unit_t u) { return (unsigned)u < RU_UNIT_COUNT ? k_sym[u] : ""; }

ru_val_t ru_val_unknown(void) { return (ru_val_t){0}; }

ru_val_t ru_val_make(float v, ru_unit_t unit, uint32_t now_ms, uint32_t ttl_ms) {
  if (!isfinite(v)) return ru_val_unknown();
  return (ru_val_t){.v = v, .unit = unit, .valid = true, .ts_mono_ms = now_ms, .ttl_ms = ttl_ms};
}

uint32_t ru_val_age_ms(const ru_val_t *v, uint32_t now_ms) { return now_ms - v->ts_mono_ms; }

ru_val_state_t ru_val_state(const ru_val_t *v, uint32_t now_ms) {
  if (!v->valid) return RU_VAL_UNKNOWN;
  return ru_val_age_ms(v, now_ms) > v->ttl_ms ? RU_VAL_STALE : RU_VAL_OK;
}

bool ru_val_parse(const char *s, uint32_t now_ms, uint32_t ttl_ms, ru_val_t *out) {
  *out = ru_val_unknown();
  if (!s) return false;
  while (isspace((unsigned char)*s)) s++;
  /* strtod accepts "nan", "inf", hex floats: require a digit, sign or dot first, and no 'x'. */
  if (!(isdigit((unsigned char)*s) || *s == '-' || *s == '+' || *s == '.')) return false;
  const char *p = s;
  if (*p == '-' || *p == '+') p++;
  if (!(isdigit((unsigned char)*p) || *p == '.')) return false;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) return false;
  char *end;
  errno = 0;
  double d = strtod(s, &end);
  if (end == s || errno == ERANGE || !isfinite(d)) return false;
  while (isspace((unsigned char)*end)) end++;
  size_t len = strlen(end);
  while (len && isspace((unsigned char)end[len - 1])) len--;
  for (size_t i = 0; i < sizeof k_units / sizeof *k_units; i++) {
    if (strlen(k_units[i].name) == len && strncasecmp(k_units[i].name, end, len) == 0) {
      *out = ru_val_make((float)d, k_units[i].unit, now_ms, ttl_ms);
      return out->valid;
    }
  }
  return false;
}

int ru_val_format(const ru_val_t *v, char *buf, size_t n) {
  if (!buf || !n) return 0;
  if (!v->valid) return snprintf(buf, n, "--");
  float a = fabsf(v->v);
  const char *sym = ru_unit_symbol(v->unit);
  const char *sp = (v->unit == RU_UNIT_NONE || v->unit == RU_UNIT_PCT || v->unit == RU_UNIT_C) ? "" : " ";
  if (a >= 100.0f || v->v == floorf(v->v)) return snprintf(buf, n, "%.0f%s%s", v->v, sp, sym);
  return snprintf(buf, n, "%.1f%s%s", v->v, sp, sym);
}

int ru_age_format(uint32_t age_ms, char *buf, size_t n) {
  if (!buf || !n) return 0;
  uint32_t s = age_ms / 1000;
  if (s < 60) return snprintf(buf, n, "just now");
  if (s < 3600) return snprintf(buf, n, "%u min ago", (unsigned)(s / 60));
  if (s < 86400) return snprintf(buf, n, "%u h ago", (unsigned)(s / 3600));
  return snprintf(buf, n, "%u d ago", (unsigned)(s / 86400));
}
