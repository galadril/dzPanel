/* Honest, freshness-tagged values. See DESIGN.md section 7. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { RU_UNIT_NONE, RU_UNIT_W, RU_UNIT_KW, RU_UNIT_WH, RU_UNIT_KWH, RU_UNIT_C, RU_UNIT_PCT, RU_UNIT_V, RU_UNIT_A, RU_UNIT_HPA, RU_UNIT_COUNT } ru_unit_t;

typedef struct {
  float v;
  ru_unit_t unit;
  bool valid;          /* false = unknown, never rendered as 0 */
  uint32_t ts_mono_ms; /* monotonic, immune to clock jumps */
  uint32_t ttl_ms;     /* freshness window */
} ru_val_t;

typedef enum { RU_VAL_UNKNOWN, RU_VAL_OK, RU_VAL_STALE } ru_val_state_t;

ru_val_t ru_val_unknown(void);
ru_val_t ru_val_make(float v, ru_unit_t unit, uint32_t now_ms, uint32_t ttl_ms);
ru_val_state_t ru_val_state(const ru_val_t *v, uint32_t now_ms); /* wrap-safe */
uint32_t ru_val_age_ms(const ru_val_t *v, uint32_t now_ms);

const char *ru_unit_symbol(ru_unit_t u);

/* Parse strings like "1234 Watt", "21.5 C", "50 %", "3.2 kWh". Strict: whole string must match.
 * Returns false (and an unknown value) on empty, NaN/Inf, trailing junk or unknown unit. Never uses atof. */
bool ru_val_parse(const char *s, uint32_t now_ms, uint32_t ttl_ms, ru_val_t *out);

/* Writes "--" for unknown. Stale values still print their number (the widget dims them). Always NUL-terminates. */
int ru_val_format(const ru_val_t *v, char *buf, size_t n);
/* "12 min ago", "just now", "3 h ago", "2 d ago" */
int ru_age_format(uint32_t age_ms, char *buf, size_t n);
