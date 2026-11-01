#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini_test.h"
#include "ru_climate.h"
#include "ru_dzview.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "tests/fixtures"
#endif

#define DEG "\xC2\xB0"
typedef struct { dz_device_t d[32]; int n; } bag_t;
static bool collect(const dz_device_t *d, void *u) { bag_t *b = u; if (b->n < 32) b->d[b->n++] = *d; return true; }
static const dz_device_t *find(const bag_t *b, const char *idx) {
  for (int i = 0; i < b->n; i++) if (!strcmp(b->d[i].idx, idx)) return &b->d[i];
  return NULL;
}
static char *slurp(const char *name) {
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
  return b;
}

static bag_t M, I; /* metric and imperial installs */
static dz_units_t UM, UI;

static void load(bag_t *b, const char *dev, const char *cfg, dz_units_t *u) {
  char *js = slurp(dev), *c = slurp(cfg);
  b->n = 0;
  CHECK(dz_parse_response(js, strlen(js), collect, b, NULL) == DZ_OK);
  free(js);
  memset(u, 0, sizeof *u);
  /* the unit parser lives in dz_client.c, not linked here: read the two keys the same strict way */
  if (strstr(c, "\"TempSign\": \"F\"")) { u->have_temp = true; strcpy(u->temp, "F"); }
  else if (strstr(c, "\"TempSign\": \"C\"")) { u->have_temp = true; strcpy(u->temp, "C"); }
  if (strstr(c, "\"WindSign\": \"mph\"")) { u->have_wind = true; strcpy(u->wind, "mph"); }
  else if (strstr(c, "\"WindSign\": \"m/s\"")) { u->have_wind = true; strcpy(u->wind, "m/s"); }
  free(c);
}

static void all_text(const bag_t *b, const dz_units_t *u, char *out, size_t cap) {
  /* every string any climate screen could show for these devices, concatenated */
  ru_cl_units_t cu;
  ru_cl_units(u, &cu);
  out[0] = 0;
  for (int i = 0; i < b->n; i++) {
    const dz_device_t *d = &b->d[i];
    char t[64];
    bool known;
    ru_dzv_value_text_u(d, u, t, sizeof t, &known);
    strncat(out, t, cap - strlen(out) - 1);
    strncat(out, "|", cap - strlen(out) - 1);
    if (d->cap == DZ_CAP_THERMOSTAT) {
      ru_cl_thermo_t th;
      ru_cl_thermo(d, u, NULL, true, &th);
      ru_cl_thermo_text(&th, th.setpoint, t, sizeof t);
      strncat(out, t, cap - strlen(out) - 1);
      strncat(out, "|", cap - strlen(out) - 1);
      continue;
    }
    ru_cl_view_t v;
    ru_cl_build(d, &cu, &v);
    strncat(out, v.hero, cap - strlen(out) - 1);
    strncat(out, "|", cap - strlen(out) - 1);
    strncat(out, v.speed, cap - strlen(out) - 1);
    strncat(out, v.speed_unit, cap - strlen(out) - 1);
    strncat(out, "|", cap - strlen(out) - 1);
    for (int k = 0; k < v.n_lines; k++) { strncat(out, v.line[k].text, cap - strlen(out) - 1); strncat(out, "|", cap - strlen(out) - 1); }
  }
}

