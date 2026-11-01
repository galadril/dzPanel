#include "dz_parse.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 8
#define VAL_CAP 128

typedef struct {
  const char *p, *end;
} sc_t;

typedef enum { V_STR, V_NUM, V_BOOL, V_NULL, V_SKIP } vt_t;
typedef struct {
  vt_t t;
  char s[VAL_CAP]; /* text of the value; for bool "true"/"false" */
} val_t;

static void ws(sc_t *s) {
  while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++;
}

static uint32_t hex4(sc_t *s) {
  uint32_t v = 0;
  if (s->end - s->p < 4) return UINT32_MAX;
  for (int i = 0; i < 4; i++) {
    char c = *s->p++;
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else return UINT32_MAX;
    v = v * 16 + d;
  }
  return v;
}

static size_t utf8_put(uint32_t cp, char *o) {
  if (cp < 0x80) { o[0] = (char)cp; return 1; }
  if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* Remove an incomplete UTF-8 sequence left at the end by truncation. */
static void utf8_trim(char *b, size_t n) {
  size_t i = n;
  while (i > 0 && ((unsigned char)b[i - 1] & 0xC0) == 0x80) i--;
  if (i > 0) {
    unsigned char lead = (unsigned char)b[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (need > 1 && n - (i - 1) < need) i--;
    else i = n;
  } else {
    i = 0;
  }
  b[i] = 0;
}

/* Parses a JSON string at s->p. Output is truncated to cap-1 bytes. out may be NULL (skip). */
static bool str(sc_t *s, char *out, size_t cap) {
  size_t n = 0;
  bool full = false;
  if (s->p >= s->end || *s->p != '"') return false;
  s->p++;
  while (s->p < s->end) {
    unsigned char c = (unsigned char)*s->p++;
    char tmp[4];
    size_t len;
    if (c == '"') {
      if (out && cap) { out[n] = 0; utf8_trim(out, n); }
      return true;
    }
    if (c < 0x20) return false;
    if (c == '\\') {
      uint32_t cp;
      if (s->p >= s->end) return false;
      char e = *s->p++;
      switch (e) {
        case '"': cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;
        case 'b': cp = 8; break;
        case 'f': cp = 12; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
          cp = hex4(s);
          if (cp == UINT32_MAX) return false;
          if (cp >= 0xD800 && cp < 0xDC00) {
            if (s->end - s->p >= 2 && s->p[0] == '\\' && s->p[1] == 'u') {
              s->p += 2;
              uint32_t lo = hex4(s);
              if (lo == UINT32_MAX) return false;
              cp = (lo >= 0xDC00 && lo < 0xE000) ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00) : 0xFFFD;
            } else {
              cp = 0xFFFD;
            }
          } else if ((cp >= 0xDC00 && cp < 0xE000) || cp == 0) {
            cp = 0xFFFD;
          }
          break;
        default: return false;
      }
      len = utf8_put(cp, tmp);
    } else {
      tmp[0] = (char)c;
      len = 1;
    }
    if (out && !full) {
      if (n + len < cap) { memcpy(out + n, tmp, len); n += len; }
      else full = true;
    }
  }
  return false; /* unterminated */
}

static bool is_num_char(char c) { return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E'; }

static bool lit(sc_t *s, const char *w) {
  size_t n = strlen(w);
  if ((size_t)(s->end - s->p) < n || memcmp(s->p, w, n) != 0) return false;
  s->p += n;
  return true;
}

static bool skip_value(sc_t *s, int depth);

static bool skip_container(sc_t *s, int depth, char close) {
  if (depth > MAX_DEPTH) return false;
  s->p++; /* open */
  ws(s);
  if (s->p < s->end && *s->p == close) { s->p++; return true; }
  for (;;) {
    ws(s);
    if (close == '}') {
      if (!str(s, NULL, 0)) return false;
      ws(s);
      if (s->p >= s->end || *s->p++ != ':') return false;
      ws(s);
    }
    if (!skip_value(s, depth + 1)) return false;
    ws(s);
    if (s->p >= s->end) return false;
    char c = *s->p++;
    if (c == close) return true;
    if (c != ',') return false;
  }
}

static bool skip_value(sc_t *s, int depth) {
  if (s->p >= s->end) return false;
  char c = *s->p;
  if (c == '"') return str(s, NULL, 0);
  if (c == '{') return skip_container(s, depth, '}');
  if (c == '[') return skip_container(s, depth, ']');
  if (c == 't') return lit(s, "true");
  if (c == 'f') return lit(s, "false");
  if (c == 'n') return lit(s, "null");
  if (!is_num_char(c)) return false;
  while (s->p < s->end && is_num_char(*s->p)) s->p++;
  return true;
}

/* Reads any value into v. Containers are skipped (V_SKIP). */
static bool read_value(sc_t *s, val_t *v, int depth) {
  v->s[0] = 0;
  if (s->p >= s->end) return false;
  char c = *s->p;
  if (c == '"') { v->t = V_STR; return str(s, v->s, sizeof v->s); }
  if (c == '{' || c == '[') { v->t = V_SKIP; return skip_value(s, depth); }
  if (c == 't') { v->t = V_BOOL; strcpy(v->s, "true"); return lit(s, "true"); }
  if (c == 'f') { v->t = V_BOOL; strcpy(v->s, "false"); return lit(s, "false"); }
  if (c == 'n') { v->t = V_NULL; return lit(s, "null"); }
  if (!is_num_char(c)) return false;
  v->t = V_NUM;
  size_t n = 0;
  while (s->p < s->end && is_num_char(*s->p)) {
    if (n + 1 >= sizeof v->s) return false;
    v->s[n++] = *s->p++;
  }
  v->s[n] = 0;
  return true;
}

/* Strict: whole string is one finite number. Never atof. */
static bool to_float(const char *t, float *out) {
  char *e;
  if (!*t) return false;
  float f = strtof(t, &e);
  if (*e || !isfinite(f)) return false;
  *out = f;
  return true;
}

static bool to_int(const char *t, int *out) {
  float f;
  if (!to_float(t, &f) || f < -1e9f || f > 1e9f) return false;
  *out = (int)f;
  return true;
}

static bool to_bool(const val_t *v) {
  if (v->t == V_BOOL || v->t == V_STR) return !strcmp(v->s, "true") || !strcmp(v->s, "1");
  if (v->t == V_NUM) { float f; return to_float(v->s, &f) && f != 0.0f; }
  return false;
}

static void copy(char *dst, size_t cap, const val_t *v) {
  if (v->t == V_STR || v->t == V_NUM) {
    size_t n = strlen(v->s);
    if (n >= cap) n = cap - 1;
    memcpy(dst, v->s, n);
    dst[n] = 0;
    utf8_trim(dst, n);
  }
}

/* "15 Watt", "7.397 kWh", "510 Liter", "18.5": a number, then an optional unit word. Strict about the number. */
static bool parse_qty(const char *t, float *out, char *unit, size_t ucap) {
  char *e;
  if (unit && ucap) unit[0] = 0;
  while (*t == ' ') t++;
  if (!*t) return false;
  {
    const char *q = t + ((*t == '-' || *t == '+') ? 1 : 0);
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) return false; /* strtof would read hex: Domoticz never sends it */
  }
  float f = strtof(t, &e);
  if (e == t || !isfinite(f)) return false;
  while (*e == ' ') e++;
  if (unit && ucap) {
    size_t n = 0;
    while (e[n] && e[n] != ' ' && e[n] != ';' && e[n] != ',' && n + 1 < ucap) { unit[n] = e[n]; n++; }
    unit[n] = 0;
    if (e[n] && e[n] != ' ' && e[n] != ';' && e[n] != ',') unit[0] = 0; /* unit too long: treat as unknown */
  }
  *out = f;
  return true;
}

/* Power in watts. Only an explicit W/Watt/kW unit counts; a bare number is not guessed. */
static bool parse_watts(const char *t, float *out) {
  float f;
  char u[8];
  if (!parse_qty(t, &f, u, sizeof u)) return false;
  if (!strcmp(u, "Watt") || !strcmp(u, "W")) { *out = f; return true; }
  if (!strcmp(u, "kW")) { *out = f * 1000.0f; return true; }
  return false;
}

static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

/* Decode into out as printable text. On any error out is empty. */
static void b64_decode(const char *in, char *out, size_t cap) {
  size_t n = 0;
  uint32_t acc = 0;
  int bits = 0;
  out[0] = 0;
  for (; *in && *in != '='; in++) {
    int v = b64v(*in);
    if (v < 0) { out[0] = 0; return; }
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      unsigned char b = (unsigned char)((acc >> bits) & 0xFF);
      if (n + 1 >= cap) break;
      out[n++] = (b < 0x20 || b == 0x7F) ? '?' : (char)b;
    }
  }
  out[n] = 0;
  utf8_trim(out, n);
}

