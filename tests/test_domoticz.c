#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dz_parse.h"
#include "mini_test.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "tests/fixtures"
#endif

typedef struct {
  dz_device_t d[32];
  int n;
} bag_t;

static bool collect(const dz_device_t *d, void *u) {
  bag_t *b = u;
  if (b->n < 32) b->d[b->n++] = *d;
  return true;
}

static bool stop_after_two(const dz_device_t *d, void *u) {
  (void)d;
  return ++*(int *)u < 2;
}

static const dz_device_t *find(const bag_t *b, const char *idx) {
  for (int i = 0; i < b->n; i++) if (!strcmp(b->d[i].idx, idx)) return &b->d[i];
  return NULL;
}

static char *slurp(const char *name, size_t *len) {
  char path[256];
  snprintf(path, sizeof path, "%s/%s", FIXTURE_DIR, name);
  FILE *f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); exit(2); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)n + 1);
  if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
  b[n] = 0;
  fclose(f);
  *len = (size_t)n;
  return b;
}

static dz_err_t parse_str(const char *s, bag_t *b, dz_result_t *r) {
  b->n = 0;
  return dz_parse_response(s, strlen(s), collect, b, r);
}

static void test_fixture(void) {
  size_t len;
  char *js = slurp("mock_getdevices.json", &len);
  static bag_t b;
  dz_result_t r;
  b.n = 0;
  CHECK(dz_parse_response(js, len, collect, &b, &r) == DZ_OK);
  CHECK(b.n == 14 && r.devices == 14 && r.rejected == 0);
  CHECK(strlen(r.server_time) == 19);

  const dz_device_t *d = find(&b, "1");
  CHECK(d && d->cap == DZ_CAP_ONOFF && d->controllable && !strcmp(d->name, "Hall Lamp"));
  d = find(&b, "2");
  CHECK(d && d->cap == DZ_CAP_DIMMER && d->max_level == 100 && d->has_level);
  d = find(&b, "3");
  CHECK(d && d->cap == DZ_CAP_BLINDS);
  d = find(&b, "4");
  CHECK(d && d->cap == DZ_CAP_SELECTOR && d->level_int == 20 && !strcmp(d->level_names, "Off|Low|Med|High"));
  d = find(&b, "5");
  CHECK(d && d->cap == DZ_CAP_PUSH);
  d = find(&b, "6");
  CHECK(d && d->cap == DZ_CAP_LOCK && d->controllable);
  d = find(&b, "7");
  CHECK(d && d->cap == DZ_CAP_STATUS && d->is_protected && !d->controllable);
  d = find(&b, "10");
  CHECK(d && d->cap == DZ_CAP_THERMOSTAT && d->has_setpoint);
  CHECK_NEAR(d->setpoint, 21.5, 1e-4); /* arrives as a string */
  d = find(&b, "11");
  CHECK(d && d->cap == DZ_CAP_TEMP && d->has_temp && d->has_hum && !d->has_baro);
  CHECK_NEAR(d->temp, 12.4, 1e-4);
  d = find(&b, "12");
  CHECK(d && d->cap == DZ_CAP_WEATHER && d->has_baro);
  d = find(&b, "13");
  CHECK(d && d->cap == DZ_CAP_WEATHER && !d->has_temp);
  d = find(&b, "14");
  CHECK(d && d->cap == DZ_CAP_WEATHER);
  d = find(&b, "20");
  CHECK(d && d->cap == DZ_CAP_ENERGY && !d->controllable && !strcmp(d->data, "2100 Watt"));
  d = find(&b, "30");
  CHECK(d && d->cap == DZ_CAP_TEMP && d->timeout && !strcmp(d->last_update, "2026-10-30 07:12:00"));
  free(js);

  js = slurp("mock_getscenes.json", &len);
  b.n = 0;
  CHECK(dz_parse_response(js, len, collect, &b, &r) == DZ_OK && b.n == 4);
  CHECK(b.d[0].cap == DZ_CAP_SCENE && b.d[1].cap == DZ_CAP_SCENE && b.d[2].cap == DZ_CAP_GROUP && b.d[3].cap == DZ_CAP_GROUP);
  CHECK(b.d[0].controllable && b.d[2].controllable);
  free(js);

  js = slurp("mock_getversion.json", &len);
  CHECK(dz_parse_response(js, len, NULL, NULL, &r) == DZ_OK && !strcmp(r.version, "2026.1") && r.devices == 0);
  free(js);
}

static void test_errors(void) {
  static bag_t b;
  dz_result_t r;
  /* Well-formed but not OK */
  CHECK(parse_str("{\"status\":\"ERR\"}", &b, &r) == DZ_ERR_STATUS);
  CHECK(parse_str("{\"title\":\"x\"}", &b, &r) == DZ_ERR_STATUS); /* no status */
  CHECK(parse_str("{\"status\":\"OK\"}", &b, &r) == DZ_OK && b.n == 0); /* no result key = no devices */
  CHECK(parse_str("{\"result\":[],\"status\":\"OK\"}", &b, &r) == DZ_OK && b.n == 0);
  /* Malformed */
  CHECK(parse_str("", &b, &r) == DZ_ERR_SYNTAX);
  CHECK(parse_str("[]", &b, &r) == DZ_ERR_SYNTAX);
  CHECK(parse_str("<html>Access Denied</html>", &b, &r) == DZ_ERR_SYNTAX); /* the proxy page we saw */
  CHECK(parse_str("{\"status\":\"OK\"} junk", &b, &r) == DZ_ERR_SYNTAX);
  CHECK(parse_str("{\"status\":\"OK\",}", &b, &r) == DZ_ERR_SYNTAX);
  CHECK(parse_str("{\"status\":\"OK\",\"x\":\"unterminated}", &b, &r) == DZ_ERR_SYNTAX);
  CHECK(dz_parse_response(NULL, 0, NULL, NULL, NULL) == DZ_ERR_SYNTAX);

  /* Deep nesting must fail cleanly, not blow the stack */
  char deep[4096];
  size_t n = 0;
  n += (size_t)snprintf(deep + n, sizeof deep - n, "{\"x\":");
  for (int i = 0; i < 1000; i++) deep[n++] = '[';
  for (int i = 0; i < 1000; i++) deep[n++] = ']';
  n += (size_t)snprintf(deep + n, sizeof deep - n, ",\"status\":\"OK\"}");
  CHECK(dz_parse_response(deep, n, NULL, NULL, NULL) == DZ_ERR_SYNTAX);
}

