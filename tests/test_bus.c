#include "mini_test.h"
#include "ru_bus.h"
#include <string.h>
#include <stdatomic.h>

enum { T_NET = 1, T_MODEL = 2, T_INPUT = 3 };

static void basic(void) {
  ru_bus_t b; CHECK(ru_bus_init(&b) == RU_OK);
  ru_bus_set_coalescing(&b, T_MODEL, true);
  ru_event_t st[4]; ru_bus_sub_t s;
  CHECK(ru_bus_subscribe(&b, &s, st, 4, 1u << T_MODEL | 1u << T_NET, NULL, NULL) == RU_OK);
  ru_event_t e;
  CHECK(!ru_bus_poll(&s, &e));
  int v = 1;
  ru_bus_post_data(&b, T_MODEL, 1, 42, &v, sizeof v);
  v = 2; ru_bus_post_data(&b, T_MODEL, 1, 42, &v, sizeof v); /* coalesced */
  v = 3; ru_bus_post_data(&b, T_MODEL, 1, 43, &v, sizeof v);
  CHECK(ru_bus_pending(&s) == 2);
  CHECK(ru_bus_poll(&s, &e)); CHECK(e.key == 42 && e.len == 4); memcpy(&v, e.payload, 4); CHECK(v == 2);
  CHECK(ru_bus_poll(&s, &e)); CHECK(e.key == 43);
  /* key 0 never coalesces; non-coalescing topic never coalesces */
  ru_bus_post_data(&b, T_MODEL, 1, 0, NULL, 0); ru_bus_post_data(&b, T_MODEL, 1, 0, NULL, 0);
  ru_bus_post_data(&b, T_NET, 1, 7, NULL, 0); ru_bus_post_data(&b, T_NET, 1, 7, NULL, 0);
  CHECK(ru_bus_pending(&s) == 4);
  /* overflow evicts oldest, counts drop on that topic */
  ru_bus_post_data(&b, T_NET, 9, 8, NULL, 0);
  CHECK(ru_bus_pending(&s) == 4);
  CHECK(ru_bus_sub_stats(&s, T_MODEL).dropped == 1);
  CHECK(ru_bus_sub_stats(&s, T_NET).dropped == 0);
  /* newest survives */
  size_t n = 0; ru_event_t last = {0};
  while (ru_bus_poll(&s, &e)) { last = e; n++; }
  CHECK(n == 4 && last.type == 9);
  /* unheard topic and invalid */
  ru_bus_post_data(&b, T_INPUT, 0, 0, NULL, 0);
  CHECK(ru_bus_topic_stats(&b, T_INPUT).unheard == 1);
  CHECK(ru_bus_post_data(&b, 99, 0, 0, NULL, 0) == RU_ERR_INVALID);
  CHECK(ru_bus_post_data(&b, T_NET, 0, 0, NULL, 99) == RU_ERR_INVALID);
  ru_bus_stats_t ts = ru_bus_topic_stats(&b, T_MODEL);
  CHECK(ts.accepted == ts.delivered + ts.coalesced + ts.dropped);
  ru_bus_unsubscribe(&b, &s);
  ru_bus_post_data(&b, T_MODEL, 1, 1, NULL, 0); /* no subscribers, no crash */
  ru_bus_deinit(&b);
}

static int wakes;
static void wake(void *c) { (void)c; wakes++; }
static int seen;
static void h(void *c, const ru_event_t *e) { (void)c; (void)e; seen++; }
static void dispatch_wake(void) {
  ru_bus_t b; ru_bus_init(&b);
  ru_event_t st[8]; ru_bus_sub_t s;
  ru_bus_subscribe(&b, &s, st, 8, 1u << T_NET, wake, NULL);
  for (int i = 0; i < 5; i++) ru_bus_post_data(&b, T_NET, 0, 0, NULL, 0);
  CHECK(wakes == 5);
  CHECK(ru_bus_dispatch(&s, h, NULL, 3) == 3); CHECK(seen == 3);
  CHECK(ru_bus_dispatch(&s, h, NULL, 99) == 2);
  ru_event_t e; CHECK(!ru_bus_wait(&s, &e, 5));
  ru_bus_unsubscribe(&b, &s); ru_bus_deinit(&b);
}

/* ---- multi-thread stress ---- */
#define NPROD 4
#define NKEY 8
#define NPOST 20000
static ru_bus_t SB;
static atomic_int stop;
typedef struct { int id; } parg_t;

