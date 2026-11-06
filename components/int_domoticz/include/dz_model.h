/* Device model: a fixed-capacity store of dz_device_t keyed by (idx, namespace) with honest,
 * freshness-tagged values. Network code stages a response and commits it only when the whole
 * document parsed and "status" was OK, so a half-parsed response never replaces good data.
 * Thread-safe: writers (the network task) and readers (UI) are serialised by an internal mutex;
 * readers get copies. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dz_parse.h"
#include "ru_data.h"
#include "ru_osal.h"

typedef struct {
  dz_device_t dev;
  ru_val_t val;       /* primary numeric reading if the capability has one; valid=false otherwise */
  uint32_t ts_ms;     /* monotonic time the server last confirmed this entry */
  uint32_t changed_gen;
  bool is_scene;      /* scene/group namespace (switchscene idx), separate from device idx */
  bool from_scenes;   /* last written by a getscenes commit */
  uint16_t order;     /* position in the last complete full response of its kind (devices / scenes); see dz_model_order_of */
} dz_entry_t;

typedef struct {
  uint32_t ttl_ms;         /* entry is STALE when not confirmed by the server for this long (0 = 300000) */
  uint32_t old_update_s;   /* sensor LastUpdate older than this is STALE (0 = 21600 = 6 h; ~0u = off) */
} dz_model_cfg_t;

typedef enum {
  DZ_COMMIT_FULL_DEVICES, /* getdevices without cursor: upsert, remove device entries not present */
  DZ_COMMIT_FULL_SCENES,  /* getscenes: upsert, remove scene entries not present */
  DZ_COMMIT_DELTA,        /* getdevices with cursor: upsert; the server confirmed all others unchanged */
  DZ_COMMIT_PARTIAL       /* single-device re-read: upsert only, touches nothing else */
} dz_commit_t;

/* Display units, published once by getconfig (TempSign, TempScale, WindSign, WindScale, CurrencySign). Nothing else
 * from getconfig is ever read, kept or logged: it also holds the user's coordinates. A field is only used when its
 * have_ flag is set; until then the UI shows a bare number. */
typedef struct {
  bool have_temp;  char temp[4];      /* "C" or "F" */
  bool have_wind;  char wind[8];      /* "m/s", "km/h", "mph", "kn", "bft" */
  bool have_temp_scale, have_wind_scale;
  float temp_scale, wind_scale;
  bool have_currency; char currency[8];
} dz_units_t;

typedef struct {
  dz_entry_t *e; size_t cap, n;
  dz_device_t *stage; size_t stage_cap, stage_n;
  size_t stage_dropped;       /* stage overflow in the current response */
  dz_model_cfg_t cfg;
  ru_mutex_t *mu;
  uint32_t overflow;          /* cumulative devices dropped because the store was full */
  uint32_t changes;           /* cumulative added/changed/removed entries */
  uint32_t generation;        /* bumps once per commit that changed anything */
  uint32_t commits, aborts;
  uint32_t copies;            /* dz_device_t copies handed out by dz_model_at/get (test and diagnostics counter) */
  uint16_t tail_n;            /* entries added by delta/partial since the last complete full commit */
  dz_units_t units;           /* DZ_UNITS: all have_ flags false until getconfig was read */
  int64_t server_epoch_s;     /* server clock at last commit (from ServerTime), -1 unknown */
  uint32_t server_sync_ms;
} dz_model_t;

/* entries/stage are caller-provided (static, or PSRAM on the device). Returns false on bad args. */
bool dz_model_init(dz_model_t *m, dz_entry_t *entries, size_t cap, dz_device_t *stage, size_t stage_cap,
                   const dz_model_cfg_t *cfg);
void dz_model_deinit(dz_model_t *m);

/* Staging. dz_model_stage_cb is a dz_device_cb: hand it straight to dz_parse_response with the model as user. */
void dz_model_begin(dz_model_t *m);
bool dz_model_stage_cb(const dz_device_t *d, void *user);
/* Commit staged devices. server_time is the response's "YYYY-MM-DD HH:MM:SS" (may be NULL). Returns the
 * number of entries added/changed/removed. */
int dz_model_commit(dz_model_t *m, dz_commit_t mode, const char *server_time, uint32_t now_ms);
void dz_model_abort(dz_model_t *m);

/* begin + dz_parse_response + commit on DZ_OK, abort otherwise. */
dz_err_t dz_model_apply(dz_model_t *m, const char *json, size_t len, dz_commit_t mode, uint32_t now_ms, dz_result_t *res,
                        int *changed);

/* Readers (copy out under the lock). */
bool dz_model_get(dz_model_t *m, const char *idx, bool scene, dz_device_t *dev, ru_val_t *val, ru_val_state_t *st,
                  uint32_t now_ms);
bool dz_model_at(dz_model_t *m, size_t i, dz_device_t *dev, ru_val_t *val, ru_val_state_t *st, bool *scene,
                 uint32_t now_ms);
size_t dz_model_count(dz_model_t *m);
/* Visit every entry under ONE lock, with a const pointer and NO copy. cb returns false to stop. The pointer is valid
 * only inside cb, which must be quick and must not call back into the model. */
typedef bool (*dz_model_visit_fn)(size_t i, const dz_device_t *d, bool scene, ru_val_state_t st, uint16_t order, void *ud);
void dz_model_foreach(dz_model_t *m, uint32_t now_ms, dz_model_visit_fn cb, void *ud);
/* number of dz_device_t copies made by dz_model_at/dz_model_get so far */
uint32_t dz_model_copies(dz_model_t *m);
/* The server's own order (the user's Domoticz order with order=Name): position in the last COMPLETE full response
 * (FULL_DEVICES for devices, FULL_SCENES for scenes), counted separately per kind. An entry that arrived through a
 * delta or partial read sorts after all of them (0xFE00 + arrival number, capped at 0xFFFE) until the next complete
 * full refresh. A full response that overflowed the stage or the store never reorders. 0xFFFF = no such entry. */
uint16_t dz_model_order_of(dz_model_t *m, const char *idx, bool scene);
uint16_t dz_model_order_at(dz_model_t *m, size_t i);
uint32_t dz_model_overflow(dz_model_t *m);
uint32_t dz_model_changes(dz_model_t *m);
uint32_t dz_model_generation(dz_model_t *m);

/* Units (see dz_units_t). get copies under the lock and returns true when at least one unit is known. */
bool dz_model_units(dz_model_t *m, dz_units_t *out);
void dz_model_set_units(dz_model_t *m, const dz_units_t *u);
/* "°C" / "°F" from the temperature sign, or "" when unknown. The only place a degree symbol is built. */
void dz_units_temp_text(const dz_units_t *u, char *out, size_t n);

/* The single temperature sensor sharing a Domoticz plan (room) with `d`, for a setpoint device that has no
 * temperature of its own. Excludes d itself, setpoint devices, wind/UV auxiliary temperatures and scenes. Returns
 * true only when there is EXACTLY ONE such sensor in the plans of d (several plans are merged by device). */
bool dz_model_plan_temp(dz_model_t *m, const dz_device_t *d, dz_device_t *sensor, ru_val_t *val, ru_val_state_t *st,
                        uint32_t now_ms);

/* Helpers */
bool dz_status_is_on(const dz_device_t *d);  /* "On", "Set Level: n %", "Group On"... */
/* "YYYY-MM-DD HH:MM:SS" -> seconds since 1970 treating it as UTC (only differences are meaningful). */
bool dz_time_parse(const char *s, int64_t *out);
