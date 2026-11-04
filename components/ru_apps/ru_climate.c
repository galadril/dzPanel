#include "ru_climate.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ru_dzview.h"

#define DEG "\xC2\xB0"

void ru_cl_units(const dz_units_t *u, ru_cl_units_t *out) {
  memset(out, 0, sizeof *out);
  if (!u) return;
  dz_units_temp_text(u, out->temp, sizeof out->temp);
  if (u->have_wind && u->wind[0]) snprintf(out->wind, sizeof out->wind, "%s", u->wind);
}

static bool is_temp_kind(dz_rk_t k) { return k == DZ_RK_TEMP || k == DZ_RK_AUXTEMP || k == DZ_RK_DEW || k == DZ_RK_CHILL; }
static bool is_wind_kind(dz_rk_t k) { return k == DZ_RK_WIND || k == DZ_RK_GUST; }

/* "%.1f" without "-0.0" */
static void fixed1(char *o, size_t n, float v) {
  snprintf(o, n, "%.1f", (double)v);
  if (!strcmp(o, "-0.0")) snprintf(o, n, "0.0");
}

static void generic_num(char *o, size_t n, float v) {
  float a = fabsf(v);
  if (a >= 100.0f || v == floorf(v)) snprintf(o, n, "%.0f", (double)v);
  else if (a >= 10.0f) snprintf(o, n, "%.1f", (double)v);
  else {
    snprintf(o, n, "%.2f", (double)v);
    size_t l = strlen(o);
    while (l > 1 && o[l - 1] == '0') o[--l] = 0;
    if (l > 1 && o[l - 1] == '.') o[--l] = 0;
  }
  if (!strcmp(o, "-0")) snprintf(o, n, "0");
}

void ru_cl_fmt(const dz_reading_t *r, const ru_cl_units_t *u, ru_cl_val_t *out) {
  memset(out, 0, sizeof *out);
  snprintf(out->num, sizeof out->num, "--");
  if (!r || !r->has || !isfinite(r->v) || r->kind == DZ_RK_TEXT) return;
  out->known = true;
  const char *unit = r->unit;
  if (is_temp_kind(r->kind)) {
    fixed1(out->num, sizeof out->num, r->v);
    unit = u ? u->temp : "";
  } else if (is_wind_kind(r->kind)) {
    unit = u ? u->wind : "";
    if (unit[0] && !strcmp(unit, "bft")) snprintf(out->num, sizeof out->num, "%.0f", (double)r->v);
    else fixed1(out->num, sizeof out->num, r->v);
  } else if (r->kind == DZ_RK_DIR) {
    snprintf(out->num, sizeof out->num, "%.0f", (double)r->v);
    unit = DEG; /* degrees of an angle: intrinsic to a bearing */
  } else if (r->kind == DZ_RK_HUM) {
    snprintf(out->num, sizeof out->num, "%.0f", (double)r->v);
    unit = "%";
  } else {
    generic_num(out->num, sizeof out->num, r->v);
  }
  snprintf(out->unit, sizeof out->unit, "%s", unit ? unit : "");
}

void ru_cl_text(const dz_reading_t *r, const ru_cl_units_t *u, char *out, size_t n) {
  ru_cl_val_t v;
  ru_cl_fmt(r, u, &v);
  if (!v.known) { snprintf(out, n, "--"); return; }
  /* degree and percent attach to the number; words ("m/s", "Lux") are separated */
  bool attach = !v.unit[0] || v.unit[0] == '%' || !strncmp(v.unit, DEG, 2);
  snprintf(out, n, attach ? "%s%s" : "%s %s", v.num, v.unit);
}

const char *ru_cl_uv_word(float v) {
  if (!isfinite(v) || v < 0.0f) return "";
  if (v < 3.0f) return "LOW";
  if (v < 6.0f) return "MODERATE";
  if (v < 8.0f) return "HIGH";
  if (v < 11.0f) return "VERY HIGH";
  return "EXTREME";
}

bool ru_cl_battery_low(const dz_device_t *d) { return d->has_battery && d->battery <= 15; }

const char *ru_cl_trend_word(int t) {
  return t == DZ_TREND_UP ? "RISING" : t == DZ_TREND_DOWN ? "FALLING" : t == DZ_TREND_STABLE ? "STEADY" : "";
}