static bool starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

static bool is_switch_family(const char *type) {
  return !strcmp(type, "Light/Switch") || !strcmp(type, "Color Switch") || starts(type, "Lighting");
}


static bool ieq(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
    if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
    if (x != y) return false;
  }
  return *a == *b;
}

static bool has_word_ci(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  for (; *hay; hay++) {
    size_t i = 0;
    while (i < n && hay[i]) {
      char x = hay[i], y = needle[i];
      if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
      if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
      if (x != y) break;
      i++;
    }
    if (i == n) return true;
  }
  return false;
}

const char *dz_rk_name(dz_rk_t k) {
  static const char *const n[] = {"none", "temp", "auxtemp", "hum", "baro", "dew", "wind", "gust", "dir", "chill", "rain",
                                  "rainrate", "uvi", "lux", "radiation", "visibility", "airq", "moisture", "distance",
                                  "percent", "power", "energy", "gas", "water", "voltage", "current", "weight", "number",
                                  "text"};
  return (unsigned)k < sizeof n / sizeof n[0] ? n[k] : "?";
}

const dz_reading_t *dz_dev_reading(const dz_device_t *d, dz_rk_t kind) {
  for (int i = 0; i < d->n_rd && i < DZ_MAX_READINGS; i++)
    if (d->rd[i].has && d->rd[i].kind == kind) return &d->rd[i];
  return NULL;
}
const dz_reading_t *dz_dev_room_temp(const dz_device_t *d) { return dz_dev_reading(d, DZ_RK_TEMP); }

