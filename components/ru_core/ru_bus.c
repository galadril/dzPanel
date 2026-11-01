#include "ru_bus.h"
#include <string.h>

ru_err_t ru_bus_init(ru_bus_t *b) {
  memset(b, 0, sizeof *b);
  b->mu = ru_mutex_create();
  return b->mu ? RU_OK : RU_ERR_NOMEM;
}

void ru_bus_deinit(ru_bus_t *b) {
  if (b->mu) ru_mutex_destroy(b->mu);
  b->mu = NULL;
}

void ru_bus_set_coalescing(ru_bus_t *b, uint16_t topic, bool on) {
  if (topic >= RU_BUS_MAX_TOPICS) return;
  ru_mutex_lock(b->mu);
  if (on) b->coalesce_mask |= 1u << topic; else b->coalesce_mask &= ~(1u << topic);
  ru_mutex_unlock(b->mu);
}

ru_err_t ru_bus_subscribe(ru_bus_t *b, ru_bus_sub_t *s, ru_event_t *storage, uint16_t cap,
                          uint32_t topic_mask, ru_bus_wake_fn wake, void *wake_ctx) {
  if (!s || !storage || cap == 0) return RU_ERR_INVALID;
  memset(s, 0, sizeof *s);
  s->buf = storage; s->cap = cap; s->topic_mask = topic_mask;
  s->wake = wake; s->wake_ctx = wake_ctx;
  s->mu = ru_mutex_create();
  if (!s->mu) return RU_ERR_NOMEM;
  ru_mutex_lock(b->mu);
  for (int i = 0; i < RU_BUS_MAX_SUBS; i++) {
    if (!b->subs[i]) { b->subs[i] = s; s->attached = true; ru_mutex_unlock(b->mu); return RU_OK; }
  }
  ru_mutex_unlock(b->mu);
  ru_mutex_destroy(s->mu); s->mu = NULL;
  return RU_ERR_FULL;
}

void ru_bus_unsubscribe(ru_bus_t *b, ru_bus_sub_t *s) {
  if (!s || !s->attached) return;
  ru_mutex_lock(b->mu);
  for (int i = 0; i < RU_BUS_MAX_SUBS; i++) if (b->subs[i] == s) b->subs[i] = NULL;
  s->attached = false;
  ru_mutex_unlock(b->mu);
  ru_mutex_destroy(s->mu); s->mu = NULL;
}

/* queue index helper: logical i -> physical */
static uint16_t phys(const ru_bus_sub_t *s, uint16_t i) { return (uint16_t)((s->head + i) % s->cap); }

static void push(ru_bus_sub_t *s, const ru_event_t *ev, bool coalesce) {
  ru_bus_stats_t *st = &s->st[ev->topic];
  st->accepted++;
  if (coalesce && ev->key != 0) {
    for (uint16_t i = 0; i < s->count; i++) {
      ru_event_t *q = &s->buf[phys(s, i)];
      if (q->topic == ev->topic && q->key == ev->key) { *q = *ev; st->coalesced++; return; }
    }
  }
  if (s->count == s->cap) { /* evict oldest */
    s->st[s->buf[s->head].topic].dropped++;
    s->head = (uint16_t)((s->head + 1) % s->cap);
    s->count--;
  }
  s->buf[phys(s, s->count)] = *ev;
  s->count++;
}

ru_err_t ru_bus_post(ru_bus_t *b, const ru_event_t *in) {
  if (in->topic >= RU_BUS_MAX_TOPICS || in->len > RU_BUS_PAYLOAD) {
    ru_mutex_lock(b->mu); b->bad_posts++; ru_mutex_unlock(b->mu);
    return RU_ERR_INVALID;
  }
  ru_event_t ev = *in;
  if (ev.ts_ms == 0) ev.ts_ms = ru_now_ms();
  ru_mutex_lock(b->mu);
  b->posted[ev.topic]++;
  bool co = (b->coalesce_mask >> ev.topic) & 1u;
  bool heard = false;
  for (int i = 0; i < RU_BUS_MAX_SUBS; i++) {
    ru_bus_sub_t *s = b->subs[i];
    if (!s || !((s->topic_mask >> ev.topic) & 1u)) continue;
    heard = true;
    ru_mutex_lock(s->mu);
    push(s, &ev, co);
    ru_mutex_unlock(s->mu);
    if (s->wake) s->wake(s->wake_ctx);
  }
  if (!heard) b->unheard[ev.topic]++;
  ru_mutex_unlock(b->mu);
  return RU_OK;
}

ru_err_t ru_bus_post_data(ru_bus_t *b, uint16_t topic, uint16_t type, uint32_t key,
                          const void *data, size_t len) {
  if (len > RU_BUS_PAYLOAD) return RU_ERR_INVALID;
  ru_event_t ev; memset(&ev, 0, sizeof ev);
  ev.topic = topic; ev.type = type; ev.key = key; ev.len = (uint8_t)len;
  if (len && data) memcpy(ev.payload, data, len);
  return ru_bus_post(b, &ev);
}

bool ru_bus_poll(ru_bus_sub_t *s, ru_event_t *out) {
  bool got = false;
  ru_mutex_lock(s->mu);
  if (s->count) {
    *out = s->buf[s->head];
    s->head = (uint16_t)((s->head + 1) % s->cap);
    s->count--;
    s->st[out->topic].delivered++;
    got = true;
  }
  ru_mutex_unlock(s->mu);
  return got;
}

bool ru_bus_wait(ru_bus_sub_t *s, ru_event_t *out, uint32_t timeout_ms) {
  uint32_t t0 = ru_now_ms();
  for (;;) {
    if (ru_bus_poll(s, out)) return true;
    if (timeout_ms != RU_WAIT_FOREVER && (uint32_t)(ru_now_ms() - t0) >= timeout_ms) return false;
    ru_sleep_ms(1);
  }
}

size_t ru_bus_dispatch(ru_bus_sub_t *s, ru_bus_handler_fn h, void *ctx, size_t max) {
  ru_event_t ev; size_t n = 0;
  while (n < max && ru_bus_poll(s, &ev)) { h(ctx, &ev); n++; }
  return n;
}

size_t ru_bus_pending(ru_bus_sub_t *s) {
  ru_mutex_lock(s->mu); size_t n = s->count; ru_mutex_unlock(s->mu); return n;
}

ru_bus_stats_t ru_bus_sub_stats(ru_bus_sub_t *s, uint16_t topic) {
  ru_bus_stats_t r; memset(&r, 0, sizeof r);
  if (topic >= RU_BUS_MAX_TOPICS) return r;
  ru_mutex_lock(s->mu); r = s->st[topic]; ru_mutex_unlock(s->mu);
  return r;
}

ru_bus_stats_t ru_bus_topic_stats(ru_bus_t *b, uint16_t topic) {
  ru_bus_stats_t r; memset(&r, 0, sizeof r);
  if (topic >= RU_BUS_MAX_TOPICS) return r;
  ru_mutex_lock(b->mu);
  r.unheard = b->unheard[topic];
  for (int i = 0; i < RU_BUS_MAX_SUBS; i++) {
    ru_bus_sub_t *s = b->subs[i];
    if (!s) continue;
    ru_mutex_lock(s->mu);
    r.accepted += s->st[topic].accepted; r.delivered += s->st[topic].delivered;
    r.coalesced += s->st[topic].coalesced; r.dropped += s->st[topic].dropped;
    ru_mutex_unlock(s->mu);
  }
  ru_mutex_unlock(b->mu);
  return r;
}
