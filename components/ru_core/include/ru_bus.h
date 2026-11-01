/* ru_bus: in-process event bus. Pure C11, built on ru_osal.
 *
 * - Events are small and fixed: topic, type, coalescing key, 16 bytes of payload.
 * - Any thread may post; each subscriber owns a bounded queue and drains it on its
 *   own loop (ru_bus_poll / ru_bus_dispatch / ru_bus_wait). Handlers never run on the poster.
 * - Coalescing: on topics marked coalescing, an event with key != 0 replaces a still-queued
 *   event with the same (topic,key) in that subscriber's queue (latest value wins).
 * - Overflow: the OLDEST queued event is evicted and counted as a drop on its topic, so a
 *   stalled consumer loses old updates, not new ones.
 * - No allocation after init: the subscriber supplies its queue storage.
 * Lock order: bus lock, then subscriber lock. Never call bus APIs from a wake callback.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ru_osal.h"

#define RU_BUS_MAX_TOPICS 32
#define RU_BUS_MAX_SUBS 16
#define RU_BUS_PAYLOAD 16

typedef struct {
  uint16_t topic;  /* < RU_BUS_MAX_TOPICS */
  uint16_t type;   /* topic-specific event type */
  uint32_t key;    /* coalescing key (e.g. device idx); 0 = never coalesce */
  uint32_t ts_ms;  /* filled by post if 0 */
  uint8_t len;     /* valid payload bytes, <= RU_BUS_PAYLOAD */
  uint8_t payload[RU_BUS_PAYLOAD];
} ru_event_t;

typedef struct {
  uint32_t accepted;  /* events that matched this subscription */
  uint32_t delivered; /* handed to the subscriber */
  uint32_t coalesced; /* merged into an already queued event */
  uint32_t dropped;   /* evicted because the queue was full */
  uint32_t unheard;   /* (bus level only) posted with no subscriber */
} ru_bus_stats_t;
/* invariant per subscriber/topic: accepted == delivered + coalesced + dropped + pending */

typedef struct ru_bus_sub ru_bus_sub_t;
typedef void (*ru_bus_wake_fn)(void *ctx); /* called on the poster's thread; keep tiny */
typedef void (*ru_bus_handler_fn)(void *ctx, const ru_event_t *ev);

struct ru_bus_sub {
  ru_event_t *buf; /* caller storage, cap entries */
  uint16_t cap, head, count;
  uint32_t topic_mask;
  ru_mutex_t *mu;
  ru_bus_wake_fn wake;
  void *wake_ctx;
  ru_bus_stats_t st[RU_BUS_MAX_TOPICS];
  bool attached;
};

typedef struct {
  ru_mutex_t *mu;
  ru_bus_sub_t *subs[RU_BUS_MAX_SUBS];
  uint32_t coalesce_mask;
  uint32_t unheard[RU_BUS_MAX_TOPICS];
  uint32_t posted[RU_BUS_MAX_TOPICS];
  uint32_t bad_posts;
} ru_bus_t;

ru_err_t ru_bus_init(ru_bus_t *b);
void ru_bus_deinit(ru_bus_t *b); /* all subs must be unsubscribed */
void ru_bus_set_coalescing(ru_bus_t *b, uint16_t topic, bool on);

/* storage: caller array of cap events. topic_mask: bit n = topic n. */
ru_err_t ru_bus_subscribe(ru_bus_t *b, ru_bus_sub_t *s, ru_event_t *storage, uint16_t cap,
                          uint32_t topic_mask, ru_bus_wake_fn wake, void *wake_ctx);
void ru_bus_unsubscribe(ru_bus_t *b, ru_bus_sub_t *s); /* safe vs concurrent post */

/* Copies ev. Returns RU_OK, or RU_ERR_INVALID (bad topic/len). Never blocks on a full queue. */
ru_err_t ru_bus_post(ru_bus_t *b, const ru_event_t *ev);
ru_err_t ru_bus_post_data(ru_bus_t *b, uint16_t topic, uint16_t type, uint32_t key,
                          const void *data, size_t len);

bool ru_bus_poll(ru_bus_sub_t *s, ru_event_t *out); /* non-blocking pop */
bool ru_bus_wait(ru_bus_sub_t *s, ru_event_t *out, uint32_t timeout_ms); /* polls at 1 ms */
size_t ru_bus_dispatch(ru_bus_sub_t *s, ru_bus_handler_fn h, void *ctx, size_t max);
size_t ru_bus_pending(ru_bus_sub_t *s);

ru_bus_stats_t ru_bus_sub_stats(ru_bus_sub_t *s, uint16_t topic);
ru_bus_stats_t ru_bus_topic_stats(ru_bus_t *b, uint16_t topic); /* summed over subscribers */
