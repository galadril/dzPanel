/* Simulator driver for the Domoticz apps: a fixture-backed fake model (no network, no mock server) so every app
 * renders with real-shaped data. Used by dzpanel_sim --apps and by the app tests. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "dz_client.h"
#include "dz_model.h"
#include "ru_dzapp.h"
#include "sim_core.h"

#define APPFX_CAP 160

typedef struct {
  dz_model_t model;
  dz_client_t client;   /* never stepped: only present because the app wants one */
  dz_transport_t *tr;
  dz_entry_t *ent;
  dz_device_t *stage;
  char *rbuf;
  uint32_t T;           /* virtual clock: model commits and UI ticks use it */
} appfx_t;

typedef enum {
  APPFX_REAL,    /* tests/fixtures/real_getdevices.json + real_getscenes.json (synthetic: renumbered idx, generic names, fixed dates) */
  APPFX_MOCK,    /* mock_getdevices + mock_getscenes: has lock, blinds, thermostat, selector, push, Scene and Group */
  APPFX_MOCK_MIXED, /* as MOCK, with one Group On and one Group Mixed */
  APPFX_EMPTY,   /* nothing at all */
  APPFX_CLIMATE_METRIC,   /* HAND-WRITTEN climate set (tests/fixtures/README.md), TempSign C and WindSign m/s */
  APPFX_CLIMATE_IMPERIAL, /* the same set as a Fahrenheit / mph install */
  APPFX_CLIMATE_NOUNITS,  /* the metric devices but getconfig never read: units unknown, bare numbers */
  APPFX_MOCK_NOFAV,       /* as MOCK, no favourites at all */
  APPFX_SWITCH_TYPES,     /* generated: six Lights-tab tiles of other classes (blinds, lock, contact, smoke, push) */
  APPFX_LIGHTS,           /* tests/fixtures/mock_lights.json: colour and white lamps (idx 1 RGBWW orange, 2 RGB, 3 RGBW, 4 WW, 5 WW off, 6 RGBWW off, 7 RGBWW white, 8 protected, 9 plain) */
  APPFX_REAL_NOSYNC,      /* as REAL, but without a ServerTime anywhere: the clock has nothing to show */
  APPFX_CLOCK_TWO,        /* generated: two numeric favourites (a temperature and a power meter) and a ServerTime */
  APPFX_LONGNAMES,        /* generated: real-world long names and long values, to check that text stays inside rows and tiles */
  APPFX_MANY              /* generated: 100 lights (the first 8 favourites), 30 temperature, 20 utility; idx 1..150 in that order */
} appfx_set_t;

/* ttl_ms: how long an entry stays fresh. Returns false on any failure. */
bool appfx_load(appfx_t *f, const char *fixture_dir, appfx_set_t set, uint32_t ttl_ms);
void appfx_unload(appfx_t *f);
/* First entry with this capability and (if state >= 0) this dz_device_state; copies its idx. */
bool appfx_find(appfx_t *f, dz_cap_t cap, int state, char *idx, bool *scene);
/* Replace the scenes with a literal getscenes document (tests). */
bool appfx_apply_scenes(appfx_t *f, const char *json);

/* Feed the simulator's pointer to a ru_gesture recogniser exactly like the device shell does: rim edge swipes go BACK /
 * HOME, content swipes go to ru_dzapp_swipe. Call detach before destroying the app. */
void appfx_gestures_attach(ru_dzapp_t *a, int diameter, bool square);
void appfx_gestures_detach(void);
/* Preferences like the device's glue: START PAGE and ROTATION go through a ru_kv_mem store ("ui" namespace, ru_prefs); a rotation is
 * applied live with sim_set_rotation (the window, the mouse and the PNGs turn, LVGL stays upright). The store lives for the process:
 * a second app instance starts with what the first one chose. Attached to every app apps_demo_show builds. */
void appfx_prefs_attach(ru_dzapp_t *a);
void appfx_prefs_reset(void); /* forget everything stored and turn the screen back to 0 (tests) */
/* the same drag as appfx_drag, but the points are PANEL pixels (a finger on a turned screen) */
void appfx_drag_panel(int x0, int y0, int x1, int y1, int steps);
/* a drag of `n` 16 ms steps from (x0,y0) to (x1,y1), through the pointer (and so through the gesture hook) */
void appfx_drag(int x0, int y0, int x1, int y1, int steps);

extern const char *const apps_demo_names[];
extern const int apps_demo_count;

/* Build the named app screen from fixtures and show it. Returns the app (caller destroys it with
 * ru_dzapp_destroy and appfx_unload) or NULL for an unknown name. */
ru_dzapp_t *apps_demo_show(appfx_t *f, const char *fixture_dir, const char *name);
/* load + show + render + save PNG + cleanup */
int apps_demo_shot(sim_t *s, const char *fixture_dir, const char *name, const char *path);
