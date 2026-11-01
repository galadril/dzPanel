/* ru_runtime: registry + lifecycle + low-memory levels.
 *
 * Rules
 *  - Exactly one foreground app. Leaving it suspends it (stays created, on_visible(false),
 *    no events) if light; heavy apps are destroyed on leave.
 *  - At most one heavy app is alive at any time.
 *  - At most max_suspended light apps stay suspended (least recently used destroyed first).
 *  - Memory levels come from heap used/cap: LOW sheds caches and destroys suspended apps,
 *    CRITICAL also closes the foreground app and returns home, FATAL calls the fatal hook
 *    (controlled restart). Evaluated after every switch and by ru_runtime_check_memory().
 *  - Create failure (NOMEM): shed everything and retry once, else stay on home.
 */
#pragma once
#include "ru_app.h"

#define RU_MAX_APPS 16
#define RU_MAX_CACHES 8

typedef struct {
  uint8_t low_pct, critical_pct, fatal_pct; /* of heap cap; 0 = defaults 70/85/95 */
  uint8_t max_suspended;                    /* default 3 */
} ru_runtime_cfg_t;

typedef void (*ru_cache_shed_fn)(void *ctx, ru_mem_level_t level);
typedef void (*ru_fatal_fn)(void *ctx);

typedef enum { RU_APP_UNLOADED = 0, RU_APP_FOREGROUND, RU_APP_SUSPENDED } ru_app_state_t;

typedef struct {
  const ru_app_t *app;
  void *state;
  ru_app_state_t st;
  uint32_t last_used; /* switch sequence number, for LRU */
} ru_slot_t;

struct ru_runtime {
  ru_heap_t *heap;
  ru_bus_t *bus;
  ru_runtime_cfg_t cfg;
  ru_slot_t slot[RU_MAX_APPS];
  int n_apps;
  int fg;    /* slot index or -1 */
  int home;  /* slot index or -1 */
  uint32_t seq;
  ru_mem_level_t level;
  struct { ru_cache_shed_fn fn; void *ctx; } cache[RU_MAX_CACHES];
  int n_caches;
  ru_fatal_fn fatal; void *fatal_ctx;
  ru_bus_sub_t sub; ru_event_t sub_buf[32]; bool sub_on;
  /* counters */
  uint32_t switches, sheds, evictions, create_fail, critical_closes;
};

void ru_runtime_init(ru_runtime_t *rt, ru_heap_t *heap, ru_bus_t *bus /*may be NULL*/, const ru_runtime_cfg_t *cfg);
void ru_runtime_shutdown(ru_runtime_t *rt); /* destroys all apps */

ru_err_t ru_runtime_register(ru_runtime_t *rt, const ru_app_t *app, bool is_home);
ru_err_t ru_runtime_register_cache(ru_runtime_t *rt, ru_cache_shed_fn fn, void *ctx);
void ru_runtime_set_fatal(ru_runtime_t *rt, ru_fatal_fn fn, void *ctx);

ru_err_t ru_runtime_switch_to(ru_runtime_t *rt, const char *app_id); /* RU_ERR_INVALID if unknown */
ru_err_t ru_runtime_go_home(ru_runtime_t *rt);
const ru_app_t *ru_runtime_foreground(const ru_runtime_t *rt);
ru_app_state_t ru_runtime_app_state(const ru_runtime_t *rt, const char *app_id);
int ru_runtime_alive_count(const ru_runtime_t *rt, int *heavy_out);

ru_mem_level_t ru_runtime_mem_level(const ru_runtime_t *rt);
ru_mem_level_t ru_runtime_check_memory(ru_runtime_t *rt); /* evaluates, acts, returns level */

/* Delivers queued bus events (topic_mask) to the foreground app. Returns events handled. */
void ru_runtime_subscribe(ru_runtime_t *rt, uint32_t topic_mask);
size_t ru_runtime_pump(ru_runtime_t *rt, size_t max);
