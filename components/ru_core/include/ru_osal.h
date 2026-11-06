/* ru_osal: tiny OS abstraction. Host port = pthreads. ESP-IDF port = FreeRTOS (later). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { RU_OK = 0, RU_ERR_INVALID, RU_ERR_TIMEOUT, RU_ERR_NOMEM, RU_ERR_FULL } ru_err_t;
#define RU_WAIT_FOREVER UINT32_MAX

uint32_t ru_now_ms(void); /* monotonic */
uint32_t ru_now_us(void); /* monotonic, wraps every 71 minutes: only for differences (diagnostics) */
void ru_sleep_ms(uint32_t ms);

typedef struct ru_mutex ru_mutex_t;
ru_mutex_t *ru_mutex_create(void);
void ru_mutex_lock(ru_mutex_t *m);
void ru_mutex_unlock(ru_mutex_t *m);
void ru_mutex_destroy(ru_mutex_t *m);

/* Fixed-size item queue; copies items. */
typedef struct ru_queue ru_queue_t;
ru_queue_t *ru_queue_create(size_t item_size, size_t capacity);
ru_err_t ru_queue_send(ru_queue_t *q, const void *item, uint32_t timeout_ms);
ru_err_t ru_queue_recv(ru_queue_t *q, void *item, uint32_t timeout_ms);
size_t ru_queue_count(ru_queue_t *q);
void ru_queue_destroy(ru_queue_t *q);

typedef struct ru_task ru_task_t;
typedef void (*ru_task_fn)(void *arg);
ru_task_t *ru_task_create(const char *name, ru_task_fn fn, void *arg);
void ru_task_join(ru_task_t *t); /* joins and frees */
