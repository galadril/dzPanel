#include "ru_dzview.h"

#include "ru_climate.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void ru_dzv_upper(char *dst, size_t n, const char *src) {
  size_t i = 0;
  for (; src[i] && i + 1 < n; i++) dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? (char)(src[i] - 32) : src[i];
  dst[i] = 0;
}

void ru_dzv_clamp(char *dst, size_t n, const char *src, size_t max) {
  if (n == 0) return;
  size_t len = strlen(src);
  if (len <= max && len < n) { memmove(dst, src, len + 1); return; }
  size_t cut = max < n - 3 ? max : n - 3;
  while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80) cut--;
  memmove(dst, src, cut);
  memcpy(dst + cut, "..", 3);
}

ru_tile_state_t ru_dzv_tile_state(const dz_device_t *d) {
  if (d->cap == DZ_CAP_SCENE) return RU_TILE_PUSH; /* a Scene has no state worth showing */
  switch (dz_device_state(d)) {
    case DZ_STATE_ON: return RU_TILE_ON;
    case DZ_STATE_OFF: return RU_TILE_OFF;
    case DZ_STATE_MIXED: return d->cap == DZ_CAP_GROUP ? RU_TILE_MIXED : RU_TILE_UNKNOWN;
    default: return RU_TILE_UNKNOWN;
  }
}

const char *ru_dzv_tile_label(ru_tile_state_t s) {
  switch (s) {
    case RU_TILE_ON: return "ON";
    case RU_TILE_OFF: return "OFF";
    case RU_TILE_MIXED: return "MIXED";
    case RU_TILE_PUSH: return "SCENE";
    default: return "--";
  }
}

bool ru_dzv_scene_tap(const dz_device_t *d, dz_cmd_kind_t *kind, const char **verb) {
  dz_cmd_kind_t k;
  if (!dz_cmd_scene_tap_kind(d, &k)) return false;
  if (kind) *kind = k;
  if (verb) *verb = d->cap == DZ_CAP_SCENE ? "ACTIVATE" : k == DZ_CMD_SCENE_OFF ? "SWITCH OFF" : "SWITCH ON";
  return true;
}

void ru_dzv_power_text(float w, char *out, size_t n) {
  if (!isfinite(w)) { snprintf(out, n, "--"); return; }
  if (fabsf(w) >= 1000.0f) snprintf(out, n, "%.2f kW", (double)w / 1000.0);
  else snprintf(out, n, "%.0f W", (double)w);
}

bool ru_dzv_selector_name(const dz_device_t *d, int index, char *out, size_t n) {
  if (index < 0 || !d->level_names[0]) return false;
  const char *p = d->level_names;
  for (int k = 0; k < index; k++) {
    p = strchr(p, '|');
    if (!p) return false;
    p++;
  }
  const char *e = strchr(p, '|');
  size_t l = e ? (size_t)(e - p) : strlen(p);
  if (l == 0) return false;
  char tmp[24];
  if (l >= sizeof tmp) l = sizeof tmp - 1;
  memcpy(tmp, p, l);
  tmp[l] = 0;
  ru_dzv_upper(out, n, tmp);
  return true;
}

void ru_dzv_temp_text(const dz_device_t *d, char *out, size_t n) {
  if (d->has_temp && isfinite(d->temp)) snprintf(out, n, "%.1f", (double)d->temp);
  else snprintf(out, n, "--");
}
void ru_dzv_hum_text(const dz_device_t *d, char *out, size_t n) {
  if (d->has_hum && isfinite(d->hum)) snprintf(out, n, "HUMIDITY %.0f%%", (double)d->hum);
  else if (n) out[0] = 0;
}
void ru_dzv_baro_text(const dz_device_t *d, char *out, size_t n) {
  if (d->has_baro && isfinite(d->baro)) {
    const dz_reading_t *r = dz_dev_reading(d, DZ_RK_BARO);
    if (r && r->unit[0]) snprintf(out, n, "%.0f %s", (double)d->baro, r->unit);
    else snprintf(out, n, "%.0f", (double)d->baro);
  }
  else if (n) out[0] = 0;
}