static void test_units_in_text(void) {
  static char mt[8192], it[8192];
  all_text(&M, &UM, mt, sizeof mt);
  all_text(&I, &UI, it, sizeof it);
  /* a Fahrenheit/mph install shows F and mph and never C or m/s */
  CHECK(strstr(it, DEG "F") && strstr(it, "mph"));
  CHECK(!strstr(it, DEG "C") && !strstr(it, "m/s"));
  /* the metric install shows C and m/s and never F or mph */
  CHECK(strstr(mt, DEG "C") && strstr(mt, "m/s"));
  CHECK(!strstr(mt, DEG "F") && !strstr(mt, "mph"));
  /* nothing known: bare numbers, no unit symbol of any kind for temperature or wind */
  static char bt[8192];
  static bag_t nosp; /* setpoint devices may print the vunit the DEVICE reported: that is read, not guessed (tested in test_thermo) */
  nosp.n = 0;
  for (int i = 0; i < M.n; i++) if (M.d[i].cap != DZ_CAP_THERMOSTAT) nosp.d[nosp.n++] = M.d[i];
  all_text(&nosp, &(dz_units_t){0}, bt, sizeof bt);
  CHECK(!strstr(bt, DEG "C") && !strstr(bt, DEG "F") && !strstr(bt, "m/s") && !strstr(bt, "mph"));
  CHECK(strstr(bt, "21.5") && strstr(bt, "3.4"));
  /* the unit a General sensor wrote itself survives: it is Domoticz's word, not ours */
  CHECK(strstr(bt, "123 Lux") && strstr(bt, "850 ppm") && strstr(bt, "kg") && strstr(bt, "zorp"));
  /* an unknown wind unit with a known temperature unit */
  ru_cl_units_t cu;
  dz_units_t half = {.have_temp = true, .temp = "C"};
  ru_cl_units(&half, &cu);
  ru_cl_view_t v;
  ru_cl_build(find(&M, "104"), &cu, &v);
  CHECK(!strcmp(v.speed, "3.4") && !v.speed_unit[0]);
  dz_units_t bad = {.have_temp = true, .temp = "K"}; /* not a sign Domoticz produces: no symbol */
  ru_cl_units(&bad, &cu);
  CHECK(!cu.temp[0]);
}

static void test_wind_temp(void) {
  /* a Wind device's own thermometer is never "the temperature" and is labelled as its own */
  ru_cl_units_t cu;
  ru_cl_units(&UM, &cu);
  const dz_device_t *w = find(&M, "104"), *uv = find(&M, "106");
  CHECK(ru_cl_page_of(w) == RU_CLP_WIND && ru_cl_page_of(uv) == RU_CLP_UV);
  ru_cl_view_t v;
  ru_cl_build(w, &cu, &v);
  bool own = false;
  for (int i = 0; i < v.n_lines; i++) own = own || strstr(v.line[i].text, "SENSOR TEMP 12.0" DEG "C") != NULL;
  CHECK(own && v.has_dir && v.dir_deg == 315.0f && !strcmp(v.dir_txt, "NW") && !strcmp(v.speed, "3.4") && !strcmp(v.speed_unit, "m/s"));
  CHECK(v.hero[0] == 0); /* no big temperature on a wind page */
  char t[32];
  bool known;
  ru_dzv_value_text_u(w, &UM, t, sizeof t, &known);
  CHECK(known && !strcmp(t, "3.4 m/s"));
  /* a wind device with NO speed must not fall back to its thermometer for the list value */
  dz_device_t nw = *w;
  for (int i = 0; i < nw.n_rd; i++) if (nw.rd[i].kind == DZ_RK_WIND) { memmove(&nw.rd[i], &nw.rd[i + 1], (size_t)(nw.n_rd - i - 1) * sizeof nw.rd[0]); nw.n_rd--; break; }
  nw.dir_str[0] = 0;
  ru_cl_summary(&nw, &cu, t, sizeof t, &known);
  CHECK(strstr(t, "12.0") == NULL);
  /* setpoint NOW: only a real room temperature counts */
  ru_cl_thermo_t th;
  ru_cl_thermo(find(&M, "122"), &UM, NULL, true, &th);
  CHECK(!th.has_now);
  CHECK(ru_cl_page_of(uv) == RU_CLP_UV);
  ru_cl_build(uv, &cu, &v);
  CHECK(!strcmp(v.hero, "5") && !strcmp(v.word, "MODERATE") && v.ring > 0.4f && v.ring < 0.5f);
}

