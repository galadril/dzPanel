#include "ru_wx.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DEG "\xC2\xB0"

static const uint32_t k_rgb[RU_WX_COLORS] = {
  0xE6E9EE, /* NONE: the theme text colour */
  0x8A93A0, 0xA8D8FF, 0x4DB3FF, 0x3DE0C5, 0x3DDC84, 0xFFC247, 0xFF8A3D, 0xFF5A5F, 0x4D7CFF, 0x8C7CFF,
};

uint32_t ru_wx_color_rgb(ru_wx_color_t c) { return (unsigned)c < RU_WX_COLORS ? k_rgb[c] : k_rgb[0]; }

static bool ieq(const char *a, const char *b) {
  for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
  return !*a && !*b;
}

static bool icontains(const char *s, const char *needle) {
  size_t n = strlen(needle);
  for (; *s; s++) {
    size_t i = 0;
    while (i < n && s[i] && tolower((unsigned char)s[i]) == needle[i]) i++;
    if (i == n) return true;
  }
  return false;
}

ru_wx_color_t ru_wx_temp_band(float v, const char *unit) {
  if (!isfinite(v) || !unit) return RU_WX_NONE;
  float c;
  if (!strcmp(unit, DEG "C") || !strcmp(unit, "C")) c = v;
  else if (!strcmp(unit, DEG "F") || !strcmp(unit, "F")) c = (v - 32.0f) * 5.0f / 9.0f;
  else return RU_WX_NONE;
  if (c <= 0.0f) return RU_WX_ICE;
  if (c < 10.0f) return RU_WX_BLUE;
  if (c < 18.0f) return RU_WX_TEAL;
  if (c < 24.0f) return RU_WX_GREEN;
  if (c < 30.0f) return RU_WX_AMBER;
  if (c < 35.0f) return RU_WX_ORANGE;
  return RU_WX_RED;
}

ru_wx_color_t ru_wx_hum_band(const char *status, bool has_v, float v) {
  if (status && status[0]) {
    if (ieq(status, "Dry")) return RU_WX_AMBER;
    if (ieq(status, "Comfortable") || ieq(status, "Normal")) return RU_WX_GREEN;
    if (ieq(status, "Wet")) return RU_WX_BLUE;
  }
  if (!has_v || !isfinite(v)) return RU_WX_NONE;
  if (v < 30.0f) return RU_WX_AMBER;
  if (v > 70.0f) return RU_WX_BLUE;
  return RU_WX_GREEN;
}

ru_wx_color_t ru_wx_uv_band(float v) {
  if (!isfinite(v) || v < 0.0f) return RU_WX_NONE;
  if (v < 3.0f) return RU_WX_GREEN;
  if (v < 6.0f) return RU_WX_AMBER;
  if (v < 8.0f) return RU_WX_ORANGE;
  if (v < 11.0f) return RU_WX_RED;
  return RU_WX_VIOLET;
}

/* Beaufort number -> a speed inside its range, for devices that report the scale itself */
static const float k_bft_ms[13] = { 0.0f, 0.9f, 2.5f, 4.5f, 6.8f, 9.4f, 12.3f, 15.5f, 19.0f, 22.7f, 26.5f, 30.6f, 35.0f };

bool ru_wx_wind_ms(float v, const char *unit, float *ms) {
  if (!isfinite(v) || v < 0.0f || !unit || !unit[0]) return false;
  float r;
  if (ieq(unit, "m/s") || ieq(unit, "ms")) r = v;
  else if (ieq(unit, "km/h") || ieq(unit, "kmh") || ieq(unit, "kph")) r = v / 3.6f;
  else if (ieq(unit, "mph")) r = v * 0.44704f;
  else if (ieq(unit, "kn") || ieq(unit, "kt") || ieq(unit, "kts") || ieq(unit, "knots")) r = v * 0.514444f;
  else if (ieq(unit, "bft")) {
    int b = (int)lroundf(v);
    r = k_bft_ms[b < 0 ? 0 : b > 12 ? 12 : b];
  } else return false;
  *ms = r;
  return true;
}

int ru_wx_beaufort(float ms) {
  static const float up[12] = { 0.5f, 1.6f, 3.4f, 5.5f, 8.0f, 10.8f, 13.9f, 17.2f, 20.8f, 24.5f, 28.5f, 32.7f };
  if (!isfinite(ms)) return 0;
  for (int i = 0; i < 12; i++) if (ms < up[i]) return i;
  return 12;
}

const char *ru_wx_beaufort_word(int b) {
  static const char *w[13] = { "Calm", "Light air", "Light breeze", "Gentle breeze", "Moderate breeze", "Fresh breeze", "Strong breeze",
                               "Near gale", "Gale", "Strong gale", "Storm", "Violent storm", "Hurricane" };
  return w[b < 0 ? 0 : b > 12 ? 12 : b];
}

ru_wx_color_t ru_wx_wind_band(float v, const char *unit) {
  float ms;
  if (!ru_wx_wind_ms(v, unit, &ms)) return RU_WX_NONE;
  if (ms < 1.5f) return RU_WX_DIM;
  if (ms < 5.5f) return RU_WX_BLUE;
  if (ms < 10.8f) return RU_WX_TEAL;
  if (ms < 17.2f) return RU_WX_AMBER;
  return RU_WX_RED;
}

