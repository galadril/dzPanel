/* Domoticz JSON parser: bounded, allocation-free, tolerant of unknown fields.
 * See docs/research/domoticz-api.md. Field names come from reading the Domoticz
 * source and the hand-written mock, not from a live capture yet. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dz_tabs.h"

/* What a screen can do with a device. Chosen from capability, never from a raw type (DESIGN.md). */
typedef enum {
  DZ_CAP_READONLY, /* unknown type: show Name and Data only */
  DZ_CAP_ONOFF,
  DZ_CAP_DIMMER,
  DZ_CAP_BLINDS,
  DZ_CAP_SELECTOR,
  DZ_CAP_PUSH,
  DZ_CAP_LOCK,
  DZ_CAP_STATUS, /* contact, motion, smoke, dusk: read-only state */
  DZ_CAP_THERMOSTAT,
  DZ_CAP_TEMP,
  DZ_CAP_WEATHER,
  DZ_CAP_ENERGY,
  DZ_CAP_SCENE, /* Domoticz "Scene": PUSH ONLY. It has an On command and no Off, and no state worth toggling */
  DZ_CAP_GROUP, /* Domoticz "Group": toggles On and Off; its state is On, Off or Mixed */
  DZ_CAP_SENSOR, /* any other device with a readable value and no control: shown generically from its readings */
  DZ_CAP_MODE    /* thermostat / fan mode (Mode + Modes); set with thermostatstate (UNVERIFIED on a live server) */
} dz_cap_t;

/* Scenes and groups live in their own idx namespace (getscenes), apart from devices. */
static inline bool dz_cap_is_scene_ns(dz_cap_t c) { return c == DZ_CAP_SCENE || c == DZ_CAP_GROUP; }

typedef enum { DZ_STATE_UNKNOWN, DZ_STATE_OFF, DZ_STATE_ON, DZ_STATE_MIXED } dz_state_t;


/* What a reading measures. Screens render readings, not Domoticz types, so a type nobody has seen still shows. */
typedef enum {
  DZ_RK_NONE = 0,
  DZ_RK_TEMP,       /* the room / weather temperature (unit: TempSign from getconfig, not in the reading) */
  DZ_RK_AUXTEMP,    /* a temperature that belongs to another sensor (a Wind or UV sensor's own thermometer) */
  DZ_RK_HUM,        /* relative humidity, unit "%" */
  DZ_RK_BARO,       /* barometer / pressure, unit as Domoticz gives it ("hPa", "Bar") or empty */
  DZ_RK_DEW,        /* dew point (TempSign) */
  DZ_RK_WIND,       /* wind speed (WindSign from getconfig) */
  DZ_RK_GUST,
  DZ_RK_DIR,        /* degrees */
  DZ_RK_CHILL,      /* wind chill (TempSign) */
  DZ_RK_RAIN,       /* rain total; Domoticz gives no unit in the field, so none is shown */
  DZ_RK_RAINRATE,
  DZ_RK_UVI,
  DZ_RK_LUX,
  DZ_RK_RADIATION,
  DZ_RK_VISIBILITY,
  DZ_RK_AIRQ,
  DZ_RK_MOISTURE,   /* soil moisture, leaf wetness */
  DZ_RK_DISTANCE,
  DZ_RK_PERCENT,
  DZ_RK_POWER,
  DZ_RK_ENERGY,
  DZ_RK_GAS,
  DZ_RK_WATER,
  DZ_RK_VOLTAGE,
  DZ_RK_CURRENT,
  DZ_RK_WEIGHT,
  DZ_RK_NUMBER,     /* any other number, with the unit text Domoticz gave */
  DZ_RK_TEXT,       /* no number: the text is dz_device_t.data */
  DZ_RK_COUNT
} dz_rk_t;
const char *dz_rk_name(dz_rk_t k);

/* has=false never happens inside rd[0..n_rd); the flag exists so a zeroed reading is never mistaken for 0. */
typedef struct {
  dz_rk_t kind;
  bool has;
  float v;
  char unit[8]; /* exactly as Domoticz gave it ("Lux", "ppm", "Watt/m2"), or empty. Never guessed. */
} dz_reading_t;

#define DZ_MAX_READINGS 8
#define DZ_MAX_DEV_PLANS 4
#define DZ_MAX_MODES 8
typedef enum { DZ_TREND_UNKNOWN = 0, DZ_TREND_STABLE = 1, DZ_TREND_UP = 2, DZ_TREND_DOWN = 3 } dz_trend_t;

