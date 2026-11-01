/* View-model helpers: turn a dz_device_t into the text and state a screen shows. Pure C, no LVGL, so every rule
 * (Scene vs Group, W vs kW, "--" for unknown) is unit tested on the host. Never guesses a unit. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

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