bool dz_dev_in_plan(const dz_device_t *d, int plan) {
  if (plan <= 0) return false;
  for (int i = 0; i < d->n_plan_ids && i < DZ_MAX_DEV_PLANS; i++) if (d->plan_ids[i] == plan) return true;
  return false;
}

/* Add or replace the reading of a kind. Bounded: a ninth reading is dropped, never written past the array. */
static void add_rd(dz_device_t *d, dz_rk_t kind, float v, const char *unit) {
  int at = -1;
  for (int i = 0; i < d->n_rd; i++) if (d->rd[i].kind == kind) at = i;
  if (at < 0) {
    if (d->n_rd >= DZ_MAX_READINGS) return;
    at = d->n_rd++;
  }
  dz_reading_t *r = &d->rd[at];
  memset(r, 0, sizeof *r);
  r->kind = kind;
  r->has = true;
  r->v = v;
  if (unit) {
    size_t n = strlen(unit);
    if (n >= sizeof r->unit) n = 0; /* too long to be a unit: unknown, not cut */
    memcpy(r->unit, unit, n);
    r->unit[n] = 0;
  }
}

/* "0;Off;1;Heat;" -> indices and names */
static void parse_modes(dz_device_t *d, const char *t) {
  size_t used = 0;
  d->n_modes = 0;
  d->mode_names[0] = 0;
  while (*t && d->n_modes < DZ_MAX_MODES) {
    char *e;
    long idx = strtol(t, &e, 10);
    if (e == t || *e != ';' || idx < 0 || idx > 255) break;
    t = e + 1;
    const char *nm = t;
    size_t l = 0;
    while (t[l] && t[l] != ';') l++;
    if (l == 0) break;
    if (l > 15) l = 15;
    if (used + l + 2 > sizeof d->mode_names) break;
    if (d->n_modes) d->mode_names[used++] = '|';
    memcpy(d->mode_names + used, nm, l);
    used += l;
    d->mode_names[used] = 0;
    utf8_trim(d->mode_names, used);
    used = strlen(d->mode_names);
    d->mode_val[d->n_modes++] = (short)idx;
    t = nm;
    while (*t && *t != ';') t++;
    if (*t == ';') t++;
  }
}

/* The kind of a number that only has Data (General subtypes and unknown types): the SubType word first, then
 * the unit text. Never the device name. */
