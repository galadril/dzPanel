/* NVS implementation of ru_kv. ESP-IDF only. Compile-checked, not run on a device. */
#include <string.h>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ru_kv.h"

static ru_kv_err_t map(esp_err_t e) {
  switch (e) {
    case ESP_OK: return RU_KV_OK;
    case ESP_ERR_NVS_NOT_FOUND: return RU_KV_NOT_FOUND;
    case ESP_ERR_NVS_INVALID_LENGTH: return RU_KV_TOO_SMALL;
    case ESP_ERR_NVS_INVALID_NAME:
    case ESP_ERR_NVS_KEY_TOO_LONG:
    case ESP_ERR_NVS_VALUE_TOO_LONG:
    case ESP_ERR_INVALID_ARG: return RU_KV_INVALID;
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
    case ESP_ERR_NVS_PAGE_FULL: return RU_KV_FULL;
    default: return RU_KV_IO;
  }
}

int ru_kv_nvs_init(void) {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    if (nvs_flash_erase() != ESP_OK) return -1;
    e = nvs_flash_init();
  }
  return e == ESP_OK ? 0 : -1;
}

static ru_kv_err_t open_h(const char *ns, const char *key, bool rw, nvs_handle_t *h) {
  if (!ru_kv_name_ok(ns) || (key && !ru_kv_name_ok(key))) return RU_KV_INVALID;
  return map(nvs_open(ns, rw ? NVS_READWRITE : NVS_READONLY, h));
}

static ru_kv_err_t n_get_str(ru_kv_t *kv, const char *ns, const char *key, char *out, size_t cap) {
  (void)kv;
  nvs_handle_t h;
  if (!out || !cap) return RU_KV_INVALID;
  ru_kv_err_t r = open_h(ns, key, false, &h);
  if (r != RU_KV_OK) return r == RU_KV_NOT_FOUND ? RU_KV_NOT_FOUND : r; /* namespace absent == key absent */
  size_t n = cap;
  esp_err_t e = nvs_get_str(h, key, out, &n);
  nvs_close(h);
  if (e != ESP_OK) out[0] = 0;
  return map(e);
}
static ru_kv_err_t n_set_str(ru_kv_t *kv, const char *ns, const char *key, const char *val) {
  (void)kv;
  nvs_handle_t h;
  if (!val) return RU_KV_INVALID;
  ru_kv_err_t r = open_h(ns, key, true, &h);
  if (r != RU_KV_OK) return r;
  esp_err_t e = nvs_set_str(h, key, val);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return map(e);
}
static ru_kv_err_t n_get_blob(ru_kv_t *kv, const char *ns, const char *key, void *out, size_t cap, size_t *len) {
  (void)kv;
  nvs_handle_t h;
  ru_kv_err_t r = open_h(ns, key, false, &h);
  if (r != RU_KV_OK) return r;
  size_t n = 0;
  esp_err_t e = nvs_get_blob(h, key, NULL, &n); /* size query */
  if (e == ESP_OK) {
    if (len) *len = n;
    if (n > cap) e = ESP_ERR_NVS_INVALID_LENGTH;
    else if (n) e = nvs_get_blob(h, key, out, &n);
  }
  nvs_close(h);
  return map(e);
}
static ru_kv_err_t n_set_blob(ru_kv_t *kv, const char *ns, const char *key, const void *d, size_t len) {
  (void)kv;
  nvs_handle_t h;
  if (!d && len) return RU_KV_INVALID;
  ru_kv_err_t r = open_h(ns, key, true, &h);
  if (r != RU_KV_OK) return r;
  esp_err_t e = nvs_set_blob(h, key, d, len);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return map(e);
}
static ru_kv_err_t n_erase_key(ru_kv_t *kv, const char *ns, const char *key) {
  (void)kv;
  nvs_handle_t h;
  ru_kv_err_t r = open_h(ns, key, true, &h);
  if (r != RU_KV_OK) return r;
  esp_err_t e = nvs_erase_key(h, key);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return map(e);
}
static ru_kv_err_t n_erase_ns(ru_kv_t *kv, const char *ns) {
  (void)kv;
  nvs_handle_t h;
  ru_kv_err_t r = open_h(ns, NULL, true, &h);
  if (r != RU_KV_OK) return r;
  esp_err_t e = nvs_erase_all(h);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return map(e);
}

static const ru_kv_ops_t OPS = {n_get_str, n_set_str, n_get_blob, n_set_blob, n_erase_key, n_erase_ns};
static ru_kv_t SINGLETON = {&OPS};
ru_kv_t *ru_kv_nvs_get(void) { return &SINGLETON; }
