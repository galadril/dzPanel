/* View model: Scene vs Group semantics, energy units, unknown shows "--". Uses the parser on real-shaped JSON. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dz_parse.h"
#include "dz_tabs.h"
#include "mini_test.h"
#include "ru_dzview.h"

typedef struct { dz_device_t d; int n; } box_t;
static bool grab(const dz_device_t *x, void *u) { box_t *b = u; b->d = *x; b->n++; return true; }

static dz_device_t one(const char *json) {
  static char buf[2048];
  box_t box;
  memset(&box, 0, sizeof box);
  snprintf(buf, sizeof buf, "{\"result\":[%s],\"status\":\"OK\"}", json);
  CHECK(dz_parse_response(buf, strlen(buf), grab, &box, NULL) == DZ_OK);
  CHECK(box.n == 1);
  return box.d;
}

/* Every class of the Domoticz Switches tab (tests/fixtures/switch_types.json) is on the Lights tab, has a capability
 * and a non-empty tile text; the read-only ones are never controllable. */
typedef struct { dz_device_t d[40]; int n; } bag_t;
static bool collect(const dz_device_t *x, void *u) { bag_t *b = u; if (b->n < 40) b->d[b->n++] = *x; return true; }

static void switch_classes(void) {
  static bag_t b;
  FILE *f = fopen(FIXTURE_DIR "/switch_types.json", "rb");
  CHECK(f != NULL);
  if (!f) return;
  static char js[65536];
  size_t len = fread(js, 1, sizeof js - 1, f);
  fclose(f);
  js[len] = 0;
  dz_result_t r;
  CHECK(dz_parse_response(js, len, collect, &b, &r) == DZ_OK && r.rejected == 0 && b.n == 32);
  /* by idx: the expected capability */
  static const dz_cap_t want[33] = {
    0, DZ_CAP_ONOFF, DZ_CAP_DIMMER, DZ_CAP_DIMMER, DZ_CAP_BLINDS, DZ_CAP_BLINDS, DZ_CAP_BLINDS, DZ_CAP_BLINDS, DZ_CAP_BLINDS,
    DZ_CAP_BLINDS, DZ_CAP_BLINDS, DZ_CAP_BLINDS, DZ_CAP_SELECTOR, DZ_CAP_PUSH, DZ_CAP_PUSH, DZ_CAP_PUSH, DZ_CAP_PUSH,
    DZ_CAP_LOCK, DZ_CAP_LOCK, DZ_CAP_STATUS, DZ_CAP_STATUS, DZ_CAP_STATUS, DZ_CAP_STATUS, DZ_CAP_STATUS, DZ_CAP_STATUS,
    DZ_CAP_READONLY, DZ_CAP_READONLY, DZ_CAP_STATUS /* Security Panel: setsecstatus, not switchlight */, DZ_CAP_ONOFF, DZ_CAP_ONOFF, DZ_CAP_ONOFF, DZ_CAP_SENSOR /* Heating Relay: shown, not switched */, DZ_CAP_ONOFF,
  };
  for (int i = 0; i < b.n; i++) {
    const dz_device_t *d = &b.d[i];
    int ix = atoi(d->idx);
    CHECK(ix >= 1 && ix <= 32);
    CHECK((d->tabs & DZ_TAB_LIGHT) != 0);
    if (ix >= 1 && ix <= 32 && d->cap != want[ix]) { printf("idx %d: cap %s want %s\n", ix, dz_cap_name(d->cap), dz_cap_name(want[ix])); CHECK(d->cap == want[ix]); }
    char t[40];
    bool known;
    ru_dzv_value_text(d, t, sizeof t, &known);
    CHECK(t[0] != 0);
    if (d->cap == DZ_CAP_STATUS || d->cap == DZ_CAP_READONLY || d->cap == DZ_CAP_SENSOR) CHECK(!d->controllable);
    if (d->cap == DZ_CAP_BLINDS || d->cap == DZ_CAP_LOCK || d->cap == DZ_CAP_SELECTOR) CHECK(d->controllable || d->is_protected);
  }
}

