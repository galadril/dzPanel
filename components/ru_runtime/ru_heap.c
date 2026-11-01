#include "ru_heap.h"
#include <stdlib.h>
#include <string.h>

typedef union { size_t n; max_align_t a; } hdr_t;

void ru_heap_init(ru_heap_t *h, size_t cap) { memset(h, 0, sizeof *h); h->cap = cap; }

void *ru_heap_alloc(ru_heap_t *h, size_t n) {
  if (h->cap && h->used + n > h->cap) { h->fails++; return NULL; }
  hdr_t *p = malloc(sizeof(hdr_t) + n);
  if (!p) { h->fails++; return NULL; }
  p->n = n;
  h->used += n; h->live++; h->allocs++;
  if (h->used > h->peak) h->peak = h->used;
  return p + 1;
}

void ru_heap_free(ru_heap_t *h, void *ptr) {
  if (!ptr) return;
  hdr_t *p = (hdr_t *)ptr - 1;
  h->used -= p->n; h->live--; h->frees++;
  free(p);
}