static void producer(void *a) {
  int id = ((parg_t *)a)->id;
  for (uint32_t i = 1; i <= NPOST; i++) {
    uint32_t key = (uint32_t)(id * NKEY + (int)(i % NKEY) + 1);
    uint32_t seq = i;
    ru_bus_post_data(&SB, T_MODEL, 0, key, &seq, sizeof seq);
    ru_event_t e; memset(&e, 0, sizeof e); e.topic = T_NET; e.key = 0; e.len = 4; memcpy(e.payload, &seq, 4);
    ru_bus_post(&SB, &e);            /* non-coalescing, small queue: must drop, never block */
  }
}
typedef struct { ru_bus_sub_t *s; uint32_t last[NPROD * NKEY + 2]; int order_ok; long got; uint32_t last_net[NPROD]; } carg_t;
static void consumer(void *a) {
  carg_t *c = a; ru_event_t e;
  for (;;) {
    if (ru_bus_poll(c->s, &e)) {
      c->got++;
      if (e.topic == T_MODEL) { uint32_t seq; memcpy(&seq, e.payload, 4); if (seq < c->last[e.key]) c->order_ok = 0; c->last[e.key] = seq; }
    } else if (atomic_load(&stop)) { if (!ru_bus_poll(c->s, &e)) break; else { c->got++; } }
    else ru_sleep_ms(0);
  }
}

static void stress(void) {
  ru_bus_init(&SB); ru_bus_set_coalescing(&SB, T_MODEL, true);
  static ru_event_t sa[256], sb2[8];
  ru_bus_sub_t A, B;
  ru_bus_subscribe(&SB, &A, sa, 256, 1u << T_MODEL, NULL, NULL);
  ru_bus_subscribe(&SB, &B, sb2, 8, 1u << T_MODEL | 1u << T_NET, NULL, NULL); /* small: drops */
  static carg_t ca, cb; ca.s = &A; cb.s = &B; ca.order_ok = cb.order_ok = 1;
  ru_task_t *ct[2] = { ru_task_create("ca", consumer, &ca), ru_task_create("cb", consumer, &cb) };
  parg_t pa[NPROD]; ru_task_t *pt[NPROD];
  for (int i = 0; i < NPROD; i++) { pa[i].id = i; pt[i] = ru_task_create("p", producer, &pa[i]); }
  for (int i = 0; i < NPROD; i++) ru_task_join(pt[i]);
  atomic_store(&stop, 1);
  for (int i = 0; i < 2; i++) ru_task_join(ct[i]);
  CHECK(ca.order_ok && cb.order_ok);
  /* the latest value per key is never lost on the big queue (keys fit) */
  for (int id = 0; id < NPROD; id++)
    for (int k = 0; k < NKEY; k++) {
      uint32_t key = (uint32_t)(id * NKEY + k + 1);
      uint32_t expect = 0; for (uint32_t i = 1; i <= NPOST; i++) if ((int)(i % NKEY) == k) expect = i;
      CHECK(ca.last[key] == expect);
    }
  for (int t = T_NET; t <= T_MODEL; t++) {
    ru_bus_stats_t sa_ = ru_bus_sub_stats(&A, (uint16_t)t), sb_ = ru_bus_sub_stats(&B, (uint16_t)t);
    if (t == T_MODEL) CHECK(sa_.accepted == (uint32_t)NPROD * NPOST);
    if (t == T_NET) CHECK(sa_.accepted == 0);
    CHECK(sb_.accepted == (uint32_t)NPROD * NPOST);
    CHECK(sa_.accepted == sa_.delivered + sa_.coalesced + sa_.dropped);
    CHECK(sb_.accepted == sb_.delivered + sb_.coalesced + sb_.dropped);
  }
  CHECK(ru_bus_sub_stats(&B, T_NET).dropped > 0);
  CHECK(ru_bus_sub_stats(&A, T_MODEL).dropped == 0);
  CHECK(ru_bus_sub_stats(&A, T_MODEL).coalesced > 0);
  printf("stress: B net drops %u, A model coalesced %u\n",
         ru_bus_sub_stats(&B, T_NET).dropped, ru_bus_sub_stats(&A, T_MODEL).coalesced);
  ru_bus_unsubscribe(&SB, &A); ru_bus_unsubscribe(&SB, &B); ru_bus_deinit(&SB);
}

int main(void) { basic(); dispatch_wake(); stress(); MT_DONE(); }
