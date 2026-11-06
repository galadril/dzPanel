#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini_test.h"
#include "ru_wx.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "tests/fixtures"
#endif
#ifndef MANIFEST_PATH
#define MANIFEST_PATH "tools/icons/manifest.txt"
#endif

#define DEG "\xC2\xB0"

static double lin(double c) { c /= 255.0; return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
static double lum(uint32_t rgb) { return 0.2126 * lin((rgb >> 16) & 255) + 0.7152 * lin((rgb >> 8) & 255) + 0.0722 * lin(rgb & 255); }
static double contrast(uint32_t a, uint32_t b) { double la = lum(a), lb = lum(b); if (la < lb) { double t = la; la = lb; lb = t; } return (la + 0.05) / (lb + 0.05); }

static void test_colors(void) {
  for (int c = 0; c < RU_WX_COLORS; c++) CHECK(contrast(ru_wx_color_rgb((ru_wx_color_t)c), 0x0E1013) >= 3.0);
  CHECK(ru_wx_color_rgb(RU_WX_NONE) == 0xE6E9EE && ru_wx_color_rgb(RU_WX_AMBER) == 0xFFC247 && ru_wx_color_rgb(RU_WX_VIOLET) == 0x8C7CFF);
  CHECK(ru_wx_color_rgb((ru_wx_color_t)99) == 0xE6E9EE);
}

static void test_temp(void) {
  struct { float v; const char *u; ru_wx_color_t c; } t[] = {
    {-20, DEG "C", RU_WX_ICE}, {0, DEG "C", RU_WX_ICE}, {0.1f, DEG "C", RU_WX_BLUE}, {9.9f, DEG "C", RU_WX_BLUE},
    {10, DEG "C", RU_WX_TEAL}, {17.9f, DEG "C", RU_WX_TEAL}, {18, DEG "C", RU_WX_GREEN}, {23.9f, DEG "C", RU_WX_GREEN},
    {24, DEG "C", RU_WX_AMBER}, {29.9f, DEG "C", RU_WX_AMBER}, {30, DEG "C", RU_WX_ORANGE}, {34.9f, DEG "C", RU_WX_ORANGE},
    {35, DEG "C", RU_WX_RED}, {60, DEG "C", RU_WX_RED},
    /* Fahrenheit is converted: 32F = 0C, 50F = 10C, 64.4F = 18C, 75.2F = 24C, 95F = 35C */
    {32, DEG "F", RU_WX_ICE}, {32.5f, DEG "F", RU_WX_BLUE}, {49.9f, DEG "F", RU_WX_BLUE}, {50.1f, DEG "F", RU_WX_TEAL},
    {64.5f, DEG "F", RU_WX_GREEN}, {75.3f, DEG "F", RU_WX_AMBER}, {86.1f, DEG "F", RU_WX_ORANGE}, {95, DEG "F", RU_WX_RED},
    {21, "C", RU_WX_GREEN}, {70, "F", RU_WX_GREEN},
    /* an unknown unit is no reason to guess a colour */
    {21, "", RU_WX_NONE}, {21, "K", RU_WX_NONE}, {21, DEG, RU_WX_NONE},
  };
  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
    if (ru_wx_temp_band(t[i].v, t[i].u) != t[i].c) printf("temp %g %s -> %d want %d\n", (double)t[i].v, t[i].u, ru_wx_temp_band(t[i].v, t[i].u), t[i].c);
    CHECK(ru_wx_temp_band(t[i].v, t[i].u) == t[i].c);
  }
  CHECK(ru_wx_temp_band(NAN, DEG "C") == RU_WX_NONE && ru_wx_temp_band(21, NULL) == RU_WX_NONE);
}