static void test_pages(void) {
  ru_cl_units_t cu;
  ru_cl_units(&UM, &cu);
  ru_cl_view_t v;
  ru_cl_build(find(&M, "100"), &cu, &v);
  CHECK(v.page == RU_CLP_TEMP && !strcmp(v.hero, "21.5" DEG "C") && v.trend == DZ_TREND_UP && v.ring > 0.6f && v.ring < 0.7f);
  CHECK(!strcmp(ru_cl_trend_word(v.trend), "RISING"));
  ru_cl_build(find(&M, "103"), &cu, &v);
  CHECK(v.page == RU_CLP_TEMP && v.n_lines == 3 && strstr(v.line[0].text, "HUMIDITY 52%  NORMAL") && strstr(v.line[1].text, "1009 hPa  SUNNY") &&
        strstr(v.line[2].text, "DEW POINT 10.0" DEG "C"));
  ru_cl_build(find(&M, "101"), &cu, &v);
  CHECK(v.page == RU_CLP_HUM && !strcmp(v.hero, "71%") && !strcmp(v.word, "WET") && v.ring > 0.7f);
  ru_cl_build(find(&M, "105"), &cu, &v);
  CHECK(v.page == RU_CLP_RAIN && !strcmp(v.hero, "12.5") && v.n_lines == 1 && !strcmp(v.line[0].text, "RATE 0.4")); /* no invented unit */
  ru_cl_build(find(&M, "107"), &cu, &v);
  CHECK(v.page == RU_CLP_VALUE && !strcmp(v.hero, "123") && v.n_lines >= 1 && !strcmp(v.line[0].text, "Lux"));
  ru_cl_build(find(&M, "108"), &cu, &v);
  CHECK(v.page == RU_CLP_AIR && !strcmp(v.hero, "850") && !strcmp(v.word, "FAIR"));
  ru_cl_build(find(&M, "113"), &cu, &v);
  CHECK(v.page == RU_CLP_TEXT && !strcmp(v.text, "All good"));
  /* an unseen type renders through the generic page */
  ru_cl_build(find(&M, "116"), &cu, &v);
  CHECK(v.page == RU_CLP_VALUE && !strcmp(v.hero, "7.5") && !strcmp(v.line[0].text, "zorp") && !strcmp(v.caption, "VALUE"));
  /* the UV words follow the WHO scale */
  CHECK(!strcmp(ru_cl_uv_word(0), "LOW") && !strcmp(ru_cl_uv_word(2.9f), "LOW") && !strcmp(ru_cl_uv_word(3), "MODERATE") &&
        !strcmp(ru_cl_uv_word(6), "HIGH") && !strcmp(ru_cl_uv_word(8), "VERY HIGH") && !strcmp(ru_cl_uv_word(11), "EXTREME") &&
        !ru_cl_uv_word(-1)[0] && !ru_cl_uv_word(NAN)[0]);
  /* battery */
  dz_device_t d = *find(&M, "100");
  CHECK(!ru_cl_battery_low(&d));
  d.battery = 10;
  CHECK(ru_cl_battery_low(&d));
  d.has_battery = false;
  CHECK(!ru_cl_battery_low(&d)); /* unknown is not low */
  /* unknown reading shows "--", never 0 */
  char t[32];
  ru_cl_text(NULL, &cu, t, sizeof t);
  CHECK(!strcmp(t, "--"));
  memset(&d, 0, sizeof d);
  bool known;
  ru_cl_summary(&d, &cu, t, sizeof t, &known);
  CHECK(!known && !strcmp(t, "--"));
  /* number formatting: -0.0, integral, small fractions */
  dz_reading_t r = {.kind = DZ_RK_TEMP, .has = true, .v = -0.04f};
  ru_cl_text(&r, &cu, t, sizeof t);
  CHECK(!strcmp(t, "0.0" DEG "C"));
  r.kind = DZ_RK_NUMBER; r.v = 0.05f; strcpy(r.unit, "V");
  ru_cl_text(&r, &cu, t, sizeof t);
  CHECK(!strcmp(t, "0.05 V"));
  r.v = 1234.5f; r.unit[0] = 0;
  ru_cl_text(&r, &cu, t, sizeof t);
  CHECK(!strcmp(t, "1234") || !strcmp(t, "1235"));
}