static const dz_reading_t *first_number(const dz_device_t *d, bool allow_aux) {
  for (int i = 0; i < d->n_rd; i++) {
    const dz_reading_t *r = &d->rd[i];
    if (r->has && r->kind != DZ_RK_TEXT && (allow_aux || r->kind != DZ_RK_AUXTEMP)) return r;
  }
  return NULL;
}

ru_cl_page_t ru_cl_page_of(const dz_device_t *d) {
  if (dz_dev_room_temp(d)) return RU_CLP_TEMP;
  if (dz_dev_reading(d, DZ_RK_HUM)) return RU_CLP_HUM;
  if (dz_dev_reading(d, DZ_RK_WIND) || dz_dev_reading(d, DZ_RK_GUST) || dz_dev_reading(d, DZ_RK_DIR)) return RU_CLP_WIND;
  if (dz_dev_reading(d, DZ_RK_RAIN) || dz_dev_reading(d, DZ_RK_RAINRATE)) return RU_CLP_RAIN;
  if (dz_dev_reading(d, DZ_RK_UVI)) return RU_CLP_UV;
  if (dz_dev_reading(d, DZ_RK_AIRQ)) return RU_CLP_AIR;
  if (first_number(d, true)) return RU_CLP_VALUE;
  if (dz_dev_reading(d, DZ_RK_TEXT) || d->data[0]) return RU_CLP_TEXT;
  return RU_CLP_NONE;
}

static const char *kind_label(dz_rk_t k) {
  switch (k) {
    case DZ_RK_TEMP: return "TEMP";
    case DZ_RK_AUXTEMP: return "SENSOR TEMP";
    case DZ_RK_HUM: return "HUMIDITY";
    case DZ_RK_BARO: return "PRESSURE";
    case DZ_RK_DEW: return "DEW POINT";
    case DZ_RK_WIND: return "WIND";
    case DZ_RK_GUST: return "GUST";
    case DZ_RK_DIR: return "DIRECTION";
    case DZ_RK_CHILL: return "CHILL";
    case DZ_RK_RAIN: return "RAIN TOTAL";
    case DZ_RK_RAINRATE: return "RAIN RATE";
    case DZ_RK_UVI: return "UV INDEX";
    case DZ_RK_LUX: return "LIGHT";
    case DZ_RK_RADIATION: return "SOLAR";
    case DZ_RK_VISIBILITY: return "VISIBILITY";
    case DZ_RK_AIRQ: return "AIR QUALITY";
    case DZ_RK_MOISTURE: return "MOISTURE";
    case DZ_RK_DISTANCE: return "DISTANCE";
    case DZ_RK_PERCENT: return "LEVEL";
    case DZ_RK_POWER: return "POWER";
    case DZ_RK_ENERGY: return "ENERGY";
    case DZ_RK_GAS: return "GAS";
    case DZ_RK_WATER: return "WATER";
    case DZ_RK_VOLTAGE: return "VOLTAGE";
    case DZ_RK_CURRENT: return "CURRENT";
    case DZ_RK_WEIGHT: return "WEIGHT";
    case DZ_RK_NUMBER: return "VALUE";
    case DZ_RK_TEXT: return "STATUS";
    default: return "";
  }
}

static void add_line(ru_cl_view_t *v, const char *fmt, const char *a, const char *b, bool warn) {
  if (v->n_lines >= RU_CL_LINES) return;
  ru_cl_line_t *l = &v->line[v->n_lines++];
  snprintf(l->text, sizeof l->text, fmt, a, b);
  l->warn = warn;
}

/* "LABEL value" line for a reading, when present */
static void add_rd_line(ru_cl_view_t *v, const dz_device_t *d, const ru_cl_units_t *u, dz_rk_t k, const char *label) {
  const dz_reading_t *r = dz_dev_reading(d, k);
  if (!r) return;
  char t[24];
  ru_cl_text(r, u, t, sizeof t);
  add_line(v, "%s %s", label, t, false);
}

static void upper(char *dst, size_t n, const char *src) { ru_dzv_upper(dst, n, src); }

