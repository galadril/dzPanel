/* View-model helpers: turn a dz_device_t into the text and state a screen shows. Pure C, no LVGL, so every rule
 * (Scene vs Group, W vs kW, "--" for unknown) is unit tested on the host. Never guesses a unit. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dz_cmd.h"
#include "dz_parse.h"

/* What a tile for a switch / scene / group shows.
 *   RU_TILE_PUSH   a Scene: push only, no on/off state at all
 *   RU_TILE_ON/OFF/MIXED  devices and Groups (MIXED only for a Group)
 *   RU_TILE_UNKNOWN  no state known: shown as "--" */
typedef enum { RU_TILE_UNKNOWN, RU_TILE_OFF, RU_TILE_ON, RU_TILE_MIXED, RU_TILE_PUSH } ru_tile_state_t;

ru_tile_state_t ru_dzv_tile_state(const dz_device_t *d);
const char *ru_dzv_tile_label(ru_tile_state_t s); /* "ON" "OFF" "MIXED" "SCENE" "--" */

/* The command a tap on a scene or group tile sends, and its verb ("ACTIVATE", "SWITCH ON", "SWITCH OFF"). A Scene
 * only ever yields DZ_CMD_SCENE_ON. False when d is neither. */
bool ru_dzv_scene_tap(const dz_device_t *d, dz_cmd_kind_t *kind, const char **verb);

/* One-line value of a device for lists. known=false means "--" was written. Units come from getconfig (dz_units_t):
 * the plain variant knows none, so temperatures and wind speeds are bare numbers. */
void ru_dzv_value_text(const dz_device_t *d, char *out, size_t n, bool *known);
void ru_dzv_value_text_u(const dz_device_t *d, const dz_units_t *units, char *out, size_t n, bool *known);

/* "15 W", "2.10 kW": W below 1000, kW from 1000, sign kept. */
void ru_dzv_power_text(float watts, char *out, size_t n);

typedef enum { RU_FLOW_NONE = 0, RU_FLOW_IMPORT, RU_FLOW_EXPORT } ru_flow_t;
typedef struct {
  bool known;          /* has a power reading at all */
  char num[12];        /* big number: "15" or "2.10", "--" when unknown */
  char unit[4];        /* "W" or "kW", "" when unknown */
  ru_flow_t flow;      /* direction when usage_deliv_w is known and non-zero */
  char flow_text[24];  /* "EXPORT 1.20 kW" / "IMPORT 15 W" / "" */
  bool has_today;
  char today[28];      /* "TODAY 7.4 kWh"; "--" when absent. Unit is what Domoticz reports, never assumed */
  float power_w;       /* signed net: positive = import. 0 when unknown */
  float hero_w;        /* the magnitude shown as the big number, in W (0 when unknown) */
} ru_energy_view_t;
void ru_dzv_energy(const dz_device_t *d, ru_energy_view_t *out);

/* Legacy field-based strings, kept for callers without readings. No unit is ever invented: a bare number. */
void ru_dzv_temp_text(const dz_device_t *d, char *out, size_t n);  /* "21.5", "--" when absent */
void ru_dzv_hum_text(const dz_device_t *d, char *out, size_t n);   /* "HUMIDITY 71%" or "" */
void ru_dzv_baro_text(const dz_device_t *d, char *out, size_t n);  /* "1014" (+ " hPa" only when the device said so) or "" */

/* helpers shared by screens */
void ru_dzv_upper(char *dst, size_t n, const char *src);
void ru_dzv_clamp(char *dst, size_t n, const char *src, size_t max); /* UTF-8 safe, ".." marks a cut */
/* n-th selector name (upper case), false if absent. */
bool ru_dzv_selector_name(const dz_device_t *d, int index, char *out, size_t n);

/* ---- clock ---- */
/* epoch_s is a server wall-clock time (dz_model_server_now), counted as if the zone were UTC: no zone or DST rules are
 * applied, the digits are the ones Domoticz showed. Own day arithmetic, no libc time. */
void ru_dzv_clock_text(int64_t epoch_s, char out[6]);  /* "13:07" (24 h) */
void ru_dzv_date_text(int64_t epoch_s, char out[12]);  /* "SAT 31 OCT" */

/* ---- gauges (the clock page's round gauges) ---- */
typedef enum {
  RU_GAUGE_TEXT,     /* value shown as text only, ring unlit (frac < 0) */
  RU_GAUGE_TEMP,     /* 0..40 C or 32..104 F, needs the unit to scale */
  RU_GAUGE_POWER,    /* 0..nice max of the reading, W or kW */
  RU_GAUGE_PERCENT   /* humidity and other percent readings, 0..100 */
} ru_gauge_kind_t;
typedef struct {
  bool known;              /* a value is present; false: text is "--" and frac -1 */
  char text[24];           /* "18.5°C", "78%", "2.1kW" */
  float frac;              /* 0..1 ring fill, -1 = unlit ring */
  ru_gauge_kind_t kind;
  char caption[9];         /* the device name, upper case, cut to 8 characters */
} ru_gauge_view_t;
/* Can this device be a gauge (a number worth a ring: temperature, humidity, power and other numeric readings)? false for
 * switches, scenes, text and mode devices; out is then "--" unknown. A gauge candidate with no value yet returns true with known=false. */
bool ru_dzv_gauge(const dz_device_t *d, const dz_units_t *units, ru_gauge_view_t *out);
/* 1-2-5 step at or above v (v <= 0: 1). The range of a power ring. */
float ru_dzv_nice_max(float v);
