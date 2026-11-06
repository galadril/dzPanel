/* Weather tile view model: colour bands, icon choice and tile text for the Temperature and Weather grids. Pure C, no LVGL,
 * host-tested (tests/test_apps_wx.c). Every device is judged by ITS OWN readings only: nothing is combined across devices,
 * and there is no "outdoor device". Domoticz data only: no forecast service. A band never colours text; the widget colours
 * the icon (and the UV word) with it, the value stays white.
 *
 * Bands (stepped, every colour is at least 3:1 on the tile surface #0E1013):
 *   temperature, degrees C (F converted; unknown unit -> NONE): <=0 ICE, <10 BLUE, <18 TEAL, <24 GREEN, <30 AMBER, <35 ORANGE, else RED
 *   humidity: HumidityStatus first (Dry AMBER, Comfortable/Normal GREEN, Wet BLUE), else value: <30 AMBER, >70 BLUE, else GREEN
 *   UV index (WHO): <3 GREEN, <6 AMBER, <8 ORANGE, <11 RED, else VIOLET
 *   wind, in m/s (converted from the unit Domoticz gave; unknown unit -> NONE): <1.5 DIM, <5.5 BLUE, <10.8 TEAL, <17.2 AMBER, else RED
 *   rain RATE, ASSUMED mm/h (Domoticz gives no unit): 0 DIM, >0 BLUE, >=2.5 INDIGO, >=10 VIOLET
 *   barometer: no band (text colour), the icon follows ForecastStr when the device itself provides it */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dz_parse.h"
#include "ru_climate.h"

typedef enum {
  RU_WX_NONE = 0, /* no colour: the theme text colour */
  RU_WX_DIM,      /* #8A93A0 */
  RU_WX_ICE,      /* #A8D8FF */
  RU_WX_BLUE,     /* #4DB3FF */
  RU_WX_TEAL,     /* #3DE0C5 */
  RU_WX_GREEN,    /* #3DDC84 */
  RU_WX_AMBER,    /* #FFC247 */
  RU_WX_ORANGE,   /* #FF8A3D */
  RU_WX_RED,      /* #FF5A5F */
  RU_WX_INDIGO,   /* #4D7CFF */
  RU_WX_VIOLET,   /* #8C7CFF */
  RU_WX_COLORS
} ru_wx_color_t;

uint32_t ru_wx_color_rgb(ru_wx_color_t c); /* 0xRRGGBB; NONE gives the text colour #E6E9EE */

ru_wx_color_t ru_wx_temp_band(float v, const char *unit);          /* unit "°C" / "°F" (also "C" / "F") */
ru_wx_color_t ru_wx_hum_band(const char *status, bool has_v, float v);
ru_wx_color_t ru_wx_uv_band(float v);
ru_wx_color_t ru_wx_wind_band(float v, const char *unit);
ru_wx_color_t ru_wx_rain_band(float rate_mm_h);

/* Wind speed to m/s. unit: "m/s" "km/h" "mph" "kn" "bft" (a few spellings of each). False when the unit is unknown or v is not finite. */
bool ru_wx_wind_ms(float v, const char *unit, float *ms);
int ru_wx_beaufort(float ms);          /* 0..12 */
const char *ru_wx_beaufort_word(int b); /* "Calm" ... "Hurricane" */

/* The icons: same order as tools/icons/manifest.txt, so codepoint = 0xE000 + value. */
typedef enum {
  RU_WX_ICON_NONE = -1,
  RU_WX_ICON_THERMOMETER = 0, RU_WX_ICON_DROPLET, RU_WX_ICON_GAUGE, RU_WX_ICON_SUN, RU_WX_ICON_SUN_DIM, RU_WX_ICON_MOON,
  RU_WX_ICON_CLOUD_SUN, RU_WX_ICON_CLOUD, RU_WX_ICON_CLOUD_RAIN, RU_WX_ICON_CLOUD_DRIZZLE, RU_WX_ICON_CLOUD_SNOW,
  RU_WX_ICON_SNOWFLAKE, RU_WX_ICON_WIND, RU_WX_ICON_UMBRELLA, RU_WX_ICON_CLOUD_LIGHTNING, RU_WX_ICON_CLOUD_FOG,
  RU_WX_ICON_ARROW_N, RU_WX_ICON_ARROW_NE, RU_WX_ICON_ARROW_E, RU_WX_ICON_ARROW_SE, RU_WX_ICON_ARROW_S, RU_WX_ICON_ARROW_SW,
  RU_WX_ICON_ARROW_W, RU_WX_ICON_ARROW_NW, RU_WX_ICON_THERMOMETER_SUN,
  RU_WX_ICON_COUNT
} ru_wx_icon_t;
/* The icon as UTF-8 for a label with the icon font: out needs 4 bytes. Empty string for NONE. */
void ru_wx_icon_utf8(ru_wx_icon_t icon, char out[4]);
/* The arrow that shows where the wind blows TO, for a bearing it blows FROM (0 = from north: arrow pointing down). */
ru_wx_icon_t ru_wx_wind_arrow(float from_deg);
/* ForecastStr of a barometer -> icon; GAUGE for "Stable", "Unknown", empty and anything unrecognised. */
ru_wx_icon_t ru_wx_forecast_icon(const char *forecast);

/* What one grid tile shows, from one device's own readings. */
typedef struct {
  ru_wx_icon_t icon;
  ru_wx_color_t icon_color;
  char name[40];   /* as Domoticz names it (already clamped valid UTF-8) */
  char value[16];  /* "21.5", "1013", "--" */
  char unit[24];   /* "°C", " m/s", " hPa", plus a trend chevron; "" for none */
  char second[32]; /* small dim line: "45%", "NW  G 6.1", "MODERATE", "FORECAST"; "" for none */
  ru_wx_color_t second_color; /* NONE = dim */
  bool known;      /* the value is a real reading */
} ru_wx_tile_t;
void ru_wx_tile_build(const dz_device_t *d, const ru_cl_units_t *u, ru_wx_tile_t *out);

/* LV_SYMBOL_UP / LV_SYMBOL_DOWN as UTF-8 (the Montserrat chevrons): the trend after the unit. */
#define RU_WX_TREND_UP "\xEF\x81\xB7"
#define RU_WX_TREND_DOWN "\xEF\x81\xB8"
