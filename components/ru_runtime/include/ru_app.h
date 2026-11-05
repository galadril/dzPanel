/* ru_app: app descriptor. Apps are compiled in; the runtime owns their lifecycle.
 * All hooks run on the runtime's (UI) thread and must not block. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ru_bus.h"
#include "ru_heap.h"
#include "ru_osal.h"

typedef enum { RU_MEM_LIGHT = 0, RU_MEM_HEAVY = 1 } ru_mem_class_t;
typedef enum { RU_LEVEL_NORMAL = 0, RU_LEVEL_LOW, RU_LEVEL_CRITICAL, RU_LEVEL_FATAL } ru_mem_level_t;

typedef struct ru_runtime ru_runtime_t;

/* What an app may use. Apps allocate through env->heap so the runtime can see pressure. */
typedef struct {
  ru_runtime_t *rt;
  ru_heap_t *heap;
  ru_bus_t *bus;
} ru_app_env_t;

typedef struct {
  const char *id;          /* "org.dzpanel.home" */
  const char *name;        /* display name or i18n key */
  uint16_t icon_id;
  ru_mem_class_t mem_class;
  /* Required. Allocate state; *state may be NULL if the app needs none. Fail with RU_ERR_NOMEM. */
  ru_err_t (*create)(const ru_app_env_t *env, void **state);
  void (*destroy)(void *state);                              /* required: free everything */
  void (*on_event)(void *state, const ru_event_t *ev);       /* optional; foreground only */
  void (*on_visible)(void *state, bool visible);             /* optional; true on enter, false on suspend */
  void (*on_low_memory)(void *state, ru_mem_level_t level);  /* optional; free caches */
} ru_app_t;