static void test_thermo(void) {
  ru_cl_thermo_t th;
  const dz_device_t *sp = find(&M, "118"), *t6 = find(&M, "117"), *rad = find(&M, "123"), *evo = find(&M, "124"), *two = find(&M, "121");
  /* plain setpoint: unit from TempSign; NOW from the single temperature sensor of its plan */
  const dz_device_t *room = find(&M, "100");
  ru_cl_thermo(sp, &UM, room, true, &th);
  CHECK(th.control && th.is_temp && !strcmp(th.unit, DEG "C") && th.lo == 5.0f && th.hi == 30.0f && th.step == 0.5f);
  CHECK(th.has_now && th.now_from_plan && fabsf(th.now - 21.5f) < 0.01f);
  char t[24];
  ru_cl_thermo_text(&th, th.setpoint, t, sizeof t);
  CHECK(!strcmp(t, "19.0" DEG "C"));
  /* no plan sensor: NOW unknown, never made up */
  ru_cl_thermo(sp, &UM, NULL, true, &th);
  CHECK(!th.has_now);
  /* own temperature wins over the plan's */
  ru_cl_thermo(t6, &UM, room, true, &th);
  CHECK(th.has_now && !th.now_from_plan && fabsf(th.now - 20.5f) < 0.01f);
  ru_cl_thermo(evo, &UM, NULL, true, &th);
  CHECK(!th.control && th.has_now && !strcmp(th.reason, "NO RANGE") && th.has_setpoint);
  ru_cl_thermo(rad, &UM, NULL, true, &th);
  CHECK(!th.control && !strcmp(th.reason, "NO RANGE"));
  ru_cl_thermo(sp, &UM, NULL, false, &th); /* a viewer token */
  CHECK(!th.control && !strcmp(th.reason, "VIEW ONLY"));
  dz_device_t prot = *sp;
  prot.is_protected = true;
  prot.controllable = false;
  ru_cl_thermo(&prot, &UM, NULL, true, &th);
  CHECK(!th.control && !strcmp(th.reason, "PROTECTED"));
  /* Fahrenheit install */
  ru_cl_thermo(find(&I, "118"), &UI, find(&I, "100"), true, &th);
  CHECK(!strcmp(th.unit, DEG "F") && th.lo > 40.9f && th.hi < 86.1f);
  ru_cl_thermo_text(&th, th.setpoint, t, sizeof t);
  CHECK(!strcmp(t, "66.2" DEG "F") && th.now > 70.6f && th.now < 70.8f);
  /* TempSign unknown: the unit the device itself reported (vunit), else bare */
  ru_cl_thermo(sp, &(dz_units_t){0}, NULL, true, &th);
  CHECK(!strcmp(th.unit, DEG "C"));
  dz_device_t nov = *sp;
  nov.vunit[0] = 0;
  ru_cl_thermo(&nov, &(dz_units_t){0}, NULL, true, &th);
  CHECK(!th.unit[0]);
  ru_cl_thermo_text(&th, 19.0f, t, sizeof t);
  CHECK(!strcmp(t, "19.0"));
  /* a setpoint that is not a temperature: its own unit, and no temperature NOW */
  dz_device_t pct = *sp;
  strcpy(pct.vunit, "%");
  ru_cl_thermo(&pct, &UM, room, true, &th);
  CHECK(!th.is_temp && !strcmp(th.unit, "%") && !th.has_now);
  strcpy(pct.vunit, "bar");
  ru_cl_thermo(&pct, &UM, room, true, &th);
  ru_cl_thermo_text(&th, 2.5f, t, sizeof t);
  CHECK(!strcmp(t, "2.5 bar"));
  (void)two;
  /* mode device */
  const dz_device_t *mode = find(&M, "125");
  char nm[24];
  CHECK(ru_cl_mode_name(mode, 1, nm, sizeof nm) && !strcmp(nm, "HEAT") && !ru_cl_mode_name(mode, 3, nm, sizeof nm));
  CHECK(ru_cl_mode_slot(mode) == 1);
  dz_device_t m2 = *mode;
  m2.mode = 7;
  CHECK(ru_cl_mode_slot(&m2) == -1);
  char vt[32];
  bool known;
  ru_dzv_value_text_u(mode, &UM, vt, sizeof vt, &known);
  CHECK(known && !strcmp(vt, "HEAT"));
}

static void test_every_device_has_a_value(void) {
  /* nothing in either fixture set is hidden or blank: every device yields a list value, and none says 0 for unknown */
  for (int s = 0; s < 2; s++) {
    const bag_t *b = s ? &I : &M;
    for (int i = 0; i < b->n; i++) {
      char t[48];
      bool known;
      ru_dzv_value_text_u(&b->d[i], s ? &UI : &UM, t, sizeof t, &known);
      CHECK(t[0] != 0);
      if (b->d[i].cap == DZ_CAP_SENSOR || b->d[i].cap == DZ_CAP_WEATHER || b->d[i].cap == DZ_CAP_TEMP) CHECK(known);
    }
  }
}

int main(void) {
  load(&M, "mock_climate_metric.json", "mock_getconfig_metric.json", &UM);
  load(&I, "mock_climate_imperial.json", "mock_getconfig_imperial.json", &UI);
  CHECK(M.n == 26 && I.n == 26 && UM.have_temp && UI.have_wind);
  test_units_in_text();
  test_wind_temp();
  test_pages();
  test_thermo();
  test_every_device_has_a_value();
  MT_DONE();
}
