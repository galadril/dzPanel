#include "ru_apps.h"

#include <string.h>

static ru_dzapp_t *g_shared;

typedef struct { ru_dzapp_t *app; ru_section_t sec; ru_heap_t *heap; } app_state_t;

static ru_err_t create_sec(const ru_app_env_t *env, void **out, ru_section_t sec) {
  if (!g_shared) return RU_ERR_INVALID;
  app_state_t *s = ru_heap_alloc(env->heap, sizeof *s);
  if (!s) return RU_ERR_NOMEM;
  s->app = g_shared;
  s->sec = sec;
  s->heap = env->heap;
  *out = s;
  return RU_OK;
}

static void destroy(void *p) {
  app_state_t *s = p;
  ru_heap_free(s->heap, s);
}

static void visible(void *p, bool v) {
  app_state_t *s = p;
  if (v) ru_dzapp_show_section(s->app, s->sec);
}

#define SEC_APP(fn, sec, id, name, icon) \
  static ru_err_t create_##fn(const ru_app_env_t *e, void **o) { return create_sec(e, o, sec); } \
  static const ru_app_t k_##fn = { id, name, icon, RU_MEM_LIGHT, create_##fn, destroy, NULL, visible, NULL };

SEC_APP(home, RU_SEC_HOME, RU_APP_ID_HOME, "Home", 1)
SEC_APP(devices, RU_SEC_DEVICES, RU_APP_ID_DEVICES, "Devices", 2)
SEC_APP(scenes, RU_SEC_SCENES, RU_APP_ID_SCENES, "Scenes", 3)
SEC_APP(energy, RU_SEC_ENERGY, RU_APP_ID_ENERGY, "Energy", 4)
SEC_APP(weather, RU_SEC_WEATHER, RU_APP_ID_WEATHER, "Weather", 5)
SEC_APP(settings, RU_SEC_SETTINGS, RU_APP_ID_SETTINGS, "Settings", 6)

static const ru_app_t *const k_table[RU_SEC_COUNT] = { &k_home, &k_devices, &k_scenes, &k_energy, &k_weather, &k_settings };

const ru_app_t *ru_apps_descriptor(ru_section_t s) { return s < RU_SEC_COUNT ? k_table[s] : NULL; }

bool ru_apps_available(ru_section_t s) { return g_shared && ru_dzapp_section_available(g_shared, s); }

ru_err_t ru_apps_register(ru_runtime_t *rt, ru_dzapp_t *shared) {
  g_shared = shared;
  for (int i = 0; i < RU_SEC_COUNT; i++) {
    ru_err_t e = ru_runtime_register(rt, k_table[i], i == RU_SEC_HOME);
    if (e != RU_OK) return e;
  }
  return RU_OK;
}

void ru_apps_unbind(void) { g_shared = NULL; }
