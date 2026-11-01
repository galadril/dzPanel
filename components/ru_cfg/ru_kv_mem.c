#include <string.h>

#include "ru_kv.h"

bool ru_kv_name_ok(const char *n) {
  if (!n) return false;
  size_t l = strlen(n);
  if (l < 1 || l > 15) return false;
  for (size_t i = 0; i < l; i++) {
    char c = n[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
      return false;
  }
  return true;
}

const char *ru_kv_err_name(ru_kv_err_t e) {
  switch (e) {
    case RU_KV_OK: return "ok";
    case RU_KV_NOT_FOUND: return "not_found";
    case RU_KV_TOO_SMALL: return "too_small";
    case RU_KV_INVALID: return "invalid";
    case RU_KV_FULL: return "full";
    case RU_KV_IO: return "io";
  }
  return "?";
}

static ru_kv_mem_t *self(ru_kv_t *kv) { return (ru_kv_mem_t *)kv; }

static ru_kv_mem_entry_t *find(ru_kv_mem_t *m, const char *ns, const char *key) {
  for (int i = 0; i < RU_KV_MEM_MAX_ENTRIES; i++)
    if (m->e[i].used && !strcmp(m->e[i].ns, ns) && !strcmp(m->e[i].key, key)) return &m->e[i];
  return NULL;
}

static ru_kv_err_t put(ru_kv_t *kv, const char *ns, const char *key, bool blob, const void *d, size_t len) {
  ru_kv_mem_t *m = self(kv);
  if (!ru_kv_name_ok(ns) || !ru_kv_name_ok(key) || (!d && len) || len > RU_KV_MEM_MAX_VALUE) return RU_KV_INVALID;
  m->set_calls++;
  if (m->fail_sets_after >= 0 && m->set_calls > (unsigned)m->fail_sets_after) return RU_KV_IO;
  ru_kv_mem_entry_t *e = find(m, ns, key);
  if (!e) {
    for (int i = 0; i < RU_KV_MEM_MAX_ENTRIES && !e; i++)
      if (!m->e[i].used) e = &m->e[i];
    if (!e) return RU_KV_FULL;
    memset(e, 0, sizeof *e);
    e->used = true;
    strcpy(e->ns, ns);
    strcpy(e->key, key);
  }
  e->blob = blob;
  e->len = len;
  if (len) memcpy(e->data, d, len);
  return RU_KV_OK;
}

static ru_kv_err_t m_set_str(ru_kv_t *kv, const char *ns, const char *key, const char *val) {
  if (!val) return RU_KV_INVALID;
  return put(kv, ns, key, false, val, strlen(val) + 1);
}
static ru_kv_err_t m_set_blob(ru_kv_t *kv, const char *ns, const char *key, const void *d, size_t len) {
  return put(kv, ns, key, true, d, len);
}
static ru_kv_err_t m_get_str(ru_kv_t *kv, const char *ns, const char *key, char *out, size_t cap) {
  if (!ru_kv_name_ok(ns) || !ru_kv_name_ok(key) || !out || !cap) return RU_KV_INVALID;
  ru_kv_mem_entry_t *e = find(self(kv), ns, key);
  if (!e || e->blob) return RU_KV_NOT_FOUND;
  if (e->len > cap) { out[0] = 0; return RU_KV_TOO_SMALL; }
  memcpy(out, e->data, e->len);
  return RU_KV_OK;
}
static ru_kv_err_t m_get_blob(ru_kv_t *kv, const char *ns, const char *key, void *out, size_t cap, size_t *len) {
  if (!ru_kv_name_ok(ns) || !ru_kv_name_ok(key) || (!out && cap)) return RU_KV_INVALID;
  ru_kv_mem_entry_t *e = find(self(kv), ns, key);
  if (!e || !e->blob) return RU_KV_NOT_FOUND;
  if (len) *len = e->len;
  if (e->len > cap) return RU_KV_TOO_SMALL;
  if (e->len) memcpy(out, e->data, e->len);
  return RU_KV_OK;
}
static ru_kv_err_t m_erase_key(ru_kv_t *kv, const char *ns, const char *key) {
  if (!ru_kv_name_ok(ns) || !ru_kv_name_ok(key)) return RU_KV_INVALID;
  ru_kv_mem_entry_t *e = find(self(kv), ns, key);
  if (!e) return RU_KV_NOT_FOUND;
  memset(e, 0, sizeof *e);
  return RU_KV_OK;
}
static ru_kv_err_t m_erase_ns(ru_kv_t *kv, const char *ns) {
  if (!ru_kv_name_ok(ns)) return RU_KV_INVALID;
  ru_kv_mem_t *m = self(kv);
  for (int i = 0; i < RU_KV_MEM_MAX_ENTRIES; i++)
    if (m->e[i].used && !strcmp(m->e[i].ns, ns)) memset(&m->e[i], 0, sizeof m->e[i]);
  return RU_KV_OK;
}

static const ru_kv_ops_t OPS = {m_get_str, m_set_str, m_get_blob, m_set_blob, m_erase_key, m_erase_ns};

void ru_kv_mem_init(ru_kv_mem_t *m) {
  memset(m, 0, sizeof *m);
  m->base.ops = &OPS;
  m->fail_sets_after = -1;
}
size_t ru_kv_mem_count(const ru_kv_mem_t *m, const char *ns) {
  size_t n = 0;
  for (int i = 0; i < RU_KV_MEM_MAX_ENTRIES; i++)
    if (m->e[i].used && (!ns || !strcmp(m->e[i].ns, ns))) n++;
  return n;
}
bool ru_kv_mem_contains(const ru_kv_mem_t *m, const void *needle, size_t len) {
  if (!len) return false;
  for (int i = 0; i < RU_KV_MEM_MAX_ENTRIES; i++) {
    if (!m->e[i].used || m->e[i].len < len) continue;
    for (size_t j = 0; j + len <= m->e[i].len; j++)
      if (!memcmp(m->e[i].data + j, needle, len)) return true;
  }
  return false;
}
