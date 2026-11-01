#include "ru_runtime.h"
#include <string.h>

static void destroy_slot(ru_runtime_t *rt, int i) {
  ru_slot_t *s = &rt->slot[i];
  if (s->st == RU_APP_UNLOADED) return;
  if (s->st == RU_APP_FOREGROUND && s->app->on_visible) s->app->on_visible(s->state, false);
  s->app->destroy(s->state);
  s->state = NULL; s->st = RU_APP_UNLOADED;
  if (rt->fg == i) rt->fg = -1;
}

void ru_runtime_init(ru_runtime_t *rt, ru_heap_t *heap, ru_bus_t *bus, const ru_runtime_cfg_t *cfg) {
  memset(rt, 0, sizeof *rt);
  rt->heap = heap; rt->bus = bus; rt->fg = -1; rt->home = -1;
  if (cfg) rt->cfg = *cfg;
  if (!rt->cfg.low_pct) rt->cfg.low_pct = 70;
  if (!rt->cfg.critical_pct) rt->cfg.critical_pct = 85;
  if (!rt->cfg.fatal_pct) rt->cfg.fatal_pct = 95;
  if (!rt->cfg.max_suspended) rt->cfg.max_suspended = 3;
}

void ru_runtime_shutdown(ru_runtime_t *rt) {
  for (int i = 0; i < rt->n_apps; i++) destroy_slot(rt, i);
  rt->fg = -1;
  if (rt->sub_on) { ru_bus_unsubscribe(rt->bus, &rt->sub); rt->sub_on = false; }
}

ru_err_t ru_runtime_register(ru_runtime_t *rt, const ru_app_t *app, bool is_home) {
  if (!app || !app->id || !app->create || !app->destroy) return RU_ERR_INVALID;
  if (rt->n_apps >= RU_MAX_APPS) return RU_ERR_FULL;
  for (int i = 0; i < rt->n_apps; i++) if (!strcmp(rt->slot[i].app->id, app->id)) return RU_ERR_INVALID;
  rt->slot[rt->n_apps].app = app;
  if (is_home) rt->home = rt->n_apps;
  rt->n_apps++;
  return RU_OK;
}

ru_err_t ru_runtime_register_cache(ru_runtime_t *rt, ru_cache_shed_fn fn, void *ctx) {
  if (!fn) return RU_ERR_INVALID;
  if (rt->n_caches >= RU_MAX_CACHES) return RU_ERR_FULL;
  rt->cache[rt->n_caches].fn = fn; rt->cache[rt->n_caches].ctx = ctx; rt->n_caches++;
  return RU_OK;
}

void ru_runtime_set_fatal(ru_runtime_t *rt, ru_fatal_fn fn, void *ctx) { rt->fatal = fn; rt->fatal_ctx = ctx; }

static int find(const ru_runtime_t *rt, const char *id) {
  for (int i = 0; i < rt->n_apps; i++) if (!strcmp(rt->slot[i].app->id, id)) return i;
  return -1;
}

static ru_mem_level_t level_of(const ru_runtime_t *rt) {
  if (!rt->heap || !rt->heap->cap) return RU_LEVEL_NORMAL;
  size_t pct = rt->heap->used * 100 / rt->heap->cap;
  if (pct >= rt->cfg.fatal_pct) return RU_LEVEL_FATAL;
  if (pct >= rt->cfg.critical_pct) return RU_LEVEL_CRITICAL;
  if (pct >= rt->cfg.low_pct) return RU_LEVEL_LOW;
  return RU_LEVEL_NORMAL;
}

ru_mem_level_t ru_runtime_mem_level(const ru_runtime_t *rt) { return rt->level; }

/* Flush caches and release suspended apps. */
static void shed(ru_runtime_t *rt, ru_mem_level_t lvl) {
  rt->sheds++;
  for (int i = 0; i < rt->n_caches; i++) rt->cache[i].fn(rt->cache[i].ctx, lvl);
  for (int i = 0; i < rt->n_apps; i++) {
    ru_slot_t *s = &rt->slot[i];
    if (s->st == RU_APP_SUSPENDED) { destroy_slot(rt, i); rt->evictions++; }
    else if (s->st == RU_APP_FOREGROUND && s->app->on_low_memory) s->app->on_low_memory(s->state, lvl);
  }
}

static ru_err_t create_slot(ru_runtime_t *rt, int i) {
  ru_app_env_t env = { rt, rt->heap, rt->bus };
  ru_slot_t *s = &rt->slot[i];
  void *st = NULL;
  ru_err_t e = s->app->create(&env, &st);
  if (e == RU_ERR_NOMEM) { /* make room and retry once */
    shed(rt, RU_LEVEL_CRITICAL);
    st = NULL; e = s->app->create(&env, &st);
  }
  if (e != RU_OK) { rt->create_fail++; return e; }
  s->state = st; s->st = RU_APP_SUSPENDED;
  return RU_OK;
}