void ru_dzv_energy(const dz_device_t *d, ru_energy_view_t *o) {
  memset(o, 0, sizeof *o);
  snprintf(o->num, sizeof o->num, "--");
  snprintf(o->today, sizeof o->today, "--");
  if (d->has_today && isfinite(d->today)) {
    /* The unit is whatever Domoticz reports. Without one, the bare number is shown, never a guessed unit. */
    if (d->today_unit[0]) snprintf(o->today, sizeof o->today, "TODAY %.1f %s", (double)d->today, d->today_unit);
    else snprintf(o->today, sizeof o->today, "TODAY %.1f", (double)d->today);
    o->has_today = true;
  }
  bool imp = d->has_usage && isfinite(d->usage_w), exp = d->has_usage_deliv && isfinite(d->usage_deliv_w);
  if (!imp && !exp) return;
  float use = imp ? d->usage_w : 0.0f, del = exp ? d->usage_deliv_w : 0.0f;
  float net = use - del; /* positive = drawing from the grid */
  float hero = (del > 0.0f && use <= 0.0f) ? del : use;
  char t[12];
  ru_dzv_power_text(hero, t, sizeof t);
  /* split "2.10 kW" into number and unit */
  char *sp = strchr(t, ' ');
  if (sp) { *sp = 0; snprintf(o->unit, sizeof o->unit, "%s", sp + 1); }
  snprintf(o->num, sizeof o->num, "%s", t);
  o->known = true;
  o->power_w = net;
  o->hero_w = hero;
  if (del > 0.0f) {
    o->flow = RU_FLOW_EXPORT;
    char x[12];
    ru_dzv_power_text(del, x, sizeof x);
    snprintf(o->flow_text, sizeof o->flow_text, "EXPORT %s", x);
  } else if (use > 0.0f && exp) { /* a bidirectional meter currently delivering nothing */
    o->flow = RU_FLOW_IMPORT;
    char x[12];
    ru_dzv_power_text(use, x, sizeof x);
    snprintf(o->flow_text, sizeof o->flow_text, "IMPORT %s", x);
  }
}

void ru_dzv_value_text(const dz_device_t *d, char *out, size_t n, bool *known) {
  ru_dzv_value_text_u(d, NULL, out, n, known);
}

void ru_dzv_value_text_u(const dz_device_t *d, const dz_units_t *units, char *out, size_t n, bool *known) {
  *known = true;
  char tmp[40];
  switch (d->cap) {
    case DZ_CAP_ONOFF: {
      dz_state_t s = dz_device_state(d);
      if (s == DZ_STATE_UNKNOWN) break;
      snprintf(out, n, "%s", s == DZ_STATE_OFF ? "OFF" : "ON");
      return;
    }
    case DZ_CAP_PUSH: snprintf(out, n, "TAP"); return;
    case DZ_CAP_DIMMER:
      if (dz_device_state(d) == DZ_STATE_UNKNOWN) break;
      if (!dz_status_is_on(d)) { snprintf(out, n, "OFF"); return; }
      if (!d->has_level) { snprintf(out, n, "ON"); return; }
      snprintf(out, n, "%d%%", d->max_level > 0 ? d->level * 100 / d->max_level : d->level);
      return;
    case DZ_CAP_SELECTOR:
      if (ru_dzv_selector_name(d, d->level_int / 10, out, n)) return;
      break;
    case DZ_CAP_BLINDS:
    case DZ_CAP_LOCK:
    case DZ_CAP_STATUS:
      if (d->status[0]) { ru_dzv_clamp(tmp, sizeof tmp, d->status, 14); ru_dzv_upper(out, n, tmp); return; }
      break;
    case DZ_CAP_THERMOSTAT:
      if (d->has_setpoint) {
        ru_cl_thermo_t th;
        ru_cl_thermo(d, units, NULL, false, &th);
        ru_cl_thermo_text(&th, d->setpoint, out, n);
        return;
      }
      break;
    case DZ_CAP_TEMP:
    case DZ_CAP_WEATHER:
    case DZ_CAP_SENSOR:
      if (d->n_rd > 0) {
        ru_cl_units_t u;
        ru_cl_units(units, &u);
        ru_cl_summary(d, &u, out, n, known);
        return;
      }
      if (d->has_temp) { ru_dzv_temp_text(d, out, n); return; } /* no readings (hand-made device): the legacy field */
      if (d->cap == DZ_CAP_WEATHER && d->has_baro) { ru_dzv_baro_text(d, out, n); return; }
      break;
    case DZ_CAP_MODE:
      if (d->data[0]) { ru_dzv_clamp(tmp, sizeof tmp, d->data, 14); ru_dzv_upper(out, n, tmp); return; }
      break;
    case DZ_CAP_ENERGY: {
      ru_energy_view_t e;
      ru_dzv_energy(d, &e);
      if (e.known) { snprintf(out, n, "%s %s", e.num, e.unit); return; }
      if (e.has_today && d->today_unit[0]) { snprintf(out, n, "%.1f %s", (double)d->today, d->today_unit); return; }
      break;
    }
    case DZ_CAP_SCENE:
    case DZ_CAP_GROUP: snprintf(out, n, "%s", ru_dzv_tile_label(ru_dzv_tile_state(d))); return;
    case DZ_CAP_READONLY:
      if (d->data[0]) { ru_dzv_clamp(tmp, sizeof tmp, d->data, 16); ru_dzv_upper(out, n, tmp); return; }
      break;
  }
  *known = false;
  snprintf(out, n, "--");
}