static void clock_and_gauges(void) {
  char hm[6], dt[12];
  int64_t ep;
  CHECK(dz_time_parse("2026-10-31 13:07:41", &ep));
  ru_dzv_clock_text(ep, hm);
  ru_dzv_date_text(ep, dt);
  CHECK(!strcmp(hm, "13:07") && !strcmp(dt, "SAT 31 OCT"));
  CHECK(dz_time_parse("2024-02-29 00:00:00", &ep)); /* leap day, midnight */
  ru_dzv_clock_text(ep, hm); ru_dzv_date_text(ep, dt);
  CHECK(!strcmp(hm, "00:00") && !strcmp(dt, "THU 29 FEB"));
  CHECK(dz_time_parse("2026-12-31 23:59:59", &ep));
  ru_dzv_clock_text(ep, hm); ru_dzv_date_text(ep, dt);
  CHECK(!strcmp(hm, "23:59") && !strcmp(dt, "THU 31 DEC"));
  ru_dzv_date_text(ep + 1, dt);
  CHECK(!strcmp(dt, "FRI 1 JAN"));
  CHECK(dz_time_parse("2026-03-01 09:05:00", &ep));
  ru_dzv_clock_text(ep, hm); ru_dzv_date_text(ep, dt);
  CHECK(!strcmp(hm, "09:05") && !strcmp(dt, "SUN 1 MAR"));
  ru_dzv_clock_text(0, hm); ru_dzv_date_text(0, dt); /* the epoch itself */
  CHECK(!strcmp(hm, "00:00") && !strcmp(dt, "THU 1 JAN"));
  ru_dzv_clock_text(-1, hm); ru_dzv_date_text(-1, dt); /* before it: floor, not truncate */
  CHECK(!strcmp(hm, "23:59") && !strcmp(dt, "WED 31 DEC"));

  dz_units_t c_units = {.have_temp = true, .temp = "C"}, f_units = {.have_temp = true, .temp = "F"};
  ru_gauge_view_t gv;
  dz_device_t d = one("{\"idx\":\"30\",\"Name\":\"Living Room Sensor\",\"Type\":\"Temp\",\"SubType\":\"LaCrosse TX3\",\"Temp\":20.0,\"Data\":\"20.0 C\"}");
  CHECK(ru_dzv_gauge(&d, &c_units, &gv) && gv.known && gv.kind == RU_GAUGE_TEMP);
  CHECK(!strcmp(gv.text, "20.0\xC2\xB0" "C") && gv.frac > 0.499f && gv.frac < 0.501f);
  CHECK(!strcmp(gv.caption, "LIVING R")); /* 8 characters, no trailing space issue */
  CHECK(ru_dzv_gauge(&d, &f_units, &gv) && gv.known && !strcmp(gv.text, "20.0\xC2\xB0" "F") && gv.frac < 0.0f + 1.0f);
  CHECK(gv.frac == 0.0f); /* 20 F is below the 32 F end */
  d.rd[0].v = 68.0f;
  CHECK(ru_dzv_gauge(&d, &f_units, &gv) && gv.frac > 0.499f && gv.frac < 0.501f);
  d.rd[0].v = 55.0f; /* above 40 C: pinned, not wrapped */
  CHECK(ru_dzv_gauge(&d, &c_units, &gv) && gv.frac == 1.0f);
  d.rd[0].v = 20.0f;
  CHECK(ru_dzv_gauge(&d, NULL, &gv) && gv.known && gv.kind == RU_GAUGE_TEXT && gv.frac < 0.0f && !strcmp(gv.text, "20.0")); /* no unit: no scale */
  d.n_rd = 0;
  CHECK(ru_dzv_gauge(&d, &c_units, &gv) && !gv.known && !strcmp(gv.text, "--") && gv.frac < 0.0f); /* no value is not 0 */

  d = one("{\"idx\":\"31\",\"Name\":\"Hum\",\"Type\":\"Humidity\",\"SubType\":\"LaCrosse TX3\",\"Humidity\":78,\"HumidityStatus\":\"Wet\",\"Data\":\"Humidity 78 %\"}");
  CHECK(ru_dzv_gauge(&d, &c_units, &gv) && gv.known && gv.kind == RU_GAUGE_PERCENT && !strcmp(gv.text, "78%"));
  CHECK(gv.frac > 0.779f && gv.frac < 0.781f && !strcmp(gv.caption, "HUM"));

  d = one("{\"idx\":\"32\",\"Name\":\"Mains\",\"Type\":\"Usage\",\"SubType\":\"Electric\",\"Data\":\"2100 Watt\",\"Usage\":\"2100 Watt\"}");
  CHECK(ru_dzv_gauge(&d, NULL, &gv) && gv.known && gv.kind == RU_GAUGE_POWER && !strcmp(gv.text, "2.1kW"));
  CHECK(gv.frac > 0.419f && gv.frac < 0.421f); /* 2100 of a 5000 W range */
  d.usage_w = 850;
  CHECK(ru_dzv_gauge(&d, NULL, &gv) && !strcmp(gv.text, "850W") && gv.frac > 0.849f && gv.frac < 0.851f);
  d.usage_w = 0; /* a real zero is a zero */
  CHECK(ru_dzv_gauge(&d, NULL, &gv) && gv.known && !strcmp(gv.text, "0W") && gv.frac == 0.0f);
  d.has_usage = false;
  CHECK(ru_dzv_gauge(&d, NULL, &gv) && !gv.known && !strcmp(gv.text, "--"));

  /* a numeric reading without a scale: text only, ring unlit */
  d = one("{\"idx\":\"33\",\"Name\":\"Wind\",\"Type\":\"Wind\",\"SubType\":\"WTGR800\",\"Direction\":225,\"DirectionStr\":\"SW\",\"Speed\":\"3.9\",\"Gust\":\"6.1\",\"Data\":\"SW;225;3.9;6.1;12.4;10.1\"}");
  dz_units_t wu = {.have_wind = true, .wind = "m/s"};
  CHECK(ru_dzv_gauge(&d, &wu, &gv) && gv.known && gv.kind == RU_GAUGE_TEXT && gv.frac < 0.0f && !strcmp(gv.text, "3.9 m/s"));

  /* not gauges: switches, scenes */
  d = one("{\"idx\":\"3\",\"Name\":\"L\",\"Type\":\"Light/Switch\",\"SwitchType\":\"On/Off\",\"Status\":\"On\"}");
  CHECK(!ru_dzv_gauge(&d, &c_units, &gv) && !gv.known && !strcmp(gv.text, "--"));
  d = one("{\"idx\":\"1\",\"Name\":\"S\",\"Type\":\"Scene\",\"Status\":\"On\"}");
  CHECK(!ru_dzv_gauge(&d, &c_units, &gv));
  CHECK(ru_dzv_nice_max(0.0f) == 1.0f && ru_dzv_nice_max(1200.0f) == 2000.0f && ru_dzv_nice_max(2100.0f) == 5000.0f);
}