static void trim_suspended(ru_runtime_t *rt) {
  for (;;) {
    int n = 0, lru = -1;
    for (int i = 0; i < rt->n_apps; i++) {
      if (rt->slot[i].st != RU_APP_SUSPENDED) continue;
      n++;
      if (lru < 0 || rt->slot[i].last_used < rt->slot[lru].last_used) lru = i;
    }
    if (n <= rt->cfg.max_suspended) return;
    destroy_slot(rt, lru); rt->evictions++;
  }
}

static ru_err_t switch_idx(ru_runtime_t *rt, int idx, bool allow_home_fallback);

ru_mem_level_t ru_runtime_check_memory(ru_runtime_t *rt) {
  ru_mem_level_t lvl = level_of(rt);
  if (lvl >= RU_LEVEL_LOW) {
    shed(rt, lvl);
    lvl = level_of(rt);
  }
  if (lvl >= RU_LEVEL_CRITICAL && rt->fg >= 0 && rt->fg != rt->home) {
    rt->critical_closes++;
    int old = rt->fg;
    switch_idx(rt, rt->home, false);
    if (rt->fg != old && rt->slot[old].st == RU_APP_SUSPENDED) { destroy_slot(rt, old); rt->evictions++; }
    lvl = level_of(rt);
  }
  if (lvl >= RU_LEVEL_FATAL && rt->fatal) rt->fatal(rt->fatal_ctx);
  rt->level = lvl;
  return lvl;
}

/* leave current foreground: heavy apps are destroyed, light ones suspended */
static void leave_fg(ru_runtime_t *rt) {
  if (rt->fg < 0) return;
  ru_slot_t *s = &rt->slot[rt->fg];
  if (s->app->on_visible) s->app->on_visible(s->state, false);
  if (s->app->mem_class == RU_MEM_HEAVY) { s->st = RU_APP_SUSPENDED; destroy_slot(rt, rt->fg); }
  else s->st = RU_APP_SUSPENDED;
  rt->fg = -1;
}

static ru_err_t switch_idx(ru_runtime_t *rt, int idx, bool allow_home_fallback) {
  if (idx < 0) return RU_ERR_INVALID;
  if (rt->fg == idx) return RU_OK;
  ru_slot_t *t = &rt->slot[idx];
  leave_fg(rt);
  if (t->app->mem_class == RU_MEM_HEAVY)
    for (int i = 0; i < rt->n_apps; i++)
      if (i != idx && rt->slot[i].app->mem_class == RU_MEM_HEAVY) destroy_slot(rt, i);
  if (t->st == RU_APP_UNLOADED) {
    ru_err_t e = create_slot(rt, idx);
    if (e != RU_OK) {
      if (allow_home_fallback && rt->home >= 0 && rt->home != idx) switch_idx(rt, rt->home, false);
      else if (rt->fatal) rt->fatal(rt->fatal_ctx);
      return e;
    }
  }
  t->st = RU_APP_FOREGROUND; rt->fg = idx; t->last_used = ++rt->seq; rt->switches++;
  if (t->app->on_visible) t->app->on_visible(t->state, true);
  trim_suspended(rt);
  if (allow_home_fallback) ru_runtime_check_memory(rt);
  return RU_OK;
}

ru_err_t ru_runtime_switch_to(ru_runtime_t *rt, const char *app_id) {
  return switch_idx(rt, app_id ? find(rt, app_id) : -1, true);
}

ru_err_t ru_runtime_go_home(ru_runtime_t *rt) { return switch_idx(rt, rt->home, true); }

const ru_app_t *ru_runtime_foreground(const ru_runtime_t *rt) { return rt->fg >= 0 ? rt->slot[rt->fg].app : NULL; }

ru_app_state_t ru_runtime_app_state(const ru_runtime_t *rt, const char *app_id) {
  int i = find(rt, app_id);
  return i < 0 ? RU_APP_UNLOADED : rt->slot[i].st;
}

int ru_runtime_alive_count(const ru_runtime_t *rt, int *heavy_out) {
  int n = 0, h = 0;
  for (int i = 0; i < rt->n_apps; i++)
    if (rt->slot[i].st != RU_APP_UNLOADED) { n++; if (rt->slot[i].app->mem_class == RU_MEM_HEAVY) h++; }
  if (heavy_out) *heavy_out = h;
  return n;
}

void ru_runtime_subscribe(ru_runtime_t *rt, uint32_t mask) {
  if (!rt->bus || rt->sub_on) return;
  if (ru_bus_subscribe(rt->bus, &rt->sub, rt->sub_buf, 32, mask, NULL, NULL) == RU_OK) rt->sub_on = true;
}

size_t ru_runtime_pump(ru_runtime_t *rt, size_t max) {
  if (!rt->sub_on) return 0;
  ru_event_t ev; size_t n = 0;
  while (n < max && ru_bus_poll(&rt->sub, &ev)) {
    n++;
    if (rt->fg >= 0) {
      const ru_slot_t *s = &rt->slot[rt->fg];
      if (s->app->on_event) s->app->on_event(s->state, &ev);
    }
  }
  return n;
}