static dz_rk_t kind_for(const char *sub, const char *u, const char *type) {
  if (!strcmp(sub, "Visibility")) return DZ_RK_VISIBILITY;
  if (!strcmp(sub, "Solar Radiation")) return DZ_RK_RADIATION;
  if (!strcmp(sub, "Soil Moisture") || !strcmp(sub, "Leaf Wetness")) return DZ_RK_MOISTURE;
  if (!strcmp(sub, "Distance")) return DZ_RK_DISTANCE;
  if (!strcmp(sub, "Percentage")) return DZ_RK_PERCENT;
  if (!strcmp(sub, "Pressure") || !strcmp(sub, "Barometer")) return DZ_RK_BARO;
  if (!strcmp(sub, "Voltage")) return DZ_RK_VOLTAGE;
  if (!strcmp(sub, "Current")) return DZ_RK_CURRENT;
  if (!strcmp(sub, "Lux")) return DZ_RK_LUX;
  if (!strcmp(type, "Lux")) return DZ_RK_LUX;
  if (!strcmp(type, "Air Quality")) return DZ_RK_AIRQ;
  if (ieq(u, "lux")) return DZ_RK_LUX;
  if (ieq(u, "ppm") || ieq(u, "ppb")) return DZ_RK_AIRQ;
  if (ieq(u, "v") || ieq(u, "mv")) return DZ_RK_VOLTAGE;
  if (ieq(u, "a") || ieq(u, "ma")) return DZ_RK_CURRENT;
  if (ieq(u, "kwh") || ieq(u, "wh")) return DZ_RK_ENERGY;
  if (ieq(u, "w") || ieq(u, "watt") || ieq(u, "kw")) return DZ_RK_POWER;
  if (ieq(u, "w/m2") || ieq(u, "watt/m2")) return DZ_RK_RADIATION;
  if (ieq(u, "kg") || ieq(u, "g") || ieq(u, "lb") || ieq(u, "lbs")) return DZ_RK_WEIGHT;
  if (ieq(u, "%")) return DZ_RK_PERCENT;
  if (ieq(u, "hpa") || ieq(u, "bar") || ieq(u, "mbar") || ieq(u, "kpa") || ieq(u, "psi") || ieq(u, "inhg") || ieq(u, "mmhg"))
    return DZ_RK_BARO;
  if (ieq(u, "cb")) return DZ_RK_MOISTURE;
  if (ieq(u, "cm") || ieq(u, "in") || ieq(u, "mm") || ieq(u, "m") || ieq(u, "km") || ieq(u, "mi")) return DZ_RK_DISTANCE;
  if (has_word_ci(type, "gas") || has_word_ci(sub, "gas")) return DZ_RK_GAS;
  if (has_word_ci(type, "water") || has_word_ci(sub, "water")) return DZ_RK_WATER;
  return DZ_RK_NUMBER;
}

/* Readings for what has only Data: the number and unit it contains, or the text. */
static void data_reading(dz_device_t *d) {
  float f;
  char u[8];
  if (!d->data[0]) return;
  bool listy = strchr(d->data, ';') != NULL; /* P1 and other multi-value strings are not one number */
  if (!listy && parse_qty(d->data, &f, u, sizeof u)) {
    const char *unit = u[0] ? u : d->sensor_unit[0] ? d->sensor_unit : d->value_units[0] ? d->value_units : "";
    add_rd(d, kind_for(d->subtype, u[0] ? u : unit, d->type), f, unit);
  } else {
    add_rd(d, DZ_RK_TEXT, 0.0f, "");
  }
}

static dz_cap_t classify(const dz_device_t *d) {
  if (!strcmp(d->type, "Scene")) return DZ_CAP_SCENE; /* push only */
  if (!strcmp(d->type, "Group")) return DZ_CAP_GROUP; /* On / Off / Mixed */
  if (d->has_mode && d->n_modes > 0 && !is_switch_family(d->type)) return DZ_CAP_MODE;
  /* Setpoint, Thermostat 6, Radiator 1 and Evohome ("Heating") all carry SetPoint. Whether it may be CHANGED is
   * decided separately (can_set), so an unusual one still shows its value. */
  if (d->has_setpoint && !is_switch_family(d->type)) return DZ_CAP_THERMOSTAT;
  /* SwitchTypeVal is also set on sensors and meters (P1, RFXMeter, General) where it means something
   * else. Only trust it for switch-family types, or a sensor becomes a "control". */
  int stv = is_switch_family(d->type) ? d->switch_type : -1;
  switch (stv) {
    case 0: return DZ_CAP_ONOFF;
    case 7: return DZ_CAP_DIMMER;
    case 1: case 9: case 10: return DZ_CAP_PUSH;
    case 3: case 13: case 14: case 15: case 21: case 22: return DZ_CAP_BLINDS;
    case 18: return DZ_CAP_SELECTOR;
    case 19: case 20: return DZ_CAP_LOCK;
    case 2: case 5: case 8: case 11: case 12: return DZ_CAP_STATUS;
    case -1: break;
    default: return DZ_CAP_READONLY; /* siren, media, future types */
  }
  if (!strcmp(d->type, "Wind") || !strcmp(d->type, "Rain") || strstr(d->type, "Baro") || !strcmp(d->type, "UV") ||
      !strcmp(d->type, "Humidity") || !strcmp(d->type, "Lux") || !strcmp(d->type, "Air Quality"))
    return DZ_CAP_WEATHER;
  if (starts(d->type, "Temp")) return DZ_CAP_TEMP;
  if (!strcmp(d->type, "General") && (!strcmp(d->subtype, "kWh") || d->has_usage)) return DZ_CAP_ENERGY;
  if (!strcmp(d->type, "Usage") || !strcmp(d->type, "Energy") || starts(d->type, "P1 Smart Meter") ||
      !strcmp(d->type, "RFXMeter"))
    return DZ_CAP_ENERGY;
  return DZ_CAP_READONLY;
}