/* temperature ring: scaled by the unit's own range; no unit, no scale */
static float temp_ring(float v, const ru_cl_units_t *u) {
  float lo, hi;
  if (!strcmp(u->temp, DEG "C")) { lo = -10.0f; hi = 40.0f; }
  else if (!strcmp(u->temp, DEG "F")) { lo = 14.0f; hi = 104.0f; }
  else return -1.0f;
  float f = (v - lo) / (hi - lo);
  return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

void ru_cl_build(const dz_device_t *d, const ru_cl_units_t *u, ru_cl_view_t *v) {
  memset(v, 0, sizeof *v);
  v->ring = -1.0f;
  v->trend = d->trend;
  v->battery_low = ru_cl_battery_low(d);
  char nm[40];
  upper(nm, sizeof nm, d->name);
  ru_dzv_clamp(v->name, sizeof v->name, nm, 18);
  v->page = ru_cl_page_of(d);
  ru_cl_val_t val;
  char t[24];
  switch (v->page) {
    case RU_CLP_TEMP: {
      const dz_reading_t *r = dz_dev_room_temp(d);
      ru_cl_text(r, u, v->hero, sizeof v->hero);
      v->known = true;
      snprintf(v->caption, sizeof v->caption, "TEMP");
      v->ring = temp_ring(r->v, u);
      const dz_reading_t *h = dz_dev_reading(d, DZ_RK_HUM), *b = dz_dev_reading(d, DZ_RK_BARO), *dp = dz_dev_reading(d, DZ_RK_DEW);
      char w[20];
      upper(w, sizeof w, d->hum_status);
      if (h) {
        ru_cl_text(h, u, t, sizeof t);
        if (w[0]) { char c[64]; snprintf(c, sizeof c, "%s  %s", t, w); add_line(v, "HUMIDITY %s", c, "", false); }
        else add_line(v, "HUMIDITY %s", t, "", false);
      }
      char fc[24] = "";
      if (d->forecast[0] && strcmp(d->forecast, "Unknown") != 0) upper(fc, sizeof fc, d->forecast);
      if (b) {
        ru_cl_text(b, u, t, sizeof t);
        if (fc[0]) { char c[64]; snprintf(c, sizeof c, "%s  %s", t, fc); add_line(v, "%s%s", c, "", false); }
        else add_line(v, "%s%s", t, "", false);
      } else if (fc[0]) add_line(v, "%s%s", fc, "", false);
      if (dp) { ru_cl_text(dp, u, t, sizeof t); add_line(v, "DEW POINT %s", t, "", false); }
      break;
    }
    case RU_CLP_HUM: {
      const dz_reading_t *h = dz_dev_reading(d, DZ_RK_HUM);
      ru_cl_text(h, u, v->hero, sizeof v->hero);
      v->known = true;
      snprintf(v->caption, sizeof v->caption, "HUMIDITY");
      v->ring = h->v < 0.0f ? 0.0f : h->v > 100.0f ? 1.0f : h->v / 100.0f;
      upper(v->word, sizeof v->word, d->hum_status);
      add_rd_line(v, d, u, DZ_RK_DEW, "DEW POINT");
      if (dz_dev_reading(d, DZ_RK_BARO)) { ru_cl_text(dz_dev_reading(d, DZ_RK_BARO), u, t, sizeof t); add_line(v, "%s%s", t, "", false); }
      break;
    }
    case RU_CLP_WIND: {
      const dz_reading_t *s = dz_dev_reading(d, DZ_RK_WIND), *g = dz_dev_reading(d, DZ_RK_GUST), *dir = dz_dev_reading(d, DZ_RK_DIR);
      v->known = s != NULL;
      if (s) { ru_cl_fmt(s, u, &val); snprintf(v->speed, sizeof v->speed, "%s", val.num); snprintf(v->speed_unit, sizeof v->speed_unit, "%s", val.unit); }
      else snprintf(v->speed, sizeof v->speed, "--");
      snprintf(v->caption, sizeof v->caption, "WIND");
      if (dir) { v->has_dir = true; v->dir_deg = dir->v; }
      if (d->dir_str[0]) snprintf(v->dir_txt, sizeof v->dir_txt, "%s", d->dir_str);
      add_rd_line(v, d, u, DZ_RK_GUST, "GUST");
      (void)g;
      if (dir || v->dir_txt[0]) {
        char dt[16] = "";
        if (dir) { ru_cl_text(dir, u, dt, sizeof dt); }
        char c[64];
        const dz_reading_t *ch = dz_dev_reading(d, DZ_RK_CHILL);
        char ct[24] = "";
        if (ch) ru_cl_text(ch, u, ct, sizeof ct);
        snprintf(c, sizeof c, "%s%s%s%s%s", v->dir_txt, v->dir_txt[0] && dt[0] ? " " : "", dt, ch ? "  CHILL " : "", ct);
        add_line(v, "%s%s", c, "", false);
      } else add_rd_line(v, d, u, DZ_RK_CHILL, "CHILL");
      add_rd_line(v, d, u, DZ_RK_AUXTEMP, "SENSOR TEMP");
      break;
    }
    case RU_CLP_RAIN: {
      const dz_reading_t *tot = dz_dev_reading(d, DZ_RK_RAIN), *rate = dz_dev_reading(d, DZ_RK_RAINRATE);
      const dz_reading_t *hero = tot ? tot : rate;
      ru_cl_text(hero, u, v->hero, sizeof v->hero);
      v->known = true;
      snprintf(v->caption, sizeof v->caption, "%s", tot ? "RAIN TOTAL" : "RAIN RATE");
      if (tot && rate) { ru_cl_text(rate, u, t, sizeof t); add_line(v, "RATE %s", t, "", false); }
      break;
    }
    case RU_CLP_UV: {
      const dz_reading_t *r = dz_dev_reading(d, DZ_RK_UVI);
      ru_cl_text(r, u, v->hero, sizeof v->hero);
      v->known = true;
      snprintf(v->caption, sizeof v->caption, "UV INDEX");
      v->ring = r->v < 0.0f ? 0.0f : r->v > 11.0f ? 1.0f : r->v / 11.0f;
      snprintf(v->word, sizeof v->word, "%s", ru_cl_uv_word(r->v));
      add_rd_line(v, d, u, DZ_RK_AUXTEMP, "SENSOR TEMP");
      break;
    }
    case RU_CLP_AIR: {
      const dz_reading_t *r = dz_dev_reading(d, DZ_RK_AIRQ);
      ru_cl_fmt(r, u, &val);
      snprintf(v->hero, sizeof v->hero, "%s", val.num);
      v->known = true;
      snprintf(v->caption, sizeof v->caption, "AIR QUALITY");
      upper(v->word, sizeof v->word, d->quality);
      if (val.unit[0]) add_line(v, "%s%s", val.unit, "", false);
      break;
    }
    case RU_CLP_VALUE: {
      const dz_reading_t *r = first_number(d, true);
      ru_cl_fmt(r, u, &val);
      snprintf(v->hero, sizeof v->hero, "%s", val.num);
      v->known = true;
      snprintf(v->caption, sizeof v->caption, "%s", kind_label(r->kind));
      if (val.unit[0]) add_line(v, "%s%s", val.unit, "", false);
      for (int i = 0; i < d->n_rd; i++) {
        const dz_reading_t *o = &d->rd[i];
        if (o == r || !o->has || o->kind == DZ_RK_TEXT) continue;
        ru_cl_text(o, u, t, sizeof t);
        add_line(v, "%s %s", kind_label(o->kind), t, false);
      }
      break;
    }
    case RU_CLP_TEXT:
      v->known = d->data[0] != 0;
      snprintf(v->text, sizeof v->text, "%s", d->data);
      snprintf(v->caption, sizeof v->caption, "STATUS");
      break;
    case RU_CLP_NONE:
    case RU_CLP_SETPOINT:
      break;
  }
}

void ru_cl_summary(const dz_device_t *d, const ru_cl_units_t *u, char *out, size_t n, bool *known) {
  ru_cl_page_t p = ru_cl_page_of(d);
  char t[24];
  *known = true;
  switch (p) {
    case RU_CLP_TEMP: {
      ru_cl_text(dz_dev_room_temp(d), u, out, n);
      const dz_reading_t *h = dz_dev_reading(d, DZ_RK_HUM);
      if (h) { ru_cl_text(h, u, t, sizeof t); size_t l = strlen(out); snprintf(out + l, n - l, "  %s", t); }
      return;
    }
    case RU_CLP_HUM: ru_cl_text(dz_dev_reading(d, DZ_RK_HUM), u, out, n); return;
    case RU_CLP_WIND: {
      const dz_reading_t *s = dz_dev_reading(d, DZ_RK_WIND);
      if (s) { ru_cl_text(s, u, out, n); return; }
      if (d->dir_str[0]) { snprintf(out, n, "%s", d->dir_str); return; }
      break;
    }
    case RU_CLP_RAIN: {
      const dz_reading_t *r = dz_dev_reading(d, DZ_RK_RAIN);
      ru_cl_text(r ? r : dz_dev_reading(d, DZ_RK_RAINRATE), u, out, n);
      return;
    }
    case RU_CLP_UV: ru_cl_text(dz_dev_reading(d, DZ_RK_UVI), u, t, sizeof t); snprintf(out, n, "UV %s", t); return;
    case RU_CLP_AIR: ru_cl_text(dz_dev_reading(d, DZ_RK_AIRQ), u, out, n); return;
    case RU_CLP_VALUE: ru_cl_text(first_number(d, true), u, out, n); return;
    case RU_CLP_TEXT: snprintf(out, n, "%s", d->data); if (out[0]) return; break;
    case RU_CLP_NONE:
    case RU_CLP_SETPOINT: break;
  }
  *known = false;
  snprintf(out, n, "--");
}

/* ---------------------------------------------------------------- setpoint devices */

static bool temp_like(const char *vunit) {
  return !strcmp(vunit, DEG "C") || !strcmp(vunit, DEG "F") || !strcmp(vunit, "C") || !strcmp(vunit, "F");
}

void ru_cl_thermo(const dz_device_t *d, const dz_units_t *un, const dz_device_t *plan, bool allow_control, ru_cl_thermo_t *o) {
  ru_cl_units_t u;
  memset(o, 0, sizeof *o);
  ru_cl_units(un, &u);
  o->has_setpoint = d->has_setpoint && isfinite(d->setpoint);
  o->setpoint = d->setpoint;
  o->is_temp = !d->vunit[0] || temp_like(d->vunit);
  if (o->is_temp) {
    if (u.temp[0]) snprintf(o->unit, sizeof o->unit, "%s", u.temp);
    else if (d->vunit[0] && !strncmp(d->vunit, DEG, 2)) snprintf(o->unit, sizeof o->unit, "%s", d->vunit); /* the device told us */
  } else {
    snprintf(o->unit, sizeof o->unit, "%s", d->vunit);
  }
  o->control = d->can_set && d->controllable && allow_control;
  if (o->control) { o->lo = d->setpoint_min; o->hi = d->setpoint_max; o->step = d->setpoint_step; }
  else if (d->is_protected) snprintf(o->reason, sizeof o->reason, "PROTECTED");
  else if (!d->can_set) snprintf(o->reason, sizeof o->reason, "NO RANGE");
  else snprintf(o->reason, sizeof o->reason, "VIEW ONLY");
  if (o->is_temp) {
    const dz_reading_t *own = dz_dev_room_temp(d);
    if (own) { o->has_now = true; o->now = own->v; }
    else if (plan && dz_dev_room_temp(plan)) { o->has_now = true; o->now = dz_dev_room_temp(plan)->v; o->now_from_plan = true; }
  }
}

void ru_cl_thermo_text(const ru_cl_thermo_t *t, float v, char *out, size_t n) {
  if (!isfinite(v)) { snprintf(out, n, "--"); return; }
  char num[16];
  fixed1(num, sizeof num, v);
  bool attach = !t->unit[0] || t->unit[0] == '%' || !strncmp(t->unit, DEG, 2);
  snprintf(out, n, attach ? "%s%s" : "%s %s", num, t->unit);
}

/* ---------------------------------------------------------------- mode devices */

bool ru_cl_mode_name(const dz_device_t *d, int n, char *out, size_t cap) {
  if (n < 0 || n >= d->n_modes || !cap) return false;
  const char *p = d->mode_names;
  for (int i = 0; i < n; i++) {
    p = strchr(p, '|');
    if (!p) return false;
    p++;
  }
  const char *e = strchr(p, '|');
  size_t l = e ? (size_t)(e - p) : strlen(p);
  char tmp[20];
  if (l >= sizeof tmp) l = sizeof tmp - 1;
  memcpy(tmp, p, l);
  tmp[l] = 0;
  upper(out, cap, tmp);
  return out[0] != 0;
}

int ru_cl_mode_slot(const dz_device_t *d) {
  if (!d->has_mode) return -1;
  for (int i = 0; i < d->n_modes && i < DZ_MAX_MODES; i++) if (d->mode_val[i] == d->mode) return i;
  return -1;
}
