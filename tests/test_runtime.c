#include "mini_test.h"
#include "ru_runtime.h"
#include <string.h>

typedef struct { ru_heap_t *h; void *blob; void *cache; size_t cache_n; } st_t;
static size_t blob_for(const char *id) {
  if (!strcmp(id, "home")) return 10000;
  if (!strcmp(id, "hog")) return 80000;
  if (id[0] == 'h') return 120000; /* h1, h2: heavy */
  return 20000;
}
static int destroyed, created, vis_true, vis_false, lowmem_calls;
static ru_err_t mk(const ru_app_env_t *env, void **out, const char *id) {
  st_t *s = ru_heap_alloc(env->heap, sizeof *s);
  if (!s) return RU_ERR_NOMEM;
  s->h = env->heap; s->cache = NULL; s->cache_n = 0;
  s->blob = ru_heap_alloc(env->heap, blob_for(id));
  if (!s->blob) { ru_heap_free(env->heap, s); return RU_ERR_NOMEM; }
  created++; *out = s; return RU_OK;
}
static void dtor(void *p) {
  st_t *s = p; ru_heap_free(s->h, s->cache); ru_heap_free(s->h, s->blob); ru_heap_free(s->h, s); destroyed++;
}
static void vis(void *p, bool v) {
  st_t *s = p;
  if (v) { vis_true++; if (s->cache_n < 5) { void *c = ru_heap_alloc(s->h, 8000); if (c) { /* grow cache */
      void *nc = ru_heap_alloc(s->h, 8000 * (s->cache_n + 1)); if (nc) { ru_heap_free(s->h, s->cache); s->cache = nc; s->cache_n++; } ru_heap_free(s->h, c); } } }
  else vis_false++;
}
static void low(void *p, ru_mem_level_t l) { st_t *s = p; (void)l; lowmem_calls++; ru_heap_free(s->h, s->cache); s->cache = NULL; s->cache_n = 0; }
#define APP(fn, idstr, cls) \
  static ru_err_t c_##fn(const ru_app_env_t *e, void **o) { return mk(e, o, idstr); } \
  static const ru_app_t A_##fn = { idstr, idstr, 1, cls, c_##fn, dtor, NULL, vis, low };
APP(home, "home", RU_MEM_LIGHT) APP(l1, "l1", RU_MEM_LIGHT) APP(l2, "l2", RU_MEM_LIGHT)
APP(l3, "l3", RU_MEM_LIGHT) APP(l4, "l4", RU_MEM_LIGHT) APP(h1, "h1", RU_MEM_HEAVY) APP(h2, "h2", RU_MEM_HEAVY)
APP(hog, "hog", RU_MEM_LIGHT)

static int cache_sheds;
static void cshed(void *c, ru_mem_level_t l) { (void)c; (void)l; cache_sheds++; }
static int fatals;
static void fatal(void *c) { (void)c; fatals++; }

static void lifecycle(void) {
  ru_heap_t heap; ru_heap_init(&heap, 600000);
  ru_runtime_t rt; ru_runtime_init(&rt, &heap, NULL, NULL);
  ru_runtime_register(&rt, &A_home, true); ru_runtime_register(&rt, &A_l1, false);
  ru_runtime_register(&rt, &A_h1, false); ru_runtime_register(&rt, &A_h2, false);
  CHECK(ru_runtime_register(&rt, &A_l1, false) == RU_ERR_INVALID); /* duplicate id */
  CHECK(ru_runtime_switch_to(&rt, "nope") == RU_ERR_INVALID);
  CHECK(ru_runtime_go_home(&rt) == RU_OK);
  CHECK(!strcmp(ru_runtime_foreground(&rt)->id, "home"));
  CHECK(ru_runtime_switch_to(&rt, "l1") == RU_OK);
  CHECK(ru_runtime_app_state(&rt, "home") == RU_APP_SUSPENDED);
  CHECK(ru_runtime_switch_to(&rt, "h1") == RU_OK);
  CHECK(ru_runtime_switch_to(&rt, "h2") == RU_OK);
  int hv; ru_runtime_alive_count(&rt, &hv); CHECK(hv == 1);
  CHECK(ru_runtime_app_state(&rt, "h1") == RU_APP_UNLOADED);
  CHECK(ru_runtime_switch_to(&rt, "home") == RU_OK);
  CHECK(ru_runtime_app_state(&rt, "h2") == RU_APP_UNLOADED);
  ru_runtime_shutdown(&rt);
  CHECK(heap.used == 0 && heap.live == 0);
}