static void test_hum_uv_rain(void) {
  CHECK(ru_wx_hum_band("Dry", true, 80) == RU_WX_AMBER && ru_wx_hum_band("Wet", true, 10) == RU_WX_BLUE);
  CHECK(ru_wx_hum_band("Comfortable", true, 10) == RU_WX_GREEN && ru_wx_hum_band("Normal", false, 0) == RU_WX_GREEN && ru_wx_hum_band("dry", false, 0) == RU_WX_AMBER);
  CHECK(ru_wx_hum_band("", true, 29.9f) == RU_WX_AMBER && ru_wx_hum_band("", true, 30) == RU_WX_GREEN && ru_wx_hum_band("", true, 70) == RU_WX_GREEN &&
        ru_wx_hum_band(NULL, true, 70.1f) == RU_WX_BLUE && ru_wx_hum_band("Unheard", true, 50) == RU_WX_GREEN);
  CHECK(ru_wx_hum_band("", false, 0) == RU_WX_NONE && ru_wx_hum_band("", true, NAN) == RU_WX_NONE);
  float uv[] = { 0, 2.9f, 3, 5.9f, 6, 7.9f, 8, 10.9f, 11, 14 };
  ru_wx_color_t uc[] = { RU_WX_GREEN, RU_WX_GREEN, RU_WX_AMBER, RU_WX_AMBER, RU_WX_ORANGE, RU_WX_ORANGE, RU_WX_RED, RU_WX_RED, RU_WX_VIOLET, RU_WX_VIOLET };
  for (size_t i = 0; i < sizeof uv / sizeof uv[0]; i++) CHECK(ru_wx_uv_band(uv[i]) == uc[i]);
  CHECK(ru_wx_uv_band(-1) == RU_WX_NONE && ru_wx_uv_band(NAN) == RU_WX_NONE);
  CHECK(ru_wx_rain_band(0) == RU_WX_DIM && ru_wx_rain_band(0.01f) == RU_WX_BLUE && ru_wx_rain_band(2.49f) == RU_WX_BLUE && ru_wx_rain_band(2.5f) == RU_WX_INDIGO &&
        ru_wx_rain_band(9.9f) == RU_WX_INDIGO && ru_wx_rain_band(10) == RU_WX_VIOLET && ru_wx_rain_band(-1) == RU_WX_NONE && ru_wx_rain_band(NAN) == RU_WX_NONE);
}

static void test_wind(void) {
  float ms;
  CHECK(ru_wx_wind_ms(10, "m/s", &ms) && fabsf(ms - 10) < 1e-4f);
  CHECK(ru_wx_wind_ms(36, "km/h", &ms) && fabsf(ms - 10) < 1e-3f);
  CHECK(ru_wx_wind_ms(10, "mph", &ms) && fabsf(ms - 4.4704f) < 1e-3f);
  CHECK(ru_wx_wind_ms(10, "kn", &ms) && fabsf(ms - 5.14444f) < 1e-3f);
  CHECK(ru_wx_wind_ms(6, "bft", &ms) && ms > 10.8f && ms < 13.9f);
  CHECK(ru_wx_wind_ms(99, "bft", &ms) && ms > 32.7f);
  CHECK(!ru_wx_wind_ms(5, "", &ms) && !ru_wx_wind_ms(5, NULL, &ms) && !ru_wx_wind_ms(5, "furlong", &ms) && !ru_wx_wind_ms(NAN, "m/s", &ms) && !ru_wx_wind_ms(-1, "m/s", &ms));
  /* Beaufort limits in m/s, and the words */
  struct { float ms; int b; } bf[] = { {0, 0}, {0.49f, 0}, {0.5f, 1}, {1.59f, 1}, {1.6f, 2}, {3.4f, 3}, {5.5f, 4}, {8, 5}, {10.8f, 6}, {13.9f, 7}, {17.2f, 8}, {20.8f, 9}, {24.5f, 10}, {28.5f, 11}, {32.7f, 12}, {50, 12} };
  for (size_t i = 0; i < sizeof bf / sizeof bf[0]; i++) CHECK(ru_wx_beaufort(bf[i].ms) == bf[i].b);
  CHECK(!strcmp(ru_wx_beaufort_word(0), "Calm") && !strcmp(ru_wx_beaufort_word(8), "Gale") && !strcmp(ru_wx_beaufort_word(12), "Hurricane") && !strcmp(ru_wx_beaufort_word(40), "Hurricane"));
  /* the colour follows m/s whatever the unit: 1.4 m/s calm, 5.4 light, 5.5 fresh, 10.8 strong, 17.2 gale */
  struct { float v; const char *u; ru_wx_color_t c; } w[] = {
    {1.4f, "m/s", RU_WX_DIM}, {1.5f, "m/s", RU_WX_BLUE}, {5.4f, "m/s", RU_WX_BLUE}, {5.5f, "m/s", RU_WX_TEAL}, {10.7f, "m/s", RU_WX_TEAL},
    {10.8f, "m/s", RU_WX_AMBER}, {17.1f, "m/s", RU_WX_AMBER}, {17.2f, "m/s", RU_WX_RED},
    {5.0f, "km/h", RU_WX_DIM}, {6.0f, "km/h", RU_WX_BLUE}, {20, "km/h", RU_WX_TEAL}, {40, "km/h", RU_WX_AMBER}, {62, "km/h", RU_WX_RED},
    {3, "mph", RU_WX_DIM}, {4, "mph", RU_WX_BLUE}, {12.4f, "mph", RU_WX_TEAL}, {24.2f, "mph", RU_WX_AMBER}, {38.5f, "mph", RU_WX_RED},
    {2, "kn", RU_WX_DIM}, {3, "kn", RU_WX_BLUE}, {11, "kn", RU_WX_TEAL}, {21, "kn", RU_WX_AMBER}, {34, "kn", RU_WX_RED},
    {0, "bft", RU_WX_DIM}, {2, "bft", RU_WX_BLUE}, {5, "bft", RU_WX_TEAL}, {7, "bft", RU_WX_AMBER}, {9, "bft", RU_WX_RED},
    {5, "", RU_WX_NONE}, {5, "x", RU_WX_NONE},
  };
  for (size_t i = 0; i < sizeof w / sizeof w[0]; i++) {
    if (ru_wx_wind_band(w[i].v, w[i].u) != w[i].c) printf("wind %g %s -> %d want %d\n", (double)w[i].v, w[i].u, ru_wx_wind_band(w[i].v, w[i].u), w[i].c);
    CHECK(ru_wx_wind_band(w[i].v, w[i].u) == w[i].c);
  }
}