static void assign(dz_device_t *d, const char *k, const val_t *v) {
  float f;
  int i;
  if (v->t == V_SKIP || v->t == V_NULL) return;
  if (!strcmp(k, "idx")) copy(d->idx, sizeof d->idx, v);
  else if (!strcmp(k, "Name")) copy(d->name, sizeof d->name, v);
  else if (!strcmp(k, "Type")) copy(d->type, sizeof d->type, v);
  else if (!strcmp(k, "SubType")) copy(d->subtype, sizeof d->subtype, v);
  else if (!strcmp(k, "Data")) copy(d->data, sizeof d->data, v);
  else if (!strcmp(k, "Status")) copy(d->status, sizeof d->status, v);
  else if (!strcmp(k, "LastUpdate")) copy(d->last_update, sizeof d->last_update, v);
  else if (!strcmp(k, "LevelNames")) { if (v->t == V_STR) b64_decode(v->s, d->level_names, sizeof d->level_names); }
  else if (!strcmp(k, "SwitchTypeVal")) { if (to_int(v->s, &i) && i >= 0) d->switch_type = i; }
  else if (!strcmp(k, "Favorite")) d->favorite = to_bool(v);
  else if (!strcmp(k, "Protected")) d->is_protected = to_bool(v);
  else if (!strcmp(k, "HaveTimeout")) d->timeout = to_bool(v);
  else if (!strcmp(k, "Temp")) { if (to_float(v->s, &f)) { d->temp = f; d->has_temp = true; add_rd(d, DZ_RK_TEMP, f, ""); } }
  else if (!strcmp(k, "Humidity")) { if (to_float(v->s, &f)) { d->hum = f; d->has_hum = true; add_rd(d, DZ_RK_HUM, f, "%"); } }
  else if (!strcmp(k, "Barometer")) { if (to_float(v->s, &f)) { d->baro = f; d->has_baro = true; add_rd(d, DZ_RK_BARO, f, ""); } }
  else if (!strcmp(k, "DewPoint")) { if (to_float(v->s, &f)) add_rd(d, DZ_RK_DEW, f, ""); }
  else if (!strcmp(k, "Speed")) { if (to_float(v->s, &f)) add_rd(d, DZ_RK_WIND, f, ""); }
  else if (!strcmp(k, "Gust")) { if (to_float(v->s, &f)) add_rd(d, DZ_RK_GUST, f, ""); }
  else if (!strcmp(k, "Direction")) { if (to_float(v->s, &f) && f >= 0.0f && f <= 360.0f) add_rd(d, DZ_RK_DIR, f, ""); }
  else if (!strcmp(k, "Chill")) { if (to_float(v->s, &f)) add_rd(d, DZ_RK_CHILL, f, ""); }
  else if (!strcmp(k, "Rain")) { if (to_float(v->s, &f)) add_rd(d, DZ_RK_RAIN, f, ""); }
  else if (!strcmp(k, "RainRate")) { if (to_float(v->s, &f)) add_rd(d, DZ_RK_RAINRATE, f, ""); }
  else if (!strcmp(k, "UVI")) { if (to_float(v->s, &f)) add_rd(d, DZ_RK_UVI, f, ""); }
  else if (!strcmp(k, "trend")) { if (to_int(v->s, &i) && i >= DZ_TREND_STABLE && i <= DZ_TREND_DOWN) d->trend = i; }
  else if (!strcmp(k, "BatteryLevel")) { if (to_int(v->s, &i) && i >= 0 && i <= 100) { d->battery = i; d->has_battery = true; } }
  else if (!strcmp(k, "HumidityStatus")) copy(d->hum_status, sizeof d->hum_status, v);
  else if (!strcmp(k, "ForecastStr")) copy(d->forecast, sizeof d->forecast, v);
  else if (!strcmp(k, "DirectionStr")) copy(d->dir_str, sizeof d->dir_str, v);
  else if (!strcmp(k, "Quality")) copy(d->quality, sizeof d->quality, v);
  else if (!strcmp(k, "vunit")) copy(d->vunit, sizeof d->vunit, v);
  else if (!strcmp(k, "SensorUnit")) copy(d->sensor_unit, sizeof d->sensor_unit, v);
  else if (!strcmp(k, "ValueUnits")) copy(d->value_units, sizeof d->value_units, v);
  else if (!strcmp(k, "PlanID")) { if (to_int(v->s, &i) && i > 0 && !d->n_plan_ids) { d->plan_ids[0] = i; d->n_plan_ids = 1; } }
  else if (!strcmp(k, "Mode")) { if (to_int(v->s, &i) && i >= 0 && i <= 255) { d->mode = i; d->range_bits |= 8; } }
  else if (!strcmp(k, "Modes")) { if (v->t == V_STR) { parse_modes(d, v->s); if (d->n_modes) d->range_bits |= 16; } }
  else if (!strcmp(k, "min")) { if (to_float(v->s, &f)) { d->setpoint_min = f; d->range_bits |= 1; } }
  else if (!strcmp(k, "max")) { if (to_float(v->s, &f)) { d->setpoint_max = f; d->range_bits |= 2; } }
  else if (!strcmp(k, "step")) { if (to_float(v->s, &f)) { d->setpoint_step = f; d->range_bits |= 4; } }
  else if (!strcmp(k, "Usage")) { if (parse_watts(v->s, &f)) { d->usage_w = f; d->has_usage = true; } }
  else if (!strcmp(k, "UsageDeliv")) { if (parse_watts(v->s, &f)) { d->usage_deliv_w = f; d->has_usage_deliv = true; } }
  else if (!strcmp(k, "CounterToday")) { if (parse_qty(v->s, &f, d->today_unit, sizeof d->today_unit)) { d->today = f; d->has_today = true; } }
  else if (!strcmp(k, "SetPoint")) { if (to_float(v->s, &f)) { d->setpoint = f; d->has_setpoint = true; } }
  else if (!strcmp(k, "Level")) { if (to_int(v->s, &i)) { d->level = i; d->has_level = true; } }
  else if (!strcmp(k, "LevelInt")) { if (to_int(v->s, &i)) d->level_int = i; }
  else if (!strcmp(k, "MaxDimLevel")) { if (to_int(v->s, &i) && i > 0) d->max_level = i; }
}