ru_wx_color_t ru_wx_rain_band(float rate) {
  if (!isfinite(rate) || rate < 0.0f) return RU_WX_NONE;
  if (rate <= 0.0f) return RU_WX_DIM;
  if (rate < 2.5f) return RU_WX_BLUE;
  if (rate < 10.0f) return RU_WX_INDIGO;
  return RU_WX_VIOLET;
}

void ru_wx_icon_utf8(ru_wx_icon_t icon, char out[4]) {
  if (icon < 0 || icon >= RU_WX_ICON_COUNT) { out[0] = 0; return; }
  unsigned cp = 0xE000u + (unsigned)icon;
  out[0] = (char)(0xE0 | (cp >> 12));
  out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[2] = (char)(0x80 | (cp & 0x3F));
  out[3] = 0;
}

ru_wx_icon_t ru_wx_wind_arrow(float from_deg) {
  if (!isfinite(from_deg)) return RU_WX_ICON_NONE;
  float to = fmodf(from_deg + 180.0f, 360.0f);
  if (to < 0.0f) to += 360.0f;
  int k = ((int)lroundf(to / 45.0f)) & 7;
  return (ru_wx_icon_t)(RU_WX_ICON_ARROW_N + k);
}

ru_wx_icon_t ru_wx_forecast_icon(const char *f) {
  if (!f || !f[0]) return RU_WX_ICON_GAUGE;
  if (icontains(f, "thunder") || icontains(f, "storm")) return RU_WX_ICON_CLOUD_LIGHTNING;
  if (icontains(f, "snow")) return RU_WX_ICON_CLOUD_SNOW;
  if (icontains(f, "rain") || icontains(f, "shower")) return RU_WX_ICON_CLOUD_RAIN;
  if (icontains(f, "partly")) return RU_WX_ICON_CLOUD_SUN;
  if (icontains(f, "cloud")) return RU_WX_ICON_CLOUD;
  if (icontains(f, "sun") || icontains(f, "clear") || icontains(f, "fair")) return RU_WX_ICON_SUN;
  if (icontains(f, "unstable")) return RU_WX_ICON_CLOUD_DRIZZLE;
  return RU_WX_ICON_GAUGE; /* "Stable", "Unknown" */
}

/* ---- tile text ---- */

static void split_text(const dz_reading_t *r, const ru_cl_units_t *u, char *val, size_t vn, char *unit, size_t un) {
  ru_cl_val_t v;
  ru_cl_fmt(r, u, &v);
  snprintf(val, vn, "%s", v.num);
  bool attach = !v.unit[0] || v.unit[0] == '%' || !strncmp(v.unit, DEG, 2);
  snprintf(unit, un, attach ? "%s" : " %s", v.unit);
}

static void add_trend(char *unit, size_t un, int trend) {
  const char *t = trend == DZ_TREND_UP ? RU_WX_TREND_UP : trend == DZ_TREND_DOWN ? RU_WX_TREND_DOWN : NULL;
  size_t l = strlen(unit);
  if (t && l + 5 < un) snprintf(unit + l, un - l, " %s", t);
}

static bool rd_ok(const dz_reading_t *r) { return r && r->has && isfinite(r->v); }