static void test_icons(void) {
  /* the enum follows tools/icons/manifest.txt, so the code point is 0xE000 + value */
  FILE *f = fopen(MANIFEST_PATH, "r");
  CHECK(f != NULL);
  if (f) {
    char ln[128];
    int n = 0;
    while (fgets(ln, sizeof ln, f)) {
      unsigned cp;
      if (ln[0] == '#' || sscanf(ln, "%x", &cp) != 1) continue;
      CHECK(cp == 0xE000u + (unsigned)n);
      n++;
    }
    fclose(f);
    CHECK(n == RU_WX_ICON_COUNT);
  }
  char u[4];
  ru_wx_icon_utf8(RU_WX_ICON_THERMOMETER, u);
  CHECK(!strcmp(u, "\xEE\x80\x80"));
  ru_wx_icon_utf8(RU_WX_ICON_ARROW_NW, u);
  CHECK(!strcmp(u, "\xEE\x80\x97"));
  ru_wx_icon_utf8(RU_WX_ICON_NONE, u);
  CHECK(u[0] == 0);
  /* wind FROM the north blows TO the south: the arrow points down */
  CHECK(ru_wx_wind_arrow(0) == RU_WX_ICON_ARROW_S && ru_wx_wind_arrow(90) == RU_WX_ICON_ARROW_W && ru_wx_wind_arrow(180) == RU_WX_ICON_ARROW_N &&
        ru_wx_wind_arrow(270) == RU_WX_ICON_ARROW_E && ru_wx_wind_arrow(315) == RU_WX_ICON_ARROW_SE && ru_wx_wind_arrow(360) == RU_WX_ICON_ARROW_S &&
        ru_wx_wind_arrow(-45) == RU_WX_ICON_ARROW_SE && ru_wx_wind_arrow(NAN) == RU_WX_ICON_NONE);
  CHECK(ru_wx_forecast_icon("Sunny") == RU_WX_ICON_SUN && ru_wx_forecast_icon("Partly cloudy") == RU_WX_ICON_CLOUD_SUN && ru_wx_forecast_icon("Cloudy") == RU_WX_ICON_CLOUD &&
        ru_wx_forecast_icon("Rain") == RU_WX_ICON_CLOUD_RAIN && ru_wx_forecast_icon("Cloudy/Rain") == RU_WX_ICON_CLOUD_RAIN && ru_wx_forecast_icon("Thunderstorm") == RU_WX_ICON_CLOUD_LIGHTNING &&
        ru_wx_forecast_icon("Stable") == RU_WX_ICON_GAUGE && ru_wx_forecast_icon("Unknown") == RU_WX_ICON_GAUGE && ru_wx_forecast_icon("") == RU_WX_ICON_GAUGE && ru_wx_forecast_icon(NULL) == RU_WX_ICON_GAUGE &&
        ru_wx_forecast_icon("Unstable") == RU_WX_ICON_CLOUD_DRIZZLE);
}

