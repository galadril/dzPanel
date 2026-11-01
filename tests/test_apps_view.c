/* View model: Scene vs Group semantics, energy units, unknown shows "--". Uses the parser on real-shaped JSON. */
#include <string.h>

#include "dz_parse.h"
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

int main(void) {
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
  MT_DONE();
}
