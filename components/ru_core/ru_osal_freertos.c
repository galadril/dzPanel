/* ru_osal FreeRTOS port (ESP-IDF). Selected by the IDF component CMakeLists.txt; the host uses ru_osal_pthread.c.
 * Semantics match the pthread port: queue copies items, timeout 0 on a full queue returns RU_ERR_FULL,
 * a non-zero timeout returns RU_ERR_TIMEOUT. Tasks that return are cleaned up by ru_task_join. */
#include "ru_osal.h"
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef RU_TASK_STACK_BYTES
#define RU_TASK_STACK_BYTES 10240
#endif
#ifndef RU_TASK_PRIORITY
#define RU_TASK_PRIORITY 5
#endif

static TickType_t to_ticks(uint32_t ms) {
  if (ms == RU_WAIT_FOREVER) return portMAX_DELAY;
  return pdMS_TO_TICKS(ms);
}

uint32_t ru_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void ru_sleep_ms(uint32_t ms) { vTaskDelay(ms == 0 ? 0 : (pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1)); }

struct ru_mutex { SemaphoreHandle_t h; };
ru_mutex_t *ru_mutex_create(void) {
  ru_mutex_t *m = malloc(sizeof *m);
  if (!m) return NULL;
  m->h = xSemaphoreCreateMutex();
  if (!m->h) { free(m); return NULL; }
  return m;
}
void ru_mutex_lock(ru_mutex_t *m) { xSemaphoreTake(m->h, portMAX_DELAY); }
void ru_mutex_unlock(ru_mutex_t *m) { xSemaphoreGive(m->h); }
void ru_mutex_destroy(ru_mutex_t *m) {
  if (!m) return;
  vSemaphoreDelete(m->h);
  free(m);
}

struct ru_queue { QueueHandle_t h; };
ru_queue_t *ru_queue_create(size_t item_size, size_t capacity) {
  if (!item_size || !capacity) return NULL;
  ru_queue_t *q = malloc(sizeof *q);
  if (!q) return NULL;
  q->h = xQueueCreate((UBaseType_t)capacity, (UBaseType_t)item_size);
  if (!q->h) { free(q); return NULL; }
  return q;
}
ru_err_t ru_queue_send(ru_queue_t *q, const void *item, uint32_t timeout_ms) {
  if (!q || !item) return RU_ERR_INVALID;
  if (xQueueSend(q->h, item, to_ticks(timeout_ms)) == pdTRUE) return RU_OK;
  return timeout_ms ? RU_ERR_TIMEOUT : RU_ERR_FULL;
}
ru_err_t ru_queue_recv(ru_queue_t *q, void *item, uint32_t timeout_ms) {
  if (!q || !item) return RU_ERR_INVALID;
  return xQueueReceive(q->h, item, to_ticks(timeout_ms)) == pdTRUE ? RU_OK : RU_ERR_TIMEOUT;
}
size_t ru_queue_count(ru_queue_t *q) { return q ? (size_t)uxQueueMessagesWaiting(q->h) : 0; }
void ru_queue_destroy(ru_queue_t *q) {
  if (!q) return;
  vQueueDelete(q->h);
  free(q);
}

struct ru_task { ru_task_fn fn; void *arg; SemaphoreHandle_t done; char name[16]; };
static void task_tramp(void *p) {
  ru_task_t *t = p;
  t->fn(t->arg);
  xSemaphoreGive(t->done);
  vTaskDelete(NULL); /* a FreeRTOS task must never return */
}
ru_task_t *ru_task_create(const char *name, ru_task_fn fn, void *arg) {
  if (!fn) return NULL;
  ru_task_t *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->fn = fn; t->arg = arg;
  if (name) strncpy(t->name, name, sizeof t->name - 1);
  t->done = xSemaphoreCreateBinary();
  if (!t->done) { free(t); return NULL; }
  if (xTaskCreate(task_tramp, t->name[0] ? t->name : "ru_task", RU_TASK_STACK_BYTES, t, RU_TASK_PRIORITY, NULL) != pdPASS) {
    vSemaphoreDelete(t->done);
    free(t);
    return NULL;
  }
  return t;
}
void ru_task_join(ru_task_t *t) {
  if (!t) return;
  xSemaphoreTake(t->done, portMAX_DELAY);
  vTaskDelay(1); /* let the finishing task reach vTaskDelete before its semaphore goes away */
  vSemaphoreDelete(t->done);
  free(t);
}
