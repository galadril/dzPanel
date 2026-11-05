/* Mirror of Domoticz main/Helper.cpp IsLightOrSwitch / IsTemp / IsWeather / IsUtility and the filter=light branch of
 * WebServer.cpp GetJSonDevices, keyed by the JSON strings of main/RFXNames.cpp (development branch, verified).
 * Where several Domoticz types share one string ("Security", "Heating", "Light/Switch", "Blinds") the rule is the
 * same for all of them or told apart by the SubType string. Known ambiguity: Rego 6XX "Value" devices (Status on the
 * Light tab, Counter on Utility) both carry SubType "Rego 6XX", so they cannot be told apart and get no tab. */
#include "dz_tabs.h"

#include <string.h>

#define L DZ_TAB_LIGHT
#define T DZ_TAB_TEMP
#define W DZ_TAB_WEATHER
#define U DZ_TAB_UTILITY

typedef struct { const char *type; uint8_t tabs; } type_rule_t;
typedef struct { const char *type, *sub; uint8_t tabs; } sub_rule_t;

/* every SubType of the Type is on these tabs */
static const type_rule_t k_type[] = {
  { "Lighting 1", L }, { "Lighting 2", L }, { "Lighting 3", L }, { "Lighting 4", L }, { "Lighting 5", L },
  { "Lighting 6", L }, { "Fan", L }, { "Color Switch", L }, { "Security", L }, { "Curtain", L }, { "Blinds", L },
  { "Chime", L }, { "RFY", L }, { "Thermostat 2", L }, { "Thermostat 3", L }, { "Thermostat 4", L },
  { "Remote & IR", L }, { "Light/Switch", L }, { "Home Confort", L }, { "FS20", L }, { "Hunter", L },
  { "Honeywell", L },
  { "Temp", T }, { "Humidity", T }, { "Temp + Humidity", T }, { "Temp + Humidity + Baro", T | W },
  { "Temp + Baro", T | W }, { "Thermostat 1", T }, { "Thermostat 6", T },
  { "Rain", W },
  { "P1 Smart Meter", U }, { "Lux", U }, { "Current", U }, { "Current/Energy", U }, { "Energy", U }, { "Power", U },
  { "YouLess Meter", U }, { "Air Quality", U }, { "Usage", U }, { "Weight", U }, { "RFXMeter", U }, { "Setpoint", 0 },
};

/* the SubType decides (the string is the full Domoticz one, e.g. "TFA" for Wind4 and UV3) */
static const sub_rule_t k_sub[] = {
  { "Wind", "TFA", T | W }, /* sTypeWIND4; every other Wind subtype is weather only (rule below) */
  { "UV", "TFA", T | W },   /* sTypeUV3 */
  { "Heating", "Evohome", L }, { "Heating", "Relay", L }, { "Heating", "Zone", T }, { "Heating", "Hot Water", T },
  { "Radiator 1", "Smartwares Mode", L }, { "Radiator 1", "Smartwares", U },
  { "RFXSensor", "Temperature", T }, { "RFXSensor", "Voltage", U }, { "RFXSensor", "A/D", U },
  { "Setpoint", "SetPoint", U },
  { "General", "System temperature", T },
  { "General", "Visibility", W }, { "General", "Barometer", W }, { "General", "Solar Radiation", W },
  { "General", "kWh", U }, { "General", "Voltage", U }, { "General", "Current", U }, { "General", "Text", U },
  { "General", "Percentage", U }, { "General", "Counter Incremental", U }, { "General", "Managed Counter", U },
  { "General", "Waterflow", U }, { "General", "Custom Sensor", U }, { "General", "Alert", U },
  { "General", "Pressure", U }, { "General", "Soil Moisture", U }, { "General", "Leaf Wetness", U },
  { "General", "Sound Level", U }, { "General", "Fan", U }, { "General", "Distance", U },
};

uint8_t dz_tabs_of(const char *type, const char *subtype) {
  if (!type) type = "";
  if (!subtype) subtype = "";
  for (size_t i = 0; i < sizeof k_sub / sizeof k_sub[0]; i++)
    if (!strcmp(type, k_sub[i].type) && !strcmp(subtype, k_sub[i].sub)) return k_sub[i].tabs;
  for (size_t i = 0; i < sizeof k_type / sizeof k_type[0]; i++)
    if (!strcmp(type, k_type[i].type)) return k_type[i].tabs;
  /* Wind and UV that are not the TFA subtype: weather only. Temp-bearing subtypes are handled above. */
  if (!strcmp(type, "Wind") || !strcmp(type, "UV")) return W;
  return 0;
}
