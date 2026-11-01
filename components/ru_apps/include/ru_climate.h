/* Climate view model: turns a device's READINGS (dz_reading_t) into the words and numbers a screen shows. Pure C, no
 * LVGL, host-tested. Screens render readings, not Domoticz types, so a type nobody has seen still shows.
 *
 * Units rule (never print a unit that was not read):
 *   temperature, dew point, wind chill: TempSign from getconfig ("°C"/"°F"), or nothing when unknown
 *   wind speed, gust:                   WindSign from getconfig, or nothing when unknown
 *   humidity: "%". Everything else: the unit text Domoticz put in the reading, or nothing.
 * A missing unit means the bare number is shown. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "dz_model.h"
#include "dz_parse.h"

typedef struct {
  char temp[8]; /* "°C" or "°F" (UTF-8), "" = unknown */
  char wind[8]; /* "m/s", "km/h", "mph", "kn", "bft", "" = unknown */
} ru_cl_units_t;
void ru_cl_units(const dz_units_t *u, ru_cl_units_t *out);

/* One reading as text: num "21.5", unit "°C" (may be empty). known=false -> num is "--". */
typedef struct { bool known; char num[16]; char unit[12]; } ru_cl_val_t;
void ru_cl_fmt(const dz_reading_t *r, const ru_cl_units_t *u, ru_cl_val_t *out);
/* "21.5°C", "3.4 m/s", "850 ppm", or "21.5" without a unit. "--" when r is NULL. */
void ru_cl_text(const dz_reading_t *r, const ru_cl_units_t *u, char *out, size_t n);

/* The screens a device can get. One device is one page. */
/* RU_CLP_SETPOINT is filled by the screen code for setpoint devices (see ru_cl_thermo); ru_cl_page_of never returns it. */
typedef enum { RU_CLP_NONE, RU_CLP_TEMP, RU_CLP_HUM, RU_CLP_WIND, RU_CLP_RAIN, RU_CLP_UV, RU_CLP_AIR, RU_CLP_VALUE, RU_CLP_TEXT, RU_CLP_SETPOINT } ru_cl_page_t;
ru_cl_page_t ru_cl_page_of(const dz_device_t *d);

typedef struct { char text[48]; bool warn; } ru_cl_line_t;
#define RU_CL_LINES 3

typedef struct {
  ru_cl_page_t page;
  bool known;               /* the hero has a value */
  char name[40];            /* upper case, clamped */
  char hero[24];            /* the big text: "21.5°C", "71%", "5", "123" (empty for WIND/TEXT pages) */
  char caption[20];         /* "TEMP", "HUMIDITY", "UV INDEX", "LUX" ... */
  float ring;               /* 0..1 scaled by the unit's range, -1 = no ring / unscalable (unit unknown) */
  int trend;                /* dz_trend_t */
  char word[20];            /* comfort / UV level / air quality word, upper case, "" when none */
  ru_cl_line_t line[RU_CL_LINES];
  int n_lines;
  /* wind */
  bool has_dir;
  float dir_deg;
  char dir_txt[8];          /* "NW" */
  char speed[16], speed_unit[12];
  /* text page */
  char text[48];
  bool battery_low;         /* BatteryLevel known and low */
} ru_cl_view_t;
void ru_cl_build(const dz_device_t *d, const ru_cl_units_t *u, ru_cl_view_t *out);

/* WHO UV scale. v<0 or non-finite: "". */
const char *ru_cl_uv_word(float v);
bool ru_cl_battery_low(const dz_device_t *d); /* known AND <= 15 */
const char *ru_cl_trend_word(int trend);      /* "RISING" "FALLING" "STEADY" or "" */

/* One short line for list rows ("21.5°C", "3.4 m/s", "UV 5", "47%", "All good"). known=false -> "--". */
void ru_cl_summary(const dz_device_t *d, const ru_cl_units_t *u, char *out, size_t n, bool *known);

/* ---- setpoint devices ---- */
typedef struct {
  bool has_setpoint;
  float setpoint;
  bool control;        /* may be changed: controllable, a sane range, not read-only */
  float lo, hi, step;  /* valid when control */
  char unit[12];       /* "°C" / "°F" / vunit / "" unknown: shown after the number */
  bool is_temp;        /* the setpoint is a temperature (NOW makes sense) */
  bool has_now;
  float now;
  bool now_from_plan;  /* the room sensor of the same Domoticz plan, not the device itself */
  char reason[24];     /* why it is read-only: "NO RANGE", "VIEW ONLY", "PROTECTED", "" */
} ru_cl_thermo_t;
/* plan_sensor: the single temperature sensor of the same plan (dz_model_plan_temp) or NULL. allow_control = app flag. */
void ru_cl_thermo(const dz_device_t *d, const dz_units_t *u, const dz_device_t *plan_sensor, bool allow_control,
                  ru_cl_thermo_t *out);
/* "21.5°C" / "21.5" for a setpoint or NOW value with the unit of the view. */
void ru_cl_thermo_text(const ru_cl_thermo_t *t, float v, char *out, size_t n);

/* ---- mode devices ---- */
/* The n-th mode name, upper case; false when absent. The matching Mode index is d->mode_val[n]. */
bool ru_cl_mode_name(const dz_device_t *d, int n, char *out, size_t cap);
int ru_cl_mode_slot(const dz_device_t *d);   /* position of the current Mode in mode_val, -1 unknown */