int main(void) {
  clock_and_gauges();
  /* ---- Scene: push only, no state, tap is always On ---- */
  dz_device_t sc = one("{\"idx\":\"1\",\"Name\":\"S\",\"Type\":\"Scene\",\"Status\":\"On\"}");
  CHECK(sc.cap == DZ_CAP_SCENE);
  CHECK(ru_dzv_tile_state(&sc) == RU_TILE_PUSH);
  CHECK(!strcmp(ru_dzv_tile_label(RU_TILE_PUSH), "SCENE"));
  dz_cmd_kind_t k;
  const char *verb;
  CHECK(ru_dzv_scene_tap(&sc, &k, &verb) && k == DZ_CMD_SCENE_ON && !strcmp(verb, "ACTIVATE"));
  strcpy(sc.status, "Off");
  CHECK(ru_dzv_scene_tap(&sc, &k, &verb) && k == DZ_CMD_SCENE_ON); /* a Scene never sends Off */
  strcpy(sc.status, "Mixed");
  CHECK(ru_dzv_scene_tap(&sc, &k, &verb) && k == DZ_CMD_SCENE_ON);

  /* ---- Group: On / Off / Mixed, tap: On->Off, Off/Mixed->On ---- */
  dz_device_t g = one("{\"idx\":\"2\",\"Name\":\"G\",\"Type\":\"Group\",\"Status\":\"On\"}");
  CHECK(g.cap == DZ_CAP_GROUP);
  CHECK(ru_dzv_tile_state(&g) == RU_TILE_ON);
  CHECK(ru_dzv_scene_tap(&g, &k, &verb) && k == DZ_CMD_SCENE_OFF && !strcmp(verb, "SWITCH OFF"));
  strcpy(g.status, "Off");
  CHECK(ru_dzv_tile_state(&g) == RU_TILE_OFF);
  CHECK(ru_dzv_scene_tap(&g, &k, &verb) && k == DZ_CMD_SCENE_ON && !strcmp(verb, "SWITCH ON"));
  strcpy(g.status, "Mixed");
  CHECK(ru_dzv_tile_state(&g) == RU_TILE_MIXED && !strcmp(ru_dzv_tile_label(RU_TILE_MIXED), "MIXED"));
  CHECK(ru_dzv_scene_tap(&g, &k, &verb) && k == DZ_CMD_SCENE_ON);
  g.status[0] = 0;
  CHECK(ru_dzv_tile_state(&g) == RU_TILE_UNKNOWN && !strcmp(ru_dzv_tile_label(RU_TILE_UNKNOWN), "--"));
  CHECK(ru_dzv_scene_tap(&g, &k, &verb) && k == DZ_CMD_SCENE_ON); /* unknown state: On, never a blind Off */

  /* a plain device is not a scene */
  dz_device_t sw = one("{\"idx\":\"3\",\"Name\":\"L\",\"Type\":\"Light/Switch\",\"SwitchType\":\"On/Off\",\"Status\":\"On\"}");
  CHECK(!ru_dzv_scene_tap(&sw, &k, &verb));
  CHECK(ru_dzv_tile_state(&sw) == RU_TILE_ON);

  /* ---- energy: W below 1000, kW from there, direction, unit from the server ---- */
  char t[32];
  ru_dzv_power_text(15, t, sizeof t); CHECK(!strcmp(t, "15 W"));
  ru_dzv_power_text(999, t, sizeof t); CHECK(!strcmp(t, "999 W"));
  ru_dzv_power_text(1000, t, sizeof t); CHECK(!strcmp(t, "1.00 kW"));
  ru_dzv_power_text(2100, t, sizeof t); CHECK(!strcmp(t, "2.10 kW"));
  ru_dzv_power_text(0, t, sizeof t); CHECK(!strcmp(t, "0 W")); /* a real zero is a zero */

  dz_device_t e = one("{\"idx\":\"4\",\"Name\":\"P\",\"Type\":\"P1 Smart Meter\",\"SubType\":\"Energy\","
                      "\"Data\":\"1;2;3;4;15;0\",\"Usage\":\"15 Watt\",\"UsageDeliv\":\"0 Watt\",\"CounterToday\":\"7.397 kWh\"}");
  ru_energy_view_t ev;
  ru_dzv_energy(&e, &ev);
  CHECK(ev.known && !strcmp(ev.num, "15") && !strcmp(ev.unit, "W"));
  CHECK(ev.has_today && !strcmp(ev.today, "TODAY 7.4 kWh"));
  CHECK(ev.flow == RU_FLOW_IMPORT && !strcmp(ev.flow_text, "IMPORT 15 W"));
  e.usage_w = 2500;
  ru_dzv_energy(&e, &ev);
  CHECK(!strcmp(ev.num, "2.50") && !strcmp(ev.unit, "kW"));
  e.usage_w = 0; e.usage_deliv_w = 1200; e.has_usage_deliv = true;
  ru_dzv_energy(&e, &ev);
  CHECK(ev.flow == RU_FLOW_EXPORT && !strcmp(ev.num, "1.20") && !strcmp(ev.unit, "kW") && ev.power_w < 0);
  CHECK(!strcmp(ev.flow_text, "EXPORT 1.20 kW"));

  /* unknown is "--", never 0; no guessed unit */
  dz_device_t u;
  memset(&u, 0, sizeof u);
  u.cap = DZ_CAP_ENERGY;
  ru_dzv_energy(&u, &ev);
  CHECK(!ev.known && !strcmp(ev.num, "--") && !ev.has_today && !strcmp(ev.today, "--") && ev.flow == RU_FLOW_NONE);
  u.has_today = true; u.today = 3.0f; /* a counter without a unit: the number only */
  ru_dzv_energy(&u, &ev);
  CHECK(ev.has_today && !strcmp(ev.today, "TODAY 3.0") && !strstr(ev.today, "kWh") && !strstr(ev.today, "W"));
  bool known;
  memset(&u, 0, sizeof u);
  u.cap = DZ_CAP_TEMP;
  ru_dzv_value_text(&u, t, sizeof t, &known);
  CHECK(!known && !strcmp(t, "--"));
  u.cap = DZ_CAP_ONOFF; /* no Status: unknown, not OFF */
  ru_dzv_value_text(&u, t, sizeof t, &known);
  CHECK(!known && !strcmp(t, "--"));
  u.cap = DZ_CAP_DIMMER;
  ru_dzv_value_text(&u, t, sizeof t, &known);
  CHECK(!known && !strcmp(t, "--"));
  u.cap = DZ_CAP_TEMP; u.has_temp = true; u.temp = 0.0f; /* a real 0 degrees */
  ru_dzv_value_text(&u, t, sizeof t, &known);
  CHECK(known && !strncmp(t, "0.0", 3));

  /* selector names and clamping */
  dz_device_t sel;
  memset(&sel, 0, sizeof sel);
  strcpy(sel.level_names, "Off|Low|Medium|High");
  CHECK(ru_dzv_selector_name(&sel, 2, t, sizeof t) && !strcmp(t, "MEDIUM"));
  CHECK(!ru_dzv_selector_name(&sel, 4, t, sizeof t));
  char c[8];
  ru_dzv_clamp(c, sizeof c, "caf\xC3\xA9 au lait", 4); /* cut inside the UTF-8 pair */
  CHECK(!strcmp(c, "caf..") );
  switch_classes();
  MT_DONE();
}