static bool valid_idx(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n > 9) return false;
  for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

/* PlanIDs: [1,2]. Keeps up to DZ_MAX_DEV_PLANS positive ids; 0 means "no plan". Anything else in the array is skipped. */
static bool plan_array(sc_t *s, dz_device_t *d) {
  val_t v;
  int n = 0;
  s->p++; /* [ */
  ws(s);
  if (s->p < s->end && *s->p == ']') { s->p++; return true; }
  for (;;) {
    ws(s);
    if (!read_value(s, &v, 3)) return false;
    int id;
    if ((v.t == V_NUM || v.t == V_STR) && to_int(v.s, &id) && id > 0 && n < DZ_MAX_DEV_PLANS) {
      d->plan_ids[n++] = id;
      d->n_plan_ids = n;
    }
    ws(s);
    if (s->p >= s->end) return false;
    char c = *s->p++;
    if (c == ']') return true;
    if (c != ',') return false;
  }
}

/* Parses one device object. Sets *ok=false if the entry is syntactically fine but unusable. */
static bool parse_device(sc_t *s, dz_device_t *d, int depth) {
  val_t v;
  char key[32];
  memset(d, 0, sizeof *d);
  d->switch_type = -1;
  d->max_level = 100;
  if (depth > MAX_DEPTH || s->p >= s->end || *s->p != '{') return false;
  s->p++;
  ws(s);
  if (s->p < s->end && *s->p == '}') { s->p++; return true; }
  for (;;) {
    ws(s);
    if (!str(s, key, sizeof key)) return false;
    ws(s);
    if (s->p >= s->end || *s->p++ != ':') return false;
    ws(s);
    if (!strcmp(key, "PlanIDs") && s->p < s->end && *s->p == '[') {
      if (!plan_array(s, d)) return false;
    } else {
      if (!read_value(s, &v, depth + 1)) return false;
      assign(d, key, &v);
    }
    ws(s);
    if (s->p >= s->end) return false;
    char c = *s->p++;
    if (c == '}') return true;
    if (c != ',') return false;
  }
}