/* ---- per-kind tiles from one device's own readings ---- */
static dz_device_t dev(const char *name, int n, dz_reading_t *r) {
  dz_device_t d;
  memset(&d, 0, sizeof d);
  snprintf(d.idx, sizeof d.idx, "1");
  snprintf(d.name, sizeof d.name, "%s", name);
  d.cap = DZ_CAP_WEATHER;
  for (int i = 0; i < n; i++) d.rd[i] = r[i];
  d.n_rd = n;
  return d;
}
static dz_reading_t rd(dz_rk_t k, float v, const char *unit) {
  dz_reading_t r;
  memset(&r, 0, sizeof r);
  r.kind = k; r.has = true; r.v = v;
  snprintf(r.unit, sizeof r.unit, "%s", unit);
  return r;
}

static void test_tiles(void) {
  ru_cl_units_t cu;
  memset(&cu, 0, sizeof cu);
  snprintf(cu.temp, sizeof cu.temp, DEG "C");
  snprintf(cu.wind, sizeof cu.wind, "m/s");
  ru_wx_tile_t t;
  /* temperature + humidity: thermometer, band colour, humidity as the small line, trend after the unit */
  dz_reading_t a[] = { rd(DZ_RK_TEMP, 21.5f, ""), rd(DZ_RK_HUM, 45, "%") };
  dz_device_t d = dev("Living room", 2, a);
  d.trend = DZ_TREND_UP;
  ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_THERMOMETER && t.icon_color == RU_WX_GREEN && !strcmp(t.value, "21.5") && t.known);
  CHECK(!strcmp(t.unit, DEG "C " RU_WX_TREND_UP) && !strcmp(t.second, "45%") && !strcmp(t.name, "Living room"));
  d.trend = DZ_TREND_DOWN;
  a[0].v = -3;
  d = dev("Living room", 2, a);
  d.trend = DZ_TREND_DOWN;
  ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon_color == RU_WX_ICE && !strcmp(t.value, "-3.0") && !strcmp(t.unit, DEG "C " RU_WX_TREND_DOWN));
  /* unknown temperature unit: the number, no colour, no unit */
  ru_cl_units_t nu = cu;
  nu.temp[0] = 0;
  d = dev("Probe", 1, a);
  ru_wx_tile_build(&d, &nu, &t);
  CHECK(t.icon == RU_WX_ICON_THERMOMETER && t.icon_color == RU_WX_NONE && !strcmp(t.unit, ""));
  /* Fahrenheit install */
  ru_cl_units_t fu = cu;
  snprintf(fu.temp, sizeof fu.temp, DEG "F");
  a[0] = rd(DZ_RK_TEMP, 95, "");
  d = dev("Attic", 1, a);
  ru_wx_tile_build(&d, &fu, &t);
  CHECK(t.icon_color == RU_WX_RED && !strcmp(t.unit, DEG "F"));
  /* humidity only: droplet, HumidityStatus first */
  dz_reading_t h[] = { rd(DZ_RK_HUM, 20, "%") };
  d = dev("Cellar", 1, h);
  snprintf(d.hum_status, sizeof d.hum_status, "Wet");
  ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_DROPLET && t.icon_color == RU_WX_BLUE && !strcmp(t.value, "20") && !strcmp(t.unit, "%") && !strcmp(t.second, "Wet"));
  /* rain gauge: rate 0 dim cloud, > 0 drizzle, >= 7.6 pouring; the value is the total, nothing combined with another device */
  dz_reading_t r[] = { rd(DZ_RK_RAIN, 12.4f, ""), rd(DZ_RK_RAINRATE, 0, "") };
  d = dev("Gauge", 2, r);
  ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_CLOUD && t.icon_color == RU_WX_DIM && !strcmp(t.value, "12.4") && !strcmp(t.second, "RATE 0"));
  r[1].v = 1.2f; d = dev("Gauge", 2, r); ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_CLOUD_DRIZZLE && t.icon_color == RU_WX_BLUE);
  r[1].v = 7.6f; d = dev("Gauge", 2, r); ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_CLOUD_RAIN && t.icon_color == RU_WX_INDIGO);
  r[1].v = 12; d = dev("Gauge", 2, r); ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_CLOUD_RAIN && t.icon_color == RU_WX_VIOLET);
  d = dev("Gauge", 1, r); /* a total alone says nothing about the rain right now */
  ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_CLOUD && t.icon_color == RU_WX_DIM && !strcmp(t.value, "12.4") && !t.second[0]);
  /* UV: 1, 4, 9, 12 */
  float uvv[] = { 1, 4, 9, 12 };
  ru_wx_icon_t ui[] = { RU_WX_ICON_SUN_DIM, RU_WX_ICON_SUN, RU_WX_ICON_SUN, RU_WX_ICON_SUN };
  ru_wx_color_t uc[] = { RU_WX_GREEN, RU_WX_AMBER, RU_WX_RED, RU_WX_VIOLET };
  const char *uw[] = { "LOW", "MODERATE", "VERY HIGH", "EXTREME" };
  for (int i = 0; i < 4; i++) {
    dz_reading_t u1[] = { rd(DZ_RK_UVI, uvv[i], "") };
    d = dev("UV", 1, u1);
    ru_wx_tile_build(&d, &cu, &t);
    CHECK(t.icon == ui[i] && t.icon_color == uc[i] && t.second_color == uc[i] && !strcmp(t.second, uw[i]));
  }
  /* wind: calm and gale; arrow from the bearing, the gust and the compass word on the small line */
  dz_reading_t w[] = { rd(DZ_RK_WIND, 0.4f, ""), rd(DZ_RK_GUST, 6.1f, ""), rd(DZ_RK_DIR, 315, "") };
  d = dev("Mast", 3, w);
  snprintf(d.dir_str, sizeof d.dir_str, "NW");
  ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_ARROW_SE && t.icon_color == RU_WX_DIM && !strcmp(t.value, "0.4") && !strcmp(t.unit, " m/s") && !strcmp(t.second, "NW  G 6.1"));
  w[0].v = 20; d = dev("Mast", 3, w); ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon_color == RU_WX_RED);
  dz_reading_t w2[] = { rd(DZ_RK_WIND, 3, "") };
  d = dev("Mast", 1, w2);
  ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_WIND && t.icon_color == RU_WX_BLUE && !strcmp(t.second, "Light breeze"));
  ru_cl_units_t wu = cu;
  wu.wind[0] = 0; /* unknown wind unit: no colour, no unit, no Beaufort word */
  ru_wx_tile_build(&d, &wu, &t);
  CHECK(t.icon_color == RU_WX_NONE && !strcmp(t.unit, "") && !t.second[0]);
  /* barometer: its own forecast only; gauge otherwise; the forecast icon says FORECAST */
  dz_reading_t b[] = { rd(DZ_RK_BARO, 1013, "hPa") };
  d = dev("Baro", 1, b);
  ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_GAUGE && t.icon_color == RU_WX_NONE && !strcmp(t.value, "1013") && !strcmp(t.unit, " hPa") && !t.second[0]);
  snprintf(d.forecast, sizeof d.forecast, "Cloudy");
  ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_CLOUD && !strcmp(t.second, "FORECAST"));
  snprintf(d.forecast, sizeof d.forecast, "Stable");
  ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_GAUGE && !t.second[0]);
  /* a device with no readings at all: no icon, "--", unknown */
  d = dev("Hand made", 0, NULL);
  ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_NONE && !t.known && !strcmp(t.value, "--"));
  /* the temperature reading of a wind sensor's own thermometer (AUXTEMP) is not the sensor's headline */
  dz_reading_t ww[] = { rd(DZ_RK_WIND, 3, ""), rd(DZ_RK_AUXTEMP, 40, "") };
  d = dev("Mast", 2, ww);
  ru_wx_tile_build(&d, &cu, &t);
  CHECK(t.icon == RU_WX_ICON_WIND);
}

int main(void) {
  test_colors();
  test_temp();
  test_hum_uv_rain();
  test_wind();
  test_icons();
  test_tiles();
  MT_DONE();
}