/* ---- clock ---- */

static int64_t floor_div(int64_t a, int64_t b) { int64_t q = a / b; return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q; }

void ru_dzv_clock_text(int64_t epoch_s, char out[6]) {
  int64_t sod = epoch_s - floor_div(epoch_s, 86400) * 86400;
  unsigned h = (unsigned)(sod / 3600) % 24u, m = (unsigned)(sod % 3600 / 60) % 60u;
  snprintf(out, 6, "%02u:%02u", h, m);
}

void ru_dzv_date_text(int64_t epoch_s, char out[12]) {
  static const char *const wd[7] = {"THU", "FRI", "SAT", "SUN", "MON", "TUE", "WED"}; /* 1970-01-01 was a Thursday */
  static const char *const mo[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  int64_t days = floor_div(epoch_s, 86400);
  /* civil from days (Howard Hinnant), proleptic Gregorian */
  int64_t z = days + 719468;
  int64_t era = floor_div(z, 146097);
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int day = (int)(doy - (153 * mp + 2) / 5 + 1);
  int mon = (int)(mp < 10 ? mp + 3 : mp - 9); /* 1..12 */
  int w = (int)(days - floor_div(days, 7) * 7);
  snprintf(out, 12, "%s %d %s", wd[w], day, mo[mon - 1]);
}

/* ---- gauges ---- */

float ru_dzv_nice_max(float v) {
  if (!(v > 0.0f) || !isfinite(v)) return 1.0f;
  float e = powf(10.0f, floorf(log10f(v)));
  float m = v / e;
  return (m <= 1.0f ? 1.0f : m <= 2.0f ? 2.0f : m <= 5.0f ? 5.0f : 10.0f) * e;
}

static void gauge_caption(const dz_device_t *d, char *out, size_t n) {
  char up[40];
  ru_dzv_upper(up, sizeof up, d->name);
  size_t len = strlen(up), cut = len < 8 ? len : 8;
  while (cut > 0 && cut < len && ((unsigned char)up[cut] & 0xC0) == 0x80) cut--;
  while (cut > 0 && up[cut - 1] == ' ') cut--;
  if (cut >= n) cut = n - 1;
  memcpy(out, up, cut);
  out[cut] = 0;
}

/* "2.1" "2.10" -> shortest of the two forms with a unit glued on: "2.1kW", "850W" */
static void gauge_power(float w, char *out, size_t n) {
  float a = fabsf(w);
  if (a >= 1000.0f) {
    char num[12];
    snprintf(num, sizeof num, a >= 10000.0f ? "%.0f" : "%.2f", (double)w / 1000.0);
    if (strchr(num, '.')) {
      size_t l = strlen(num);
      while (l > 1 && num[l - 1] == '0') num[--l] = 0;
      if (num[l - 1] == '.') num[--l] = 0;
    }
    snprintf(out, n, "%skW", num);
  } else snprintf(out, n, "%.0fW", (double)w);
}

bool ru_dzv_gauge(const dz_device_t *d, const dz_units_t *units, ru_gauge_view_t *o) {
  memset(o, 0, sizeof *o);
  snprintf(o->text, sizeof o->text, "--");
  o->frac = -1.0f;
  o->kind = RU_GAUGE_TEXT;
  gauge_caption(d, o->caption, sizeof o->caption);
  ru_cl_units_t u;
  ru_cl_units(units, &u);
  if (d->cap == DZ_CAP_ENERGY) {
    ru_energy_view_t e;
    ru_dzv_energy(d, &e);
    if (e.known) {
      gauge_power(e.hero_w, o->text, sizeof o->text);
      o->known = true;
      o->kind = RU_GAUGE_POWER;
      o->frac = fminf(fabsf(e.hero_w) / ru_dzv_nice_max(fmaxf(fabsf(e.hero_w), 1000.0f)), 1.0f);
    } else if (e.has_today) {
      snprintf(o->text, sizeof o->text, d->today_unit[0] ? "%.1f%s" : "%.1f", (double)d->today, d->today_unit);
      o->known = true;
    }
    return true;
  }
  if (d->cap != DZ_CAP_TEMP && d->cap != DZ_CAP_WEATHER && d->cap != DZ_CAP_SENSOR) return false;
  ru_cl_page_t pg = ru_cl_page_of(d);
  if (pg == RU_CLP_NONE || pg == RU_CLP_TEXT || pg == RU_CLP_SETPOINT) /* no number: only a temperature device is still a gauge, without a value */
    return d->cap != DZ_CAP_SENSOR && d->n_rd == 0 && pg != RU_CLP_SETPOINT;
  if (pg == RU_CLP_TEMP) {
    const dz_reading_t *r = dz_dev_room_temp(d);
    if (r && r->has && isfinite(r->v)) {
      ru_cl_text(r, &u, o->text, sizeof o->text);
      o->known = true;
      o->kind = RU_GAUGE_TEMP;
      float lo, hi;
      if (!strcmp(u.temp, "\xC2\xB0" "C")) { lo = 0.0f; hi = 40.0f; }
      else if (!strcmp(u.temp, "\xC2\xB0" "F")) { lo = 32.0f; hi = 104.0f; }
      else { lo = 0.0f; hi = 0.0f; }
      if (hi > lo) o->frac = fminf(fmaxf((r->v - lo) / (hi - lo), 0.0f), 1.0f);
      else o->kind = RU_GAUGE_TEXT; /* no unit read: no scale to guess */
    }
    return true;
  }
  const dz_reading_t *pr = NULL;
  if (pg == RU_CLP_HUM) pr = dz_dev_reading(d, DZ_RK_HUM);
  else if (pg == RU_CLP_VALUE) {
    for (int i = 0; i < d->n_rd; i++)
      if (d->rd[i].has && d->rd[i].kind != DZ_RK_TEXT && d->rd[i].kind != DZ_RK_AUXTEMP) { if (d->rd[i].kind == DZ_RK_PERCENT) pr = &d->rd[i]; break; }
  }
  if (pr && pr->has && isfinite(pr->v)) {
    ru_cl_text(pr, &u, o->text, sizeof o->text);
    o->known = true;
    o->kind = RU_GAUGE_PERCENT;
    o->frac = fminf(fmaxf(pr->v / 100.0f, 0.0f), 1.0f);
    return true;
  }
  bool known;
  char t[24];
  ru_cl_summary(d, &u, t, sizeof t, &known);
  if (known) { snprintf(o->text, sizeof o->text, "%s", t); o->known = true; }
  return true;
}