static void finish(dz_device_t *d) {
  d->has_setpoint_range = (d->range_bits & 7u) == 7u && d->setpoint_min < d->setpoint_max && d->setpoint_step > 0.0f;
  if (!d->has_usage && !strcmp(d->type, "Usage") && parse_watts(d->data, &d->usage_w)) d->has_usage = true;
  /* A Wind or UV sensor's Temp is that sensor's own thermometer, not the weather temperature. */
  if (!strcmp(d->type, "Wind") || !strcmp(d->type, "UV")) {
    for (int i = 0; i < d->n_rd; i++) if (d->rd[i].kind == DZ_RK_TEMP) d->rd[i].kind = DZ_RK_AUXTEMP;
    d->has_temp = false;
  }
  /* A Barometer reading carries the unit Domoticz wrote into Data ("1013 hPa"), or none. */
  if (strstr(d->data, "hPa")) {
    for (int i = 0; i < d->n_rd; i++) if (d->rd[i].kind == DZ_RK_BARO && !d->rd[i].unit[0]) strcpy(d->rd[i].unit, "hPa");
  }
  /* A setpoint is changeable only with a range that fits a dial: Domoticz defaults to -200..200 when the
   * user never set one, and Evohome ("Heating") overrides go through other commands. */
  {
    float span = d->setpoint_max - d->setpoint_min;
    d->can_set = d->has_setpoint && d->has_setpoint_range && span <= 100.0f && span / d->setpoint_step <= 400.0f &&
                 d->setpoint_step >= 0.01f && strcmp(d->type, "Heating") != 0;
  }
  d->has_mode = (d->range_bits & 24u) == 24u;
  d->cap = classify(d);
  /* Energy fields as readings */
  if (d->has_usage) add_rd(d, DZ_RK_POWER, d->usage_w, "W");
  if (d->has_today) {
    dz_rk_t k = kind_for(d->subtype, d->today_unit, d->type);
    add_rd(d, k == DZ_RK_NUMBER || k == DZ_RK_POWER ? DZ_RK_NUMBER : k, d->today, d->today_unit);
  }
  /* Anything with a number or text in Data and no field-derived reading: still shown. */
  if (d->n_rd == 0 && (d->cap == DZ_CAP_READONLY || d->cap == DZ_CAP_WEATHER || d->cap == DZ_CAP_TEMP || d->cap == DZ_CAP_ENERGY))
    data_reading(d);
  if (d->cap == DZ_CAP_MODE && d->data[0] && !dz_dev_reading(d, DZ_RK_TEXT) && d->n_rd < DZ_MAX_READINGS) add_rd(d, DZ_RK_TEXT, 0.0f, "");
  if (d->cap == DZ_CAP_READONLY && !is_switch_family(d->type) && d->n_rd > 0) d->cap = DZ_CAP_SENSOR;
  d->controllable = !d->is_protected && d->cap != DZ_CAP_READONLY && d->cap != DZ_CAP_STATUS && d->cap != DZ_CAP_TEMP &&
                    d->cap != DZ_CAP_WEATHER && d->cap != DZ_CAP_ENERGY && d->cap != DZ_CAP_SENSOR;
  if (d->cap == DZ_CAP_THERMOSTAT) d->controllable = !d->is_protected && d->can_set;
  if (d->cap == DZ_CAP_MODE) d->controllable = !d->is_protected && d->n_modes >= 2;
}

typedef struct {
  dz_device_cb cb;
  void *user;
  dz_result_t *res;
  bool stop;
} walk_t;

