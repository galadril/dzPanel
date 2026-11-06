#define _POSIX_C_SOURCE 200809L
#include "ru_osal.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t ru_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

uint32_t ru_now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000000u + ts.tv_nsec / 1000u);
}

void ru_sleep_ms(uint32_t ms) {
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

struct ru_mutex { pthread_mutex_t m; };
ru_mutex_t *ru_mutex_create(void) {
  ru_mutex_t *m = malloc(sizeof *m);
  if (m) pthread_mutex_init(&m->m, NULL);
  return m;
}
void ru_mutex_lock(ru_mutex_t *m) { pthread_mutex_lock(&m->m); }
void ru_mutex_unlock(ru_mutex_t *m) { pthread_mutex_unlock(&m->m); }
void ru_mutex_destroy(ru_mutex_t *m) { if (m) { pthread_mutex_destroy(&m->m); free(m); } }

struct ru_queue {
  pthread_mutex_t m;
  pthread_cond_t not_empty, not_full;
  uint8_t *buf;
  size_t item, cap, head, count;
};

static void abs_deadline(struct timespec *ts, uint32_t ms) {
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_sec += ms / 1000;
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* Caller holds q->m. Waits until pred() would be false-to-true or timeout. */
static int wait_on(pthread_cond_t *c, pthread_mutex_t *m, uint32_t timeout_ms) {
  if (timeout_ms == RU_WAIT_FOREVER) return pthread_cond_wait(c, m);
  struct timespec dl;
  abs_deadline(&dl, timeout_ms);
  return pthread_cond_timedwait(c, m, &dl);
}

ru_queue_t *ru_queue_create(size_t item_size, size_t capacity) {
  if (!item_size || !capacity) return NULL;
  ru_queue_t *q = calloc(1, sizeof *q);
  if (!q) return NULL;
  q->buf = malloc(item_size * capacity);
  if (!q->buf) { free(q); return NULL; }
  q->item = item_size; q->cap = capacity;
  pthread_mutex_init(&q->m, NULL);
  pthread_cond_init(&q->not_empty, NULL);
  pthread_cond_init(&q->not_full, NULL);
  return q;
}

ru_err_t ru_queue_send(ru_queue_t *q, const void *item, uint32_t timeout_ms) {
  if (!q || !item) return RU_ERR_INVALID;
  pthread_mutex_lock(&q->m);
  while (q->count == q->cap) {
    if (timeout_ms == 0 || wait_on(&q->not_full, &q->m, timeout_ms) == ETIMEDOUT) {
      if (q->count == q->cap) { pthread_mutex_unlock(&q->m); return timeout_ms ? RU_ERR_TIMEOUT : RU_ERR_FULL; }
    }
  }
  memcpy(q->buf + ((q->head + q->count) % q->cap) * q->item, item, q->item);
  q->count++;
  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->m);
  return RU_OK;
}

ru_err_t ru_queue_recv(ru_queue_t *q, void *item, uint32_t timeout_ms) {
  if (!q || !item) return RU_ERR_INVALID;
  pthread_mutex_lock(&q->m);
  while (q->count == 0) {
    if (timeout_ms == 0 || wait_on(&q->not_empty, &q->m, timeout_ms) == ETIMEDOUT) {
      if (q->count == 0) { pthread_mutex_unlock(&q->m); return RU_ERR_TIMEOUT; }
    }
  }
  memcpy(item, q->buf + q->head * q->item, q->item);
  q->head = (q->head + 1) % q->cap;
  q->count--;
  pthread_cond_signal(&q->not_full);
  pthread_mutex_unlock(&q->m);
  return RU_OK;
}

size_t ru_queue_count(ru_queue_t *q) {
  pthread_mutex_lock(&q->m);
  size_t n = q->count;
  pthread_mutex_unlock(&q->m);
  return n;
}

void ru_queue_destroy(ru_queue_t *q) {
  if (!q) return;
  pthread_mutex_destroy(&q->m);
  pthread_cond_destroy(&q->not_empty);
  pthread_cond_destroy(&q->not_full);
  free(q->buf);
  free(q);
}

struct ru_task { pthread_t th; ru_task_fn fn; void *arg; char name[16]; };
static void *task_tramp(void *p) {
  ru_task_t *t = p;
  t->fn(t->arg);
  return NULL;
}
ru_task_t *ru_task_create(const char *name, ru_task_fn fn, void *arg) {
  if (!fn) return NULL;
  ru_task_t *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->fn = fn; t->arg = arg;
  if (name) strncpy(t->name, name, sizeof t->name - 1);
  if (pthread_create(&t->th, NULL, task_tramp, t) != 0) { free(t); return NULL; }
  return t;
}
void ru_task_join(ru_task_t *t) {
  if (!t) return;
  pthread_join(t->th, NULL);
  free(t);
}