static void test_robustness(void) {
  static bag_t b;
  dz_result_t r;

  /* Bad idx entries are rejected, not delivered */
  CHECK(parse_str("{\"result\":[{\"Name\":\"a\"},{\"idx\":\"x1\"},{\"idx\":\"\"},{\"idx\":\"1234567890\"},{\"idx\":7,\"Name\":\"ok\"}],"
                  "\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(r.rejected == 4 && b.n == 1 && !strcmp(b.d[0].idx, "7"));

  /* Unknown fields, nested values and wrong types are ignored; unknown type is read-only */
  CHECK(parse_str("{\"result\":[{\"idx\":\"3\",\"Name\":\"N\",\"Type\":\"Future\",\"Extra\":{\"a\":[1,2,{}]},"
                  "\"Temp\":\"abc\",\"Humidity\":null,\"SwitchTypeVal\":\"zz\"}],\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(b.n == 1 && b.d[0].cap == DZ_CAP_READONLY && !b.d[0].has_temp && !b.d[0].has_hum && !b.d[0].controllable);

  /* NaN / Inf never become a value: an unknown stays unknown */
  CHECK(parse_str("{\"result\":[{\"idx\":\"3\",\"Temp\":\"nan\",\"SetPoint\":\"inf\"}],\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(!b.d[0].has_temp && !b.d[0].has_setpoint);

  /* Unknown switch type never becomes controllable */
  CHECK(parse_str("{\"result\":[{\"idx\":\"3\",\"SwitchTypeVal\":17}],\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(b.d[0].cap == DZ_CAP_READONLY && !b.d[0].controllable);

  /* Protected overrides capability */
  CHECK(parse_str("{\"result\":[{\"idx\":\"3\",\"Type\":\"Light/Switch\",\"SwitchTypeVal\":0,\"Protected\":true}],\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(b.d[0].cap == DZ_CAP_ONOFF && !b.d[0].controllable);

  /* Long names are clamped on a UTF-8 boundary: 20 x 2-byte chars = 40 bytes into 32 */
  char js[512];
  strcpy(js, "{\"result\":[{\"idx\":\"3\",\"Name\":\"");
  for (int i = 0; i < 20; i++) strcat(js, "\xC3\xA9");
  strcat(js, "\"}],\"status\":\"OK\"}");
  CHECK(parse_str(js, &b, &r) == DZ_OK);
  size_t nl = strlen(b.d[0].name);
  CHECK(nl <= 31 && nl % 2 == 0);

  /* Escapes, including a surrogate pair (U+1F600 = F0 9F 98 80) and a lone surrogate */
  CHECK(parse_str("{\"result\":[{\"idx\":\"3\",\"Name\":\"a\\u00e9\\ud83d\\ude00\\n\\\"\"}],\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(!strcmp(b.d[0].name, "a\xC3\xA9\xF0\x9F\x98\x80\n\""));
  CHECK(parse_str("{\"result\":[{\"idx\":\"3\",\"Name\":\"\\ud83dX\\u0000\"}],\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(!strcmp(b.d[0].name, "\xEF\xBF\xBDX\xEF\xBF\xBD"));

  /* Callback can stop early */
  size_t len;
  char *fx = slurp("mock_getdevices.json", &len);
  int seen = 0;
  CHECK(dz_parse_response(fx, len, stop_after_two, &seen, &r) == DZ_OK);
  CHECK(seen == 2 && r.devices == 2); /* stopped early, rest skipped, status still read */
  free(fx);

  /* Whitespace and pretty printing (Domoticz pretty-prints with tabs and newlines) */
  CHECK(parse_str("{\n\t\"result\" :\n [\n {\n\t\"idx\" : \"9\"\n }\n ],\n\t\"status\" : \"OK\"\n}\n", &b, &r) == DZ_OK);
  CHECK(b.n == 1);
}


/* Real capture from a Domoticz 2026.4 instance (names scrubbed). Regression guard: sensors and meters
 * carry a SwitchTypeVal that is NOT a switch type and must never become controls. */
static void test_real_capture(void) {
  size_t len;
  char *js = slurp("real_getdevices.json", &len);
  static bag_t b;
  dz_result_t r;
  b.n = 0;
  CHECK(dz_parse_response(js, len, collect, &b, &r) == DZ_OK);
  CHECK(r.devices == 26 && r.rejected == 0 && b.n == 26);
  int sensors = 0, ctl = 0;
  for (int i = 0; i < b.n; i++) {
    const dz_device_t *d = &b.d[i];
    bool sensor = d->cap == DZ_CAP_ENERGY || d->cap == DZ_CAP_WEATHER || d->cap == DZ_CAP_TEMP || d->cap == DZ_CAP_READONLY ||
                  d->cap == DZ_CAP_STATUS;
    if (sensor) { sensors++; CHECK(!d->controllable); }
    if (d->controllable) ctl++;
    if (!strcmp(d->type, "P1 Smart Meter") || !strcmp(d->type, "RFXMeter")) CHECK(d->cap == DZ_CAP_ENERGY);
    if (!strcmp(d->type, "Setpoint")) CHECK(d->cap == DZ_CAP_THERMOSTAT && d->has_setpoint);
    if (!strcmp(d->type, "Color Switch")) CHECK(d->cap == DZ_CAP_DIMMER);
  }
  CHECK(sensors > 8 && ctl > 6);
  free(js);

  js = slurp("real_getscenes.json", &len);
  b.n = 0;
  CHECK(dz_parse_response(js, len, collect, &b, &r) == DZ_OK && b.n == 6);
  for (int i = 0; i < b.n; i++) CHECK(b.d[i].cap == DZ_CAP_GROUP && dz_device_state(&b.d[i]) == DZ_STATE_OFF); /* real server: all Groups, all Off */
  free(js);
}

static const dz_device_t *find_sub(const bag_t *b, const char *type, const char *sub) {
  for (int i = 0; i < b->n; i++) if (!strcmp(b->d[i].type, type) && !strcmp(b->d[i].subtype, sub)) return &b->d[i];
  return NULL;
}

/* Fields the Client agent asked for, checked against the real 2026.4 capture. */
static void test_energy_and_ranges(void) {
  size_t len;
  char *js = slurp("real_getdevices.json", &len);
  static bag_t b;
  dz_result_t r;
  b.n = 0;
  CHECK(dz_parse_response(js, len, collect, &b, &r) == DZ_OK);
  CHECK(r.has_act_time && r.act_time == 1793448461LL); /* exact: not squeezed through a float */
  CHECK(!strcmp(r.version, "2026.4 (build 18568)"));    /* full build number, from app_version */

  const dz_device_t *d = find_sub(&b, "P1 Smart Meter", "Energy");
  CHECK(d && d->cap == DZ_CAP_ENERGY && d->has_usage && d->has_usage_deliv);
  CHECK_NEAR(d->usage_w, 15.0, 1e-3);
  CHECK_NEAR(d->usage_deliv_w, 0.0, 1e-3);
  CHECK(d->has_today && !strcmp(d->today_unit, "kWh"));
  CHECK_NEAR(d->today, 7.397, 1e-3);
  d = find_sub(&b, "P1 Smart Meter", "Gas");
  CHECK(d && d->has_today && !strcmp(d->today_unit, "m3") && !d->has_usage); /* gas has no power: stays unknown */
  d = find_sub(&b, "RFXMeter", "RFXMeter counter");
  CHECK(d && d->has_today && !strcmp(d->today_unit, "Liter"));
  d = find_sub(&b, "Usage", "Electric");
  CHECK(d && d->cap == DZ_CAP_ENERGY && d->has_usage); /* power only in Data: "5.15 Watt" */
  CHECK_NEAR(d->usage_w, 5.15, 1e-3);
  d = find_sub(&b, "General", "kWh");
  CHECK(d && d->cap == DZ_CAP_ENERGY && d->has_usage && !d->controllable);
  d = find_sub(&b, "Setpoint", "SetPoint");
  CHECK(d && d->has_setpoint_range);
  CHECK_NEAR(d->setpoint_min, 14.0, 1e-4);
  CHECK_NEAR(d->setpoint_max, 25.0, 1e-4);
  CHECK_NEAR(d->setpoint_step, 0.5, 1e-4);
  free(js);

  /* Hostile and odd input: unknown stays unknown, nothing becomes a number by accident */
  CHECK(parse_str("{\"result\":[{\"idx\":\"1\",\"Usage\":\"1e999 Watt\",\"UsageDeliv\":\"15\","
                  "\"CounterToday\":\"nan kWh\"}],\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(!b.d[0].has_usage && !b.d[0].has_usage_deliv && !b.d[0].has_today); /* inf, bare number, nan */
  CHECK(parse_str("{\"result\":[{\"idx\":\"1\",\"Usage\":\"2 kW\",\"CounterToday\":\"3 averyveryverylongunit\"}],\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(b.d[0].has_usage && b.d[0].usage_w == 2000.0f);
  CHECK(b.d[0].has_today && b.d[0].today_unit[0] == 0); /* over-long unit: value kept, unit unknown */
  CHECK(parse_str("{\"result\":[{\"idx\":\"1\",\"SetPoint\":\"20\",\"min\":25,\"max\":14,\"step\":0.5}],\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(!b.d[0].has_setpoint_range); /* min >= max is not a range */
  CHECK(parse_str("{\"result\":[{\"idx\":\"1\",\"min\":14,\"max\":25}],\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(!b.d[0].has_setpoint_range); /* step missing */
  CHECK(parse_str("{\"result\":[{\"idx\":\"1\",\"min\":14,\"max\":25,\"step\":0}],\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(!b.d[0].has_setpoint_range); /* step must be positive */
  CHECK(parse_str("{\"ActTime\":-5,\"status\":\"OK\"}", &b, &r) == DZ_OK && !r.has_act_time);
  CHECK(parse_str("{\"ActTime\":12.5,\"status\":\"OK\"}", &b, &r) == DZ_OK && !r.has_act_time);
  CHECK(parse_str("{\"ActTime\":\"123\",\"status\":\"OK\"}", &b, &r) == DZ_OK && !r.has_act_time); /* must be a number */
  CHECK(parse_str("{\"ActTime\":99999999999999999999,\"status\":\"OK\"}", &b, &r) == DZ_OK && !r.has_act_time); /* overflow */
  CHECK(parse_str("{\"version\":\"2026.4 (build 18568)\",\"app_version\":\"x\",\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(!strcmp(r.version, "2026.4 (build 18568)")); /* getversion's own field wins */
  CHECK(parse_str("{\"Sunrise\":\"07:35\",\"Sunset\":\"17:14\",\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(!strcmp(r.sunrise, "07:35") && !strcmp(r.sunset, "17:14"));
}

/* Domoticz semantics (confirmed in WebServerCmds.cpp): SceneType 0 = "Scene" (push only), else "Group"
 * (toggles); both report Status Off / On / Mixed. Mixed = members differ. */
static void test_scene_and_group(void) {
  static bag_t b;
  dz_result_t r;
  CHECK(parse_str("{\"result\":[{\"idx\":\"1\",\"Type\":\"Scene\",\"Status\":\"Off\"},"
                  "{\"idx\":\"2\",\"Type\":\"Scene\",\"Status\":\"On\"},"
                  "{\"idx\":\"3\",\"Type\":\"Group\",\"Status\":\"Mixed\"},"
                  "{\"idx\":\"4\",\"Type\":\"Group\",\"Status\":\"On\"},"
                  "{\"idx\":\"5\",\"Type\":\"Group\",\"Status\":\"Off\",\"Protected\":true},"
                  "{\"idx\":\"6\",\"Type\":\"Group\"}],\"status\":\"OK\"}", &b, &r) == DZ_OK && b.n == 6);
  CHECK(b.d[0].cap == DZ_CAP_SCENE && b.d[1].cap == DZ_CAP_SCENE);
  CHECK(b.d[2].cap == DZ_CAP_GROUP && b.d[3].cap == DZ_CAP_GROUP);
  CHECK(dz_cap_is_scene_ns(DZ_CAP_SCENE) && dz_cap_is_scene_ns(DZ_CAP_GROUP) && !dz_cap_is_scene_ns(DZ_CAP_ONOFF));
  CHECK(dz_device_state(&b.d[0]) == DZ_STATE_OFF && dz_device_state(&b.d[1]) == DZ_STATE_ON);
  CHECK(dz_device_state(&b.d[2]) == DZ_STATE_MIXED); /* the third state */
  CHECK(dz_device_state(&b.d[3]) == DZ_STATE_ON && dz_device_state(&b.d[4]) == DZ_STATE_OFF);
  CHECK(dz_device_state(&b.d[5]) == DZ_STATE_UNKNOWN); /* no Status: unknown, never silently Off */
  CHECK(!b.d[4].controllable); /* a protected group is never controlled */
  /* "Mixed" is a group word: on an ordinary device it means nothing and must not look like a state */
  CHECK(parse_str("{\"result\":[{\"idx\":\"9\",\"Type\":\"Light/Switch\",\"SwitchTypeVal\":0,\"Status\":\"Mixed\"}],"
                  "\"status\":\"OK\"}", &b, &r) == DZ_OK);
  CHECK(dz_device_state(&b.d[0]) == DZ_STATE_UNKNOWN);
  CHECK(!strcmp(dz_cap_name(DZ_CAP_GROUP), "group") && !strcmp(dz_cap_name(DZ_CAP_SCENE), "scene"));
}


/* ---- readings: every Domoticz family through one representation (fixtures are hand-written, see tests/fixtures/README.md) ---- */
static const dz_reading_t *rd(const dz_device_t *d, dz_rk_t k) { return dz_dev_reading(d, k); }

/* invariants that must hold for ANY parsed device, however mangled the input */
static bool sane_device(const dz_device_t *d) {
  if (d->n_rd < 0 || d->n_rd > DZ_MAX_READINGS) return false;
  for (int i = 0; i < d->n_rd; i++) {
    const dz_reading_t *r = &d->rd[i];
    if (!r->has || r->kind <= DZ_RK_NONE || r->kind >= DZ_RK_COUNT) return false;
    if (!isfinite(r->v)) return false;
    if (memchr(r->unit, 0, sizeof r->unit) == NULL) return false;
  }
  if (d->n_plan_ids < 0 || d->n_plan_ids > DZ_MAX_DEV_PLANS) return false;
  if (d->n_modes < 0 || d->n_modes > DZ_MAX_MODES) return false;
  if (memchr(d->mode_names, 0, sizeof d->mode_names) == NULL) return false;
  if (memchr(d->hum_status, 0, sizeof d->hum_status) == NULL || memchr(d->forecast, 0, sizeof d->forecast) == NULL) return false;
  if (memchr(d->dir_str, 0, sizeof d->dir_str) == NULL || memchr(d->quality, 0, sizeof d->quality) == NULL) return false;
  if (memchr(d->vunit, 0, sizeof d->vunit) == NULL || memchr(d->sensor_unit, 0, sizeof d->sensor_unit) == NULL) return false;
  if (d->trend < 0 || d->trend > 3) return false;
  if (d->has_battery && (d->battery < 0 || d->battery > 100)) return false;
  return true;
}

static void test_climate_families(const char *file, bool imperial) {
  size_t len;
  char *js = slurp(file, &len);
  static bag_t b;
  dz_result_t r;
  b.n = 0;
  CHECK(dz_parse_response(js, len, collect, &b, &r) == DZ_OK && b.n == 26 && r.rejected == 0);
  for (int i = 0; i < b.n; i++) CHECK(sane_device(&b.d[i]));
  const dz_device_t *t = find(&b, "100"), *h = find(&b, "101"), *th = find(&b, "102"), *thb = find(&b, "103"),
                    *w = find(&b, "104"), *rn = find(&b, "105"), *uv = find(&b, "106"), *lux = find(&b, "107"),
                    *aq = find(&b, "108");
  CHECK(t && h && th && thb && w && rn && uv && lux && aq);
  /* temperature: value, trend, battery, plan; the unit is NOT in the reading (it comes from getconfig) */
  CHECK(t->cap == DZ_CAP_TEMP && rd(t, DZ_RK_TEMP) && fabsf(rd(t, DZ_RK_TEMP)->v - (imperial ? 70.7f : 21.5f)) < 0.01f);
  CHECK(!rd(t, DZ_RK_TEMP)->unit[0] && t->trend == DZ_TREND_UP && t->has_battery && t->battery == 90);
  CHECK(t->n_plan_ids == 1 && t->plan_ids[0] == 7 && dz_dev_in_plan(t, 7) && !dz_dev_in_plan(t, 8) && !dz_dev_in_plan(t, 0));
  /* humidity alone is a weather device with a comfort word; plan 0 means no plan */
  CHECK(h->cap == DZ_CAP_WEATHER && rd(h, DZ_RK_HUM) && rd(h, DZ_RK_HUM)->v == 71.0f && !strcmp(rd(h, DZ_RK_HUM)->unit, "%"));
  CHECK(!strcmp(h->hum_status, "Wet") && h->n_plan_ids == 0 && !rd(h, DZ_RK_TEMP) && h->trend == DZ_TREND_UNKNOWN);
  CHECK(th->cap == DZ_CAP_TEMP && rd(th, DZ_RK_TEMP) && rd(th, DZ_RK_HUM) && rd(th, DZ_RK_DEW) && th->trend == DZ_TREND_STABLE);
  CHECK(!strcmp(th->hum_status, "Comfortable"));
  /* temp + humidity + baro: pressure unit comes from Data ("hPa"), forecast text, dew point */
  CHECK(rd(thb, DZ_RK_BARO) && rd(thb, DZ_RK_BARO)->v == 1009.0f && !strcmp(rd(thb, DZ_RK_BARO)->unit, "hPa"));
  CHECK(!strcmp(thb->forecast, "Sunny") && rd(thb, DZ_RK_DEW) && thb->trend == DZ_TREND_DOWN);
  /* wind: speed and gust carry NO unit (WindSign from getconfig); its Temp is its own thermometer */
  CHECK(w->cap == DZ_CAP_WEATHER && rd(w, DZ_RK_WIND) && rd(w, DZ_RK_GUST) && rd(w, DZ_RK_DIR) && rd(w, DZ_RK_CHILL));
  CHECK(!rd(w, DZ_RK_WIND)->unit[0] && !rd(w, DZ_RK_GUST)->unit[0]);
  CHECK(fabsf(rd(w, DZ_RK_WIND)->v - (imperial ? 7.6f : 3.4f)) < 0.01f && rd(w, DZ_RK_DIR)->v == 315.0f && !strcmp(w->dir_str, "NW"));
  CHECK(rd(w, DZ_RK_AUXTEMP) && !rd(w, DZ_RK_TEMP) && !dz_dev_room_temp(w) && !w->has_temp);
  CHECK(rd(uv, DZ_RK_UVI) && rd(uv, DZ_RK_UVI)->v == 5.0f && rd(uv, DZ_RK_AUXTEMP) && !dz_dev_room_temp(uv) && !uv->has_temp);
  CHECK(rd(rn, DZ_RK_RAIN) && rd(rn, DZ_RK_RAIN)->v == 12.5f && rd(rn, DZ_RK_RAINRATE) && !rd(rn, DZ_RK_RAIN)->unit[0]);
  CHECK(rn->cap == DZ_CAP_WEATHER);
  /* Data-only families: the number and the unit exactly as written */
  CHECK(lux->cap == DZ_CAP_WEATHER && rd(lux, DZ_RK_LUX) && rd(lux, DZ_RK_LUX)->v == 123.0f && !strcmp(rd(lux, DZ_RK_LUX)->unit, "Lux"));
  CHECK(rd(aq, DZ_RK_AIRQ) && rd(aq, DZ_RK_AIRQ)->v == 850.0f && !strcmp(rd(aq, DZ_RK_AIRQ)->unit, "ppm") && !strcmp(aq->quality, "Fair"));
  /* General subtypes */
  const dz_device_t *g;
  g = find(&b, "109"); CHECK(g && g->cap == DZ_CAP_SENSOR && rd(g, DZ_RK_PERCENT) && rd(g, DZ_RK_PERCENT)->v == 47.0f && !strcmp(rd(g, DZ_RK_PERCENT)->unit, "%"));
  g = find(&b, "110"); CHECK(g && rd(g, DZ_RK_MOISTURE) && !strcmp(rd(g, DZ_RK_MOISTURE)->unit, "cb"));
  g = find(&b, "111"); CHECK(g && rd(g, DZ_RK_RADIATION) && !strcmp(rd(g, DZ_RK_RADIATION)->unit, "Watt/m2"));
  g = find(&b, "112"); CHECK(g && rd(g, DZ_RK_WEIGHT) && rd(g, DZ_RK_WEIGHT)->v == 42.5f && !strcmp(rd(g, DZ_RK_WEIGHT)->unit, "kg"));
  g = find(&b, "113"); CHECK(g && g->cap == DZ_CAP_SENSOR && rd(g, DZ_RK_TEXT) && !strcmp(g->data, "All good") && g->n_rd == 1);
  g = find(&b, "114"); CHECK(g && rd(g, DZ_RK_BARO) && !strcmp(rd(g, DZ_RK_BARO)->unit, "Bar"));
  g = find(&b, "115"); CHECK(g && rd(g, DZ_RK_TEXT) && !rd(g, DZ_RK_NUMBER));
  /* a type nobody has seen: never hidden, still shows its Data with its unit */
  g = find(&b, "116"); CHECK(g && g->cap == DZ_CAP_SENSOR && rd(g, DZ_RK_NUMBER) && rd(g, DZ_RK_NUMBER)->v == 7.5f && !strcmp(rd(g, DZ_RK_NUMBER)->unit, "zorp"));
  CHECK(!g->controllable);
  /* setpoint family */
  const dz_device_t *t6 = find(&b, "117"), *sp = find(&b, "118"), *two = find(&b, "121"), *rad = find(&b, "123"),
                    *evo = find(&b, "124"), *mode = find(&b, "125");
  CHECK(t6 && sp && two && rad && evo && mode);
  CHECK(t6->cap == DZ_CAP_THERMOSTAT && t6->can_set && t6->controllable && rd(t6, DZ_RK_TEMP) && rd(t6, DZ_RK_HUM) && t6->trend == DZ_TREND_UP);
  CHECK(sp->cap == DZ_CAP_THERMOSTAT && sp->can_set && sp->controllable && !rd(sp, DZ_RK_TEMP) && !dz_dev_room_temp(sp));
  CHECK(!strcmp(sp->vunit, imperial ? "\xC2\xB0" "F" : "\xC2\xB0" "C") && sp->setpoint_min < sp->setpoint_max && sp->setpoint_step == 0.5f);
  CHECK(rad->cap == DZ_CAP_THERMOSTAT && rad->has_setpoint && !rad->can_set && !rad->controllable); /* no range: read-only */
  CHECK(evo->cap == DZ_CAP_THERMOSTAT && !evo->can_set && !evo->controllable && dz_dev_room_temp(evo) && !strcmp(evo->status, "Auto"));
  CHECK(two->n_plan_ids == 1 && two->plan_ids[0] == 8);
  /* thermostat mode: names and the indices thermostatstate takes */
  CHECK(mode->cap == DZ_CAP_MODE && mode->has_mode && mode->mode == 1 && mode->n_modes == 3 && mode->controllable);
  CHECK(!strcmp(mode->mode_names, "Off|Heat|Cool") && mode->mode_val[0] == 0 && mode->mode_val[1] == 1 && mode->mode_val[2] == 2);
  free(js);
}

static void test_climate_edges(void) {
  static bag_t b;
  /* a missing or malformed number stays unknown, never 0 */
  CHECK(parse_str("{\"result\":[{\"idx\":\"1\",\"Type\":\"Wind\",\"Speed\":\"fast\",\"Gust\":\"\",\"Direction\":\"400\",\"Temp\":\"nan\","
                  "\"Chill\":\"1e999\",\"Data\":\"x\"}],\"status\":\"OK\"}", &b, NULL) == DZ_OK);
  CHECK(b.n == 1 && !rd(&b.d[0], DZ_RK_WIND) && !rd(&b.d[0], DZ_RK_GUST) && !rd(&b.d[0], DZ_RK_DIR) && !rd(&b.d[0], DZ_RK_CHILL));
  CHECK(!rd(&b.d[0], DZ_RK_AUXTEMP) && !rd(&b.d[0], DZ_RK_TEMP) && b.d[0].cap == DZ_CAP_WEATHER);
  /* hex and inf are not numbers; Data that is not a number is TEXT */
  CHECK(parse_str("{\"result\":[{\"idx\":\"2\",\"Type\":\"General\",\"SubType\":\"Custom Sensor\",\"Data\":\"0x1F V\"}],\"status\":\"OK\"}", &b, NULL) == DZ_OK);
  CHECK(b.n == 1 && !rd(&b.d[0], DZ_RK_NUMBER) && !rd(&b.d[0], DZ_RK_VOLTAGE) && rd(&b.d[0], DZ_RK_TEXT) && b.d[0].cap == DZ_CAP_SENSOR);
  CHECK(parse_str("{\"result\":[{\"idx\":\"3\",\"Type\":\"General\",\"SubType\":\"Custom Sensor\",\"Data\":\"inf kg\"}],\"status\":\"OK\"}", &b, NULL) == DZ_OK);
  CHECK(b.n == 1 && !rd(&b.d[0], DZ_RK_WEIGHT) && rd(&b.d[0], DZ_RK_TEXT));
  /* a unit too long for the field is dropped, not cut */
  CHECK(parse_str("{\"result\":[{\"idx\":\"4\",\"Type\":\"Quantum\",\"Data\":\"5 averyveryverylongunit\"}],\"status\":\"OK\"}", &b, NULL) == DZ_OK);
  CHECK(b.n == 1 && rd(&b.d[0], DZ_RK_NUMBER) && !rd(&b.d[0], DZ_RK_NUMBER)->unit[0] && rd(&b.d[0], DZ_RK_NUMBER)->v == 5.0f);
  /* a legitimate zero is a reading (rain 0), distinct from none */
  CHECK(parse_str("{\"result\":[{\"idx\":\"5\",\"Type\":\"Rain\",\"Rain\":\"0\",\"RainRate\":\"0\",\"Data\":\"0\"}],\"status\":\"OK\"}", &b, NULL) == DZ_OK);
  CHECK(rd(&b.d[0], DZ_RK_RAIN) && rd(&b.d[0], DZ_RK_RAIN)->v == 0.0f && rd(&b.d[0], DZ_RK_RAINRATE));
  /* readings are bounded: more fields than slots drops the extra ones, never overflows */
  CHECK(parse_str("{\"result\":[{\"idx\":\"6\",\"Type\":\"Wind\",\"Temp\":1,\"Humidity\":2,\"Barometer\":3,\"DewPoint\":4,\"Speed\":5,"
                  "\"Gust\":6,\"Direction\":7,\"Chill\":8,\"Rain\":9,\"RainRate\":10,\"UVI\":11,\"Usage\":\"5 Watt\"}],\"status\":\"OK\"}", &b, NULL) == DZ_OK);
  CHECK(b.n == 1 && b.d[0].n_rd == DZ_MAX_READINGS && sane_device(&b.d[0]));
  /* PlanIDs: bounded, zero dropped, junk skipped, PlanID used only when there is no array */
  CHECK(parse_str("{\"result\":[{\"idx\":\"7\",\"Type\":\"Temp\",\"Temp\":1,\"PlanIDs\":[0,3,\"x\",4,5,6,7,8],\"PlanID\":\"9\"}],\"status\":\"OK\"}", &b, NULL) == DZ_OK);
  CHECK(b.d[0].n_plan_ids == DZ_MAX_DEV_PLANS && b.d[0].plan_ids[0] == 3 && b.d[0].plan_ids[1] == 4 && sane_device(&b.d[0]));
  CHECK(parse_str("{\"result\":[{\"idx\":\"8\",\"Type\":\"Temp\",\"Temp\":1,\"PlanID\":\"9\"}],\"status\":\"OK\"}", &b, NULL) == DZ_OK && b.d[0].n_plan_ids == 1 && b.d[0].plan_ids[0] == 9);
  /* battery 255 = none */
  CHECK(parse_str("{\"result\":[{\"idx\":\"9\",\"Type\":\"Temp\",\"Temp\":1,\"BatteryLevel\":255}],\"status\":\"OK\"}", &b, NULL) == DZ_OK && !b.d[0].has_battery);
  /* the default -200..200 setpoint range is not a dial: shown, not controlled */
  CHECK(parse_str("{\"result\":[{\"idx\":\"10\",\"Type\":\"Setpoint\",\"SubType\":\"SetPoint\",\"SetPoint\":\"20.0\",\"min\":-200,\"max\":200,\"step\":0.5}],\"status\":\"OK\"}", &b, NULL) == DZ_OK);
  CHECK(b.d[0].cap == DZ_CAP_THERMOSTAT && b.d[0].has_setpoint_range && !b.d[0].can_set && !b.d[0].controllable);
  /* a protected thermostat is never controllable */
  CHECK(parse_str("{\"result\":[{\"idx\":\"11\",\"Type\":\"Setpoint\",\"SetPoint\":\"20.0\",\"min\":5,\"max\":30,\"step\":0.5,\"Protected\":true}],\"status\":\"OK\"}", &b, NULL) == DZ_OK);
  CHECK(b.d[0].can_set && !b.d[0].controllable);
  /* Modes: junk and overflow are cut at the bounds */
  CHECK(parse_str("{\"result\":[{\"idx\":\"12\",\"Type\":\"General\",\"SubType\":\"Thermostat Mode\",\"Mode\":0,\"Modes\":\"0;A;1;B;2;C;3;D;4;E;5;F;6;G;7;H;8;I;9;J;\",\"Data\":\"A\"}],\"status\":\"OK\"}", &b, NULL) == DZ_OK);
  CHECK(b.d[0].cap == DZ_CAP_MODE && b.d[0].n_modes == DZ_MAX_MODES && sane_device(&b.d[0]));
  CHECK(parse_str("{\"result\":[{\"idx\":\"13\",\"Type\":\"General\",\"SubType\":\"Thermostat Mode\",\"Mode\":0,\"Modes\":\"x;y\",\"Data\":\"A\"}],\"status\":\"OK\"}", &b, NULL) == DZ_OK);
  CHECK(b.d[0].cap != DZ_CAP_MODE && !b.d[0].controllable); /* unusable Modes: not a selector */
  /* a device whose switch type is unknown stays a plain device (never offered as a sensor control) */
  CHECK(parse_str("{\"result\":[{\"idx\":\"14\",\"Type\":\"Light/Switch\",\"SubType\":\"Switch\",\"SwitchTypeVal\":17,\"Data\":\"Off\"}],\"status\":\"OK\"}", &b, NULL) == DZ_OK);
  CHECK(b.d[0].cap == DZ_CAP_READONLY && !b.d[0].controllable);
  /* the wind temperature must not leak into the legacy fields */
  CHECK(parse_str("{\"result\":[{\"idx\":\"15\",\"Type\":\"Wind\",\"Temp\":12.5,\"Speed\":\"2\"}],\"status\":\"OK\"}", &b, NULL) == DZ_OK && !b.d[0].has_temp);
  CHECK(!strcmp(dz_rk_name(DZ_RK_AUXTEMP), "auxtemp") && !strcmp(dz_cap_name(DZ_CAP_SENSOR), "sensor") && !strcmp(dz_cap_name(DZ_CAP_MODE), "mode"));
}

static const char *const CLIMATE_FILES[] = {"mock_climate_metric.json", "mock_climate_imperial.json"};

/* Every prefix of a valid document must be rejected cleanly (a cut-off HTTP body), never crash. */
static void test_truncation(void) {
  const char *files[] = {"mock_getdevices.json", CLIMATE_FILES[0], CLIMATE_FILES[1]};
  for (size_t f = 0; f < 3; f++) {
    size_t len;
    char *js = slurp(files[f], &len);
    int ok = 0;
    for (size_t cut = 0; cut < len; cut++) {
      char *copy = malloc(cut ? cut : 1); /* exact-size heap block: ASAN catches any over-read */
      memcpy(copy, js, cut);
      if (dz_parse_response(copy, cut, NULL, NULL, NULL) == DZ_OK) ok++;
      free(copy);
    }
    CHECK(ok == 0);
    free(js);
  }
}

static bool check_sane_cb(const dz_device_t *d, void *u) {
  if (!sane_device(d)) ++*(int *)u;
  return true;
}

/* Seeded byte mutations: no crash, no hang. Result may be anything. */
static void test_mutation(void) {
  const char *files[] = {"mock_getdevices.json", CLIMATE_FILES[0], CLIMATE_FILES[1]};
  int insane = 0;
  for (size_t f = 0; f < 3; f++) {
  size_t len;
  char *js = slurp(files[f], &len);
  uint32_t seed = 12345u + (uint32_t)f;
  for (int iter = 0; iter < 20000; iter++) {
    char *copy = malloc(len);
    memcpy(copy, js, len);
    int flips = 1 + (int)(seed % 4);
    for (int k = 0; k < flips; k++) {
      seed = seed * 1664525u + 1013904223u;
      size_t pos = (seed >> 8) % len;
      seed = seed * 1664525u + 1013904223u;
      copy[pos] = (char)(seed >> 16);
    }
    (void)dz_parse_response(copy, len, check_sane_cb, &insane, NULL); /* every device, however mangled, keeps its invariants */
    free(copy);
    seed = seed * 1664525u + 1013904223u;
  }
  free(js);
  }
  CHECK(insane == 0);
}

/* ---- Domoticz web UI tabs: the rules of Helper.cpp IsLightOrSwitch/IsTemp/IsWeather/IsUtility ---- */
#define TL DZ_TAB_LIGHT
#define TT DZ_TAB_TEMP
#define TW DZ_TAB_WEATHER
#define TU DZ_TAB_UTILITY

static void test_tabs(void) {
  static const struct { const char *type, *sub; unsigned tabs; } T[] = {
    { "Light/Switch", "Switch", TL }, { "Light/Switch", "Selector Switch", TL }, { "Color Switch", "RGBWW", TL },
    { "Lighting 2", "AC", TL }, { "Lighting 1", "X10", TL }, { "Security", "X10 security", TL }, { "Blinds", "RAEX", TL },
    { "Curtain", "Harrison", TL }, { "Chime", "Byron SX", TL }, { "Fan", "Itho CVE RFT", TL }, { "RFY", "RFY", TL },
    { "Thermostat 3", "Mertik G6R-H4T1", TL }, { "Remote & IR", "PC Remote", TL }, { "FS20", "FS20", TL },
    { "Home Confort", "TEL-010", TL }, { "Hunter", "Hunter Fan", TL }, { "Honeywell", "Honeywell", TL },
    { "Radiator 1", "Smartwares Mode", TL }, { "Radiator 1", "Smartwares", TU },
    { "Heating", "Evohome", TL }, { "Heating", "Relay", TL }, { "Heating", "Zone", TT }, { "Heating", "Hot Water", TT },
    { "Temp", "LaCrosse TX3", TT }, { "Humidity", "LaCrosse TX3", TT }, { "Temp + Humidity", "THGN122/123", TT },
    { "Temp + Humidity + Baro", "THB1 - BTHR918, BTHGN129", TT | TW }, { "Temp + Baro", "BMP085 I2C", TT | TW },
    { "Thermostat 1", "Digimax", TT }, { "Thermostat 6", "Temp/Setpoint", TT },
    { "RFXSensor", "Temperature", TT }, { "RFXSensor", "Voltage", TU }, { "RFXSensor", "A/D", TU },
    { "General", "System temperature", TT },
    { "Wind", "TFA", TT | TW }, { "Wind", "WTGR800", TW }, { "Wind", "Weather Station", TW },
    { "UV", "TFA", TT | TW }, { "UV", "UVN128,UV138", TW }, { "UV", "UVN800", TW },
    { "Rain", "RainByRate", TW }, { "General", "Visibility", TW }, { "General", "Barometer", TW },
    { "General", "Solar Radiation", TW },
    { "P1 Smart Meter", "Energy", TU }, { "P1 Smart Meter", "Gas", TU }, { "Usage", "Electric", TU },
    { "RFXMeter", "RFXMeter counter", TU }, { "Setpoint", "SetPoint", TU }, { "Lux", "Lux", TU },
    { "Energy", "CM119 / CM160", TU }, { "Current", "CM113, Electrisave", TU }, { "Power", "Revolt", TU },
    { "Air Quality", "Voc", TU }, { "YouLess Meter", "YouLess counter", TU }, { "Weight", "BWR102", TU },
    { "General", "kWh", TU }, { "General", "Percentage", TU }, { "General", "Text", TU },
    { "General", "Custom Sensor", TU }, { "General", "Waterflow", TU }, { "General", "Pressure", TU },
    { "General", "Alert", TU }, { "General", "Voltage", TU }, { "General", "Current", TU },
    { "General", "Counter Incremental", TU }, { "General", "Managed Counter", TU }, { "General", "Soil Moisture", TU },
    { "General", "Leaf Wetness", TU }, { "General", "Sound Level", TU }, { "General", "Fan", TU },
    { "General", "Distance", TU },
    /* on no tab: only visible as favourites */
    { "Thermostat", "SetPoint", 0 }, { "General", "ZWave Alarm", 0 }, { "General", "Thermostat Mode", 0 },
    { "Setpoint", "Temperature", 0 }, { "Barometric", "x", 0 }, { "Gas", "x", 0 }, { "Water", "x", 0 },
    { "Scene", "", 0 }, { "Group", "", 0 }, { "Nonsense", "Nonsense", 0 }, { "", "", 0 },
  };
  for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
    unsigned g = dz_tabs_of(T[i].type, T[i].sub);
    CHECK(g == T[i].tabs);
    if (g != T[i].tabs) printf("  ^ tabs %s / %s: got %u want %u\n", T[i].type, T[i].sub, g, T[i].tabs);
  }
  CHECK(dz_tabs_of(NULL, NULL) == 0);

  /* every device of the real capture: the parser sets the same bits the table gives for its Type/SubType */
  size_t len;
  char *js = slurp("real_getdevices.json", &len);
  static bag_t b;
  b.n = 0;
  CHECK(dz_parse_response(js, len, collect, &b, NULL) == DZ_OK && b.n == 26);
  static const struct { const char *type; unsigned tabs; } real[] = {
    { "Color Switch", TL }, { "General", 0xFF }, /* General: by SubType, checked below */ { "Group", 0 }, { "Light/Switch", TL },
    { "Lighting 2", TL }, { "P1 Smart Meter", TU }, { "RFXMeter", TU }, { "Rain", TW }, { "Setpoint", TU }, { "Temp", TT },
    { "Temp + Humidity + Baro", TT | TW }, { "UV", TW }, { "Usage", TU }, { "Wind", TT | TW },
  };
  for (int i = 0; i < b.n; i++) {
    const dz_device_t *d = &b.d[i];
    bool known = false;
    for (size_t k = 0; k < sizeof real / sizeof real[0]; k++) {
      if (strcmp(d->type, real[k].type)) continue;
      known = true;
      if (real[k].tabs != 0xFF) CHECK(d->tabs == real[k].tabs);
    }
    CHECK(known); /* a new Type in the capture must be classified on purpose */
    if (!strcmp(d->type, "General")) {
      unsigned want = !strcmp(d->subtype, "Visibility") ? TW : TU;
      CHECK(d->tabs == want);
    }
  }
  free(js);

  /* classified from the FULL strings: this SubType is longer than dz_device_t.subtype and is cut there */
  static const char LONG[] =
    "{\"status\":\"OK\",\"result\":[{\"idx\":\"1\",\"Name\":\"x\",\"Type\":\"Temp + Humidity + Baro\","
    "\"SubType\":\"THB1 - BTHR918, BTHGN129\",\"Temp\":1,\"Humidity\":50}]}";
  b.n = 0;
  CHECK(dz_parse_response(LONG, strlen(LONG), collect, &b, NULL) == DZ_OK && b.n == 1);
  CHECK(strlen(b.d[0].subtype) == 23 && b.d[0].tabs == (TT | TW));
  /* key order does not matter: SubType before Type */
  static const char REV[] = "{\"status\":\"OK\",\"result\":[{\"SubType\":\"TFA\",\"idx\":\"2\",\"Type\":\"Wind\"}]}";
  b.n = 0;
  CHECK(dz_parse_response(REV, strlen(REV), collect, &b, NULL) == DZ_OK && b.n == 1 && b.d[0].tabs == (TT | TW));
}

static void test_mcp_reply(void) {
  dz_mcp_reply_t r;
  const char *ok = "{\"id\":7,\"jsonrpc\":\"2.0\",\"result\":{\"content\":[{\"text\":\"Switch \\\"Lamp 1\\\" set to Off successfully.\",\"type\":\"text\"}],\"isError\":false}}";
  CHECK(dz_mcp_parse_reply(ok, strlen(ok), &r) == DZ_OK && r.has_id && r.id == 7 && r.has_result && !r.has_error && !r.is_error && r.has_text);
  CHECK(!strcmp(r.text, "Switch \"Lamp 1\" set to Off successfully."));
  const char *ie = "{\"id\":8,\"jsonrpc\":\"2.0\",\"result\":{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"Device not found\"},{\"type\":\"text\",\"text\":\"second\"}]}}";
  CHECK(dz_mcp_parse_reply(ie, strlen(ie), &r) == DZ_OK && r.is_error && !strcmp(r.text, "Device not found"));
  const char *er = "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32600,\"message\":\"Missing session\",\"data\":{\"a\":[1,2]}}}";
  CHECK(dz_mcp_parse_reply(er, strlen(er), &r) == DZ_OK && r.has_error && r.code == -32600 && !strcmp(r.message, "Missing session") && !r.has_result);
  CHECK(!r.no_action);
  const char *na = "{\"id\":4,\"jsonrpc\":\"2.0\",\"result\":{\"content\":[{\"text\":\"Switch \\\"Bell\\\" was already On. No action taken.\",\"type\":\"text\"}],\"isError\":false}}";
  CHECK(dz_mcp_parse_reply(na, strlen(na), &r) == DZ_OK && r.no_action);
  char longna[600]; /* a 300 character device name: the stored text is truncated, the marker is still found */
  char nm[301];
  memset(nm, 'N', 300); nm[300] = 0;
  snprintf(longna, sizeof longna, "{\"id\":4,\"result\":{\"content\":[{\"text\":\"Switch %s was already On. No action taken.\"}],\"isError\":false}}", nm);
  CHECK(dz_mcp_parse_reply(longna, strlen(longna), &r) == DZ_OK && r.no_action && r.has_text && strlen(r.text) < 96 && !strstr(r.text, "action"));
  snprintf(longna, sizeof longna, "{\"id\":4,\"result\":{\"content\":[{\"text\":\"No actio\\\"n No actio\"}],\"isError\":false}}");
  CHECK(dz_mcp_parse_reply(longna, strlen(longna), &r) == DZ_OK && !r.no_action);
  const char *nid = "{\"id\":null,\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}";
  CHECK(dz_mcp_parse_reply(nid, strlen(nid), &r) == DZ_OK && !r.has_id && r.code == -32700);
  const char *neither = "{\"id\":1,\"jsonrpc\":\"2.0\"}";
  CHECK(dz_mcp_parse_reply(neither, strlen(neither), &r) == DZ_ERR_STATUS);
  CHECK(dz_mcp_parse_reply("<html>Accepted</html>", 21, &r) == DZ_ERR_SYNTAX);
  CHECK(dz_mcp_parse_reply("", 0, &r) == DZ_ERR_SYNTAX);
  CHECK(dz_mcp_parse_reply(NULL, 0, &r) == DZ_ERR_SYNTAX);
  CHECK(dz_mcp_parse_reply(ok, strlen(ok) - 1, &r) == DZ_ERR_SYNTAX);
  const char *trail = "{\"result\":{}} x";
  CHECK(dz_mcp_parse_reply(trail, strlen(trail), &r) == DZ_ERR_SYNTAX);
  /* long text and message are truncated, not rejected */
  char big[600];
  snprintf(big, sizeof big, "{\"id\":1,\"result\":{\"content\":[{\"text\":\"%0200d\"}],\"isError\":false},\"error\":{\"message\":\"%0100d\"}}", 1, 2);
  CHECK(dz_mcp_parse_reply(big, strlen(big), &r) == DZ_OK && strlen(r.text) == 95 && strlen(r.message) == 47);
  /* mutation: every truncation of a good reply is an error, never a crash */
  for (size_t n = 0; n < strlen(ok); n++) CHECK(dz_mcp_parse_reply(ok, n, &r) != DZ_OK);
  /* unknown keys with nested values and wrong types are skipped */
  const char *odd = "{\"x\":{\"y\":[[1],{\"z\":\"q\"}]},\"id\":\"abc\",\"result\":{\"content\":\"nope\",\"isError\":\"false\",\"extra\":[]}}";
  CHECK(dz_mcp_parse_reply(odd, strlen(odd), &r) == DZ_OK && !r.has_id && r.has_result && !r.is_error && !r.has_text);
}

int main(void) {
  test_fixture();
  test_real_capture();
  test_energy_and_ranges();
  test_scene_and_group();
  test_errors();
  test_robustness();
  test_climate_families(CLIMATE_FILES[0], false);
  test_climate_families(CLIMATE_FILES[1], true);
  test_climate_edges();
  test_truncation();
  test_mutation();
  test_tabs();
  test_mcp_reply();
  MT_DONE();
}