void ru_wx_tile_build(const dz_device_t *d, const ru_cl_units_t *u, ru_wx_tile_t *o) {
  memset(o, 0, sizeof *o);
  o->icon = RU_WX_ICON_NONE;
  snprintf(o->name, sizeof o->name, "%s", d->name);
  snprintf(o->value, sizeof o->value, "--");
  switch (d->n_rd ? ru_cl_page_of(d) : RU_CLP_NONE) {
    case RU_CLP_TEMP: {
      const dz_reading_t *r = dz_dev_room_temp(d), *h = dz_dev_reading(d, DZ_RK_HUM);
      split_text(r, u, o->value, sizeof o->value, o->unit, sizeof o->unit);
      add_trend(o->unit, sizeof o->unit, d->trend);
      o->known = rd_ok(r);
      o->icon = RU_WX_ICON_THERMOMETER;
      o->icon_color = o->known ? ru_wx_temp_band(r->v, u->temp) : RU_WX_NONE;
      if (rd_ok(h)) ru_cl_text(h, u, o->second, sizeof o->second);
      break;
    }
    case RU_CLP_HUM: {
      const dz_reading_t *h = dz_dev_reading(d, DZ_RK_HUM);
      split_text(h, u, o->value, sizeof o->value, o->unit, sizeof o->unit);
      o->known = rd_ok(h);
      o->icon = RU_WX_ICON_DROPLET;
      o->icon_color = ru_wx_hum_band(d->hum_status, o->known, o->known ? h->v : 0.0f);
      snprintf(o->second, sizeof o->second, "%s", d->hum_status);
      break;
    }
    case RU_CLP_WIND: {
      const dz_reading_t *s = dz_dev_reading(d, DZ_RK_WIND), *g = dz_dev_reading(d, DZ_RK_GUST), *dir = dz_dev_reading(d, DZ_RK_DIR);
      o->icon = RU_WX_ICON_WIND;
      if (rd_ok(dir)) o->icon = ru_wx_wind_arrow(dir->v);
      if (rd_ok(s)) {
        split_text(s, u, o->value, sizeof o->value, o->unit, sizeof o->unit);
        o->known = true;
        o->icon_color = ru_wx_wind_band(s->v, u->wind);
      } else if (d->dir_str[0]) {
        snprintf(o->value, sizeof o->value, "%s", d->dir_str);
        o->known = true;
      }
      char gust[20] = "";
      if (rd_ok(g)) { ru_cl_val_t gv; ru_cl_fmt(g, u, &gv); snprintf(gust, sizeof gust, "G %s", gv.num); }
      if (d->dir_str[0] && rd_ok(s)) snprintf(o->second, sizeof o->second, "%s%s%s", d->dir_str, gust[0] ? "  " : "", gust);
      else snprintf(o->second, sizeof o->second, "%s", gust);
      if (!o->second[0] && rd_ok(s)) { float ms; if (ru_wx_wind_ms(s->v, u->wind, &ms)) snprintf(o->second, sizeof o->second, "%s", ru_wx_beaufort_word(ru_wx_beaufort(ms))); }
      break;
    }
    case RU_CLP_RAIN: {
      const dz_reading_t *tot = dz_dev_reading(d, DZ_RK_RAIN), *rate = dz_dev_reading(d, DZ_RK_RAINRATE);
      const dz_reading_t *hero = rd_ok(tot) ? tot : rate;
      split_text(hero, u, o->value, sizeof o->value, o->unit, sizeof o->unit);
      o->known = rd_ok(hero);
      if (rd_ok(rate)) {
        o->icon = rate->v >= 7.6f ? RU_WX_ICON_CLOUD_RAIN : rate->v > 0.0f ? RU_WX_ICON_CLOUD_DRIZZLE : RU_WX_ICON_CLOUD;
        o->icon_color = ru_wx_rain_band(rate->v);
        if (rd_ok(tot)) { ru_cl_val_t rv; ru_cl_fmt(rate, u, &rv); snprintf(o->second, sizeof o->second, "RATE %s", rv.num); }
      } else {
        o->icon = RU_WX_ICON_CLOUD; /* a total says nothing about rain right now */
        o->icon_color = RU_WX_DIM;
      }
      break;
    }
    case RU_CLP_UV: {
      const dz_reading_t *r = dz_dev_reading(d, DZ_RK_UVI);
      o->known = rd_ok(r);
      ru_cl_val_t v;
      ru_cl_fmt(r, u, &v);
      snprintf(o->value, sizeof o->value, "%s", v.num);
      snprintf(o->unit, sizeof o->unit, " UV");
      o->icon = o->known && r->v >= 3.0f ? RU_WX_ICON_SUN : RU_WX_ICON_SUN_DIM;
      o->icon_color = o->known ? ru_wx_uv_band(r->v) : RU_WX_NONE;
      if (o->known) { snprintf(o->second, sizeof o->second, "%s", ru_cl_uv_word(r->v)); o->second_color = o->icon_color; }
      break;
    }
    case RU_CLP_AIR: {
      const dz_reading_t *r = dz_dev_reading(d, DZ_RK_AIRQ);
      split_text(r, u, o->value, sizeof o->value, o->unit, sizeof o->unit);
      o->known = rd_ok(r);
      o->icon = RU_WX_ICON_CLOUD_FOG;
      snprintf(o->second, sizeof o->second, "%s", d->quality);
      break;
    }
    default: {
      /* a barometer, or any other single value: its own number, its own unit */
      const dz_reading_t *b = dz_dev_reading(d, DZ_RK_BARO);
      if (rd_ok(b)) {
        split_text(b, u, o->value, sizeof o->value, o->unit, sizeof o->unit);
        o->known = true;
      } else if (d->has_baro && isfinite(d->baro)) {
        snprintf(o->value, sizeof o->value, "%.0f", (double)d->baro);
        o->known = true;
      }
      if (o->known) {
        add_trend(o->unit, sizeof o->unit, d->trend);
        o->icon = RU_WX_ICON_GAUGE;
        /* ForecastStr is a pressure heuristic, not an observation: only when the device itself provides it, and it says so */
        if (d->forecast[0] && !ieq(d->forecast, "Unknown")) {
          ru_wx_icon_t f = ru_wx_forecast_icon(d->forecast);
          o->icon = f;
          if (f != RU_WX_ICON_GAUGE) snprintf(o->second, sizeof o->second, "FORECAST");
        }
      } else if (d->n_rd) {
        char t[sizeof o->value];
        bool k;
        ru_cl_summary(d, u, t, sizeof t, &k); /* lux, text, anything else: its own summary, no icon */
        snprintf(o->value, sizeof o->value, "%s", t);
        o->known = k;
      }
      break;
    }
  }
}