typedef struct {
  char idx[12];
  char name[32]; /* clamped, valid UTF-8 */
  char type[24];
  char subtype[24];
  char data[48];
  char status[32];
  char last_update[20]; /* "YYYY-MM-DD HH:MM:SS", server local time; empty if absent */
  char level_names[64]; /* selector names, '|' separated, decoded from base64 */
  int switch_type;      /* SwitchTypeVal, -1 if absent */
  dz_cap_t cap;
  uint8_t tabs;  /* DZ_TAB_* bits, from the FULL Type/SubType strings (Domoticz web UI tabs); 0 = none */
  bool favorite;
  bool is_protected;  /* needs a passcode: the device never controls these */
  bool timeout;       /* HaveTimeout: the device has stopped reporting */
  bool controllable;  /* capability allows control AND not protected */
  bool has_temp, has_hum, has_baro, has_setpoint, has_level;
  bool has_setpoint_range; /* min, max and step all present and sane (min < max, step > 0) */
  bool has_usage, has_usage_deliv, has_today;
  float temp, hum, baro, setpoint;
  float setpoint_min, setpoint_max, setpoint_step;
  float usage_w;        /* "Usage": "15 Watt" -> 15; also the "Data" of a plain Usage device */
  float usage_deliv_w;  /* "UsageDeliv": power delivered back to the grid */
  float today;          /* "CounterToday": 7.397 */
  char today_unit[8];   /* "kWh", "m3", "Liter": whatever Domoticz reports, never assumed */
  int level;      /* Level: 0..max_level */
  int level_int;  /* LevelInt: selector index value */
  int max_level;  /* MaxDimLevel, default 100 */
  unsigned range_bits; /* internal: which of min/max/step were seen */

  /* ---- readings (added; everything above is unchanged) ---- */
  dz_reading_t rd[DZ_MAX_READINGS];
  int n_rd;
  int trend;             /* dz_trend_t; UNKNOWN when absent */
  int battery;           /* BatteryLevel 0..100 */
  bool has_battery;      /* false for 255 ("no battery") or absent */
  char hum_status[16];   /* HumidityStatus: "Comfortable", "Dry", "Wet", "Normal" */
  char forecast[24];     /* ForecastStr */
  char dir_str[8];       /* DirectionStr: "NNW" */
  char quality[16];      /* Quality (air quality word) */
  char vunit[8];         /* setpoint unit as Domoticz gives it ("°C", "°F", "%"...), or empty */
  char sensor_unit[12];  /* SensorUnit (custom sensor) */
  char value_units[12];  /* ValueUnits (counters) */
  int plan_ids[DZ_MAX_DEV_PLANS]; /* PlanIDs, 0 (= no plan) dropped */
  int n_plan_ids;
  bool can_set;          /* setpoint device whose range is sane for a dial: otherwise it is shown read-only */
  bool has_mode;         /* Mode + Modes present */
  int mode;              /* current Mode index (the value thermostatstate takes) */
  int n_modes;
  short mode_val[DZ_MAX_MODES]; /* the index each name sets */
  char mode_names[96];   /* names, '|' separated, in mode_val order */
} dz_device_t;

/* first reading of a kind, or NULL */
const dz_reading_t *dz_dev_reading(const dz_device_t *d, dz_rk_t kind);
/* the temperature a person means by "the temperature here": a DZ_RK_TEMP reading, never an AUXTEMP */
const dz_reading_t *dz_dev_room_temp(const dz_device_t *d);
bool dz_dev_in_plan(const dz_device_t *d, int plan);
/* On/Off/Mixed from Status. MIXED is only ever returned for a Group or Scene (some members on, some off).
 * A device whose Status is not a plain on/off word (dimmer "Set Level: 40 %") counts as ON; empty = UNKNOWN. */
dz_state_t dz_device_state(const dz_device_t *d);

typedef enum {
  DZ_OK = 0,
  DZ_ERR_SYNTAX,  /* malformed, truncated, or nested too deeply */
  DZ_ERR_STATUS   /* well-formed, but "status" is missing or not "OK" */
} dz_err_t;

typedef struct {
  int devices;   /* devices delivered to the callback */
  int rejected;  /* entries dropped (bad or missing idx) */
  char status[16];
  char server_time[20];
  char version[48];  /* "2026.4 (build 18568)" from "version" (getversion) or "app_version" (getdevices) */
  bool has_act_time;
  long long act_time; /* ActTime: server epoch seconds, the cursor for delta polling */
  char sunrise[8], sunset[8]; /* "07:35", empty if absent */
} dz_result_t;

/* Return false to stop early (parse then ends with DZ_OK and fewer devices). */
typedef bool (*dz_device_cb)(const dz_device_t *dev, void *user);

/* Parse a getdevices, getscenes or getversion response. "status" usually comes LAST in the
 * document, so the callback fires before success is known: stage the devices and commit them only
 * when this returns DZ_OK. cb may be NULL. out may be NULL. */
dz_err_t dz_parse_response(const char *json, size_t len, dz_device_cb cb, void *user, dz_result_t *out);

/* One JSON-RPC 2.0 reply from the Domoticz MCP endpoint (the text-only result of tools/call). Strings are truncated.
 * DZ_OK: a well-formed object with "result" or "error"; DZ_ERR_STATUS: well-formed with neither; DZ_ERR_SYNTAX otherwise. */
typedef struct {
  bool has_id;
  long long id;
  bool has_error;
  int code;             /* error.code */
  char message[48];     /* error.message */
  bool has_result;
  bool is_error;        /* result.isError */
  bool has_text;
  char text[96];        /* result.content[0].text */
} dz_mcp_reply_t;
dz_err_t dz_mcp_parse_reply(const char *json, size_t len, dz_mcp_reply_t *out);

const char *dz_cap_name(dz_cap_t c);
