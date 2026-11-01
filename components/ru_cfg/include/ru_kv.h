/* ru_kv: key-value store interface (namespaces, strings and blobs). Implementations: ru_kv_mem.c (host tests),
 * ru_kv_nvs.c (ESP-IDF NVS). Namespace and key names are 1..15 chars [A-Za-z0-9_.-] (the NVS limit) in every
 * implementation, so a name that works in tests works on the device. Values are never logged by any implementation. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  RU_KV_OK = 0,
  RU_KV_NOT_FOUND,
  RU_KV_TOO_SMALL, /* output buffer too small; for blobs *len holds the needed size */
  RU_KV_INVALID,   /* bad namespace/key name, NULL argument, or value too large for the store */
  RU_KV_FULL,
  RU_KV_IO
} ru_kv_err_t;

typedef struct ru_kv ru_kv_t;
typedef struct {
  /* strings: out is always NUL terminated on RU_KV_OK; cap includes the NUL */
  ru_kv_err_t (*get_str)(ru_kv_t *kv, const char *ns, const char *key, char *out, size_t cap);
  ru_kv_err_t (*set_str)(ru_kv_t *kv, const char *ns, const char *key, const char *val);
  ru_kv_err_t (*get_blob)(ru_kv_t *kv, const char *ns, const char *key, void *out, size_t cap, size_t *len);
  ru_kv_err_t (*set_blob)(ru_kv_t *kv, const char *ns, const char *key, const void *data, size_t len);
  ru_kv_err_t (*erase_key)(ru_kv_t *kv, const char *ns, const char *key);
  ru_kv_err_t (*erase_ns)(ru_kv_t *kv, const char *ns);
} ru_kv_ops_t;
struct ru_kv { const ru_kv_ops_t *ops; };

static inline ru_kv_err_t ru_kv_get_str(ru_kv_t *kv, const char *ns, const char *key, char *out, size_t cap) {
  return kv->ops->get_str(kv, ns, key, out, cap);
}
static inline ru_kv_err_t ru_kv_set_str(ru_kv_t *kv, const char *ns, const char *key, const char *val) {
  return kv->ops->set_str(kv, ns, key, val);
}
static inline ru_kv_err_t ru_kv_get_blob(ru_kv_t *kv, const char *ns, const char *key, void *out, size_t cap, size_t *len) {
  return kv->ops->get_blob(kv, ns, key, out, cap, len);
}
static inline ru_kv_err_t ru_kv_set_blob(ru_kv_t *kv, const char *ns, const char *key, const void *d, size_t len) {
  return kv->ops->set_blob(kv, ns, key, d, len);
}
static inline ru_kv_err_t ru_kv_erase_key(ru_kv_t *kv, const char *ns, const char *key) {
  return kv->ops->erase_key(kv, ns, key);
}
static inline ru_kv_err_t ru_kv_erase_ns(ru_kv_t *kv, const char *ns) { return kv->ops->erase_ns(kv, ns); }

bool ru_kv_name_ok(const char *name); /* 1..15 chars of [A-Za-z0-9_.-] */
const char *ru_kv_err_name(ru_kv_err_t e);

/* ---- memory implementation (tests, simulator) ---- */
#define RU_KV_MEM_MAX_ENTRIES 24
#define RU_KV_MEM_MAX_VALUE 1024
typedef struct {
  bool used, blob;
  char ns[16], key[16];
  size_t len;
  uint8_t data[RU_KV_MEM_MAX_VALUE];
} ru_kv_mem_entry_t;
typedef struct {
  ru_kv_t base;
  ru_kv_mem_entry_t e[RU_KV_MEM_MAX_ENTRIES];
  int fail_sets_after; /* test hook: -1 = never; n = the (n+1)th set_* call and all later ones return RU_KV_IO */
  unsigned set_calls;
} ru_kv_mem_t;
void ru_kv_mem_init(ru_kv_mem_t *m);
size_t ru_kv_mem_count(const ru_kv_mem_t *m, const char *ns); /* entries in a namespace; NULL = all */
/* test helper: does any stored value contain this byte sequence? */
bool ru_kv_mem_contains(const ru_kv_mem_t *m, const void *needle, size_t len);

/* ---- NVS implementation (ESP-IDF only, ru_kv_nvs.c) ---- */
#ifdef ESP_PLATFORM
/* nvs_flash_init with erase-and-retry on version mismatch; call once at boot (fw-core may already have done it). */
int ru_kv_nvs_init(void);
ru_kv_t *ru_kv_nvs_get(void); /* singleton, opens/closes the NVS handle per operation */
#endif
