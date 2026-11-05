/* Which Domoticz web UI tabs (getdevices filter=light|temp|weather|utility) list a device, from the JSON "Type" and
 * "SubType" strings (RFX_Type_Desc / RFX_Type_SubType_Desc). Pure, no state. A device can be on several tabs
 * (Temp + Humidity + Baro is on Temperature AND Weather) or on none (Thermostat, General ZWave alarms: they only
 * show as favourites). Unknown Type/SubType pairs have no tab, as in Domoticz. Scenes and groups are not devices and
 * are not classified here. */
#pragma once
#include <stdint.h>

#define DZ_TAB_LIGHT 0x01u
#define DZ_TAB_TEMP 0x02u
#define DZ_TAB_WEATHER 0x04u
#define DZ_TAB_UTILITY 0x08u

/* NULL is the same as "". Compare the FULL strings (not the truncated ones in dz_device_t). */
uint8_t dz_tabs_of(const char *type, const char *subtype);