static bool parse_result_array(sc_t *s, walk_t *w, int depth) {
  dz_device_t d;
  if (depth > MAX_DEPTH || s->p >= s->end || *s->p != '[') return false;
  s->p++;
  ws(s);
  if (s->p < s->end && *s->p == ']') { s->p++; return true; }
  for (;;) {
    ws(s);
    if (w->stop) {
      if (!skip_value(s, depth + 1)) return false;
    } else if (s->p < s->end && *s->p == '{') {
      if (!parse_device(s, &d, depth + 1)) return false;
      if (!valid_idx(d.idx)) {
        w->res->rejected++;
      } else {
        finish(&d);
        w->res->devices++;
        if (w->cb && !w->cb(&d, w->user)) w->stop = true;
      }
    } else if (!skip_value(s, depth + 1)) {
      return false;
    }
    ws(s);
    if (s->p >= s->end) return false;
    char c = *s->p++;
    if (c == ']') return true;
    if (c != ',') return false;
  }
}

dz_err_t dz_parse_response(const char *json, size_t len, dz_device_cb cb, void *user, dz_result_t *out) {
  dz_result_t local;
  dz_result_t *r = out ? out : &local;
  walk_t w = {cb, user, r, false};
  sc_t s = {json, json + len};
  char key[32];
  val_t v;
  memset(r, 0, sizeof *r);
  if (!json) return DZ_ERR_SYNTAX;
  ws(&s);
  if (s.p >= s.end || *s.p++ != '{') return DZ_ERR_SYNTAX;
  ws(&s);
  if (s.p < s.end && *s.p == '}') { s.p++; goto trailer; }
  for (;;) {
    ws(&s);
    if (!str(&s, key, sizeof key)) return DZ_ERR_SYNTAX;
    ws(&s);
    if (s.p >= s.end || *s.p++ != ':') return DZ_ERR_SYNTAX;
    ws(&s);
    if (!strcmp(key, "result") && s.p < s.end && *s.p == '[') {
      if (!parse_result_array(&s, &w, 1)) return DZ_ERR_SYNTAX;
    } else {
      if (!read_value(&s, &v, 1)) return DZ_ERR_SYNTAX;
      if (!strcmp(key, "status")) copy(r->status, sizeof r->status, &v);
      else if (!strcmp(key, "ServerTime")) copy(r->server_time, sizeof r->server_time, &v);
      else if (!strcmp(key, "version")) copy(r->version, sizeof r->version, &v);
      else if (!strcmp(key, "app_version")) { if (!r->version[0]) copy(r->version, sizeof r->version, &v); }
      else if (!strcmp(key, "Sunrise")) copy(r->sunrise, sizeof r->sunrise, &v);
      else if (!strcmp(key, "Sunset")) copy(r->sunset, sizeof r->sunset, &v);
      else if (!strcmp(key, "ActTime") && v.t == V_NUM) {
        char *e;
        errno = 0;
        long long n = strtoll(v.s, &e, 10); /* integer parse: a float cannot hold an epoch second exactly */
        if (errno == 0 && !*e && e != v.s && n >= 0) /* ERANGE: strtoll saturates, which would freeze the delta cursor */ { r->act_time = n; r->has_act_time = true; }
      }
    }
    ws(&s);
    if (s.p >= s.end) return DZ_ERR_SYNTAX;
    char c = *s.p++;
    if (c == '}') break;
    if (c != ',') return DZ_ERR_SYNTAX;
  }
trailer:
  ws(&s);
  if (s.p != s.end) return DZ_ERR_SYNTAX;
  return strcmp(r->status, "OK") == 0 ? DZ_OK : DZ_ERR_STATUS;
}

const char *dz_cap_name(dz_cap_t c) {
  static const char *const n[] = {"readonly", "onoff", "dimmer", "blinds", "selector", "push", "lock",
                                  "status", "thermostat", "temp", "weather", "energy", "scene", "group", "sensor", "mode"};
  return (unsigned)c < sizeof n / sizeof n[0] ? n[c] : "?";
}

dz_state_t dz_device_state(const dz_device_t *d) {
  const char *s = d->status;
  if (!s[0]) return DZ_STATE_UNKNOWN;
  if (!strcmp(s, "Off") || !strcmp(s, "Closed") || !strcmp(s, "Locked")) return DZ_STATE_OFF;
  if (!strcmp(s, "Mixed")) return dz_cap_is_scene_ns(d->cap) ? DZ_STATE_MIXED : DZ_STATE_UNKNOWN; /* only groups mix */
  return DZ_STATE_ON;
}
