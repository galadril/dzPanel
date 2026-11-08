/* The Domoticz apps as ru_app_t descriptors: Clock, Favourites, Lights, Scenes, Temperature, Weather, Utility, Energy, Settings. They share one
 * ru_dzapp_t (one model, one command layer, one feedback line) and differ in which section they show. */
#pragma once
#include "ru_app.h"
#include "ru_dzapp.h"
#include "ru_runtime.h"

#define RU_APP_ID_CLOCK "org.dzpanel.clock"
#define RU_APP_ID_HOME "org.dzpanel.home"
#define RU_APP_ID_LIGHTS "org.dzpanel.lights"
#define RU_APP_ID_SCENES "org.dzpanel.scenes"
#define RU_APP_ID_TEMP "org.dzpanel.temperature"
#define RU_APP_ID_WEATHER "org.dzpanel.weather"
#define RU_APP_ID_UTILITY "org.dzpanel.utility"
#define RU_APP_ID_ENERGY "org.dzpanel.energy"
#define RU_APP_ID_SETTINGS "org.dzpanel.settings"

/* Binds the shared state and registers the nine apps (the start page as the runtime's home). */
ru_err_t ru_apps_register(ru_runtime_t *rt, ru_dzapp_t *shared);
/* The descriptor of a section, NULL if out of range. */
const ru_app_t *ru_apps_descriptor(ru_section_t s);
/* Every app but Clock, Favourites and Settings is hidden by the launcher when this is false (ru_app_t has no such field yet). */
bool ru_apps_available(ru_section_t s);
/* Forget the shared state (call after ru_runtime_shutdown and before ru_dzapp_destroy). */
void ru_apps_unbind(void);