static void loop1000(void) {
  ru_heap_t heap; ru_heap_init(&heap, 300000);
  ru_runtime_t rt; ru_runtime_init(&rt, &heap, NULL, NULL);
  ru_runtime_set_fatal(&rt, fatal, NULL);
  ru_runtime_register_cache(&rt, cshed, NULL);
  const ru_app_t *all[] = { &A_home, &A_l1, &A_l2, &A_l3, &A_l4, &A_h1, &A_h2 };
  for (int i = 0; i < 7; i++) ru_runtime_register(&rt, all[i], i == 0);
  ru_runtime_go_home(&rt);
  unsigned x = 12345; int bad = 0;
  for (int i = 0; i < 1000; i++) {
    x = x * 1103515245u + 12345u;
    const ru_app_t *t = all[(x >> 16) % 7];
    if (ru_runtime_switch_to(&rt, t->id) != RU_OK) bad++;
    int hv; ru_runtime_alive_count(&rt, &hv);
    if (hv > 1) bad++;
    if (heap.used > heap.cap) bad++;
    if (!ru_runtime_foreground(&rt)) bad++;
  }
  CHECK(bad == 0);
  CHECK(rt.sheds > 0 && cache_sheds > 0 && lowmem_calls > 0); /* pressure really happened */
  CHECK(fatals == 0);
  CHECK(heap.peak <= heap.cap);
  printf("loop: switches=%u sheds=%u evictions=%u peak=%zu/%zu create_fail=%u\n", rt.switches, rt.sheds,
         rt.evictions, heap.peak, heap.cap, rt.create_fail);
  ru_runtime_shutdown(&rt);
  CHECK(heap.used == 0 && heap.live == 0 && heap.allocs == heap.frees);
  CHECK(created == destroyed);
}

static int ev_seen;
static void on_ev(void *p, const ru_event_t *e) { (void)p; if (e->key == 5) ev_seen++; }
static const ru_app_t A_ev = { "ev", "ev", 0, RU_MEM_LIGHT, c_home, dtor, on_ev, NULL, NULL };

static void critical(void) {
  ru_heap_t heap; ru_heap_init(&heap, 92000);
  ru_runtime_t rt; ru_runtime_init(&rt, &heap, NULL, NULL);
  ru_runtime_set_fatal(&rt, fatal, NULL);
  ru_runtime_register(&rt, &A_home, true); ru_runtime_register(&rt, &A_hog, false);
  ru_runtime_go_home(&rt);
  CHECK(ru_runtime_switch_to(&rt, "hog") == RU_OK); /* 90% -> critical -> back home, hog released */
  CHECK(rt.critical_closes == 1);
  CHECK(!strcmp(ru_runtime_foreground(&rt)->id, "home"));
  CHECK(ru_runtime_app_state(&rt, "hog") == RU_APP_UNLOADED);
  CHECK(ru_runtime_mem_level(&rt) == RU_LEVEL_NORMAL);
  CHECK(fatals == 0);
  ru_runtime_shutdown(&rt);
  CHECK(heap.used == 0);
  /* cannot fit at all: create fails cleanly, stays on home */
  ru_heap_init(&heap, 75000);
  ru_runtime_init(&rt, &heap, NULL, NULL);
  ru_runtime_register(&rt, &A_home, true); ru_runtime_register(&rt, &A_hog, false);
  ru_runtime_go_home(&rt);
  CHECK(ru_runtime_switch_to(&rt, "hog") == RU_ERR_NOMEM);
  CHECK(!strcmp(ru_runtime_foreground(&rt)->id, "home"));
  ru_runtime_shutdown(&rt);
  CHECK(heap.used == 0 && heap.live == 0);
}

static void events(void) {
  ru_heap_t heap; ru_heap_init(&heap, 0);
  ru_bus_t bus; ru_bus_init(&bus);
  ru_runtime_t rt; ru_runtime_init(&rt, &heap, &bus, NULL);
  ru_runtime_register(&rt, &A_home, true); ru_runtime_register(&rt, &A_ev, false);
  ru_runtime_subscribe(&rt, 1u << 1);
  ru_runtime_go_home(&rt);
  ru_bus_post_data(&bus, 1, 0, 5, NULL, 0);
  CHECK(ru_runtime_pump(&rt, 10) == 1); CHECK(ev_seen == 0); /* home has no on_event */
  ru_runtime_switch_to(&rt, "ev");
  ru_bus_post_data(&bus, 1, 0, 5, NULL, 0);
  CHECK(ru_runtime_pump(&rt, 10) == 1); CHECK(ev_seen == 1);
  ru_runtime_switch_to(&rt, "home"); /* ev suspended: no events */
  ru_bus_post_data(&bus, 1, 0, 5, NULL, 0); ru_runtime_pump(&rt, 10); CHECK(ev_seen == 1);
  ru_runtime_shutdown(&rt); ru_bus_deinit(&bus);
  CHECK(heap.used == 0);
}

int main(void) { lifecycle(); loop1000(); critical(); events(); MT_DONE(); }
