/* ru_heap: counting allocator with an optional simulated capacity (host stand-in for heap_caps). */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
  size_t cap;        /* 0 = unlimited */
  size_t used;       /* live bytes (payload only) */
  size_t peak;
  size_t live;       /* live allocation count */
  uint32_t fails;    /* allocations refused (cap) */
  uint32_t allocs, frees;
} ru_heap_t;

void ru_heap_init(ru_heap_t *h, size_t cap);
void *ru_heap_alloc(ru_heap_t *h, size_t n);   /* NULL if it would exceed cap */
void ru_heap_free(ru_heap_t *h, void *p);      /* NULL ok */
