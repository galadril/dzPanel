#include "svc_glue.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "boot_health.h"
#include "dz_transport_esp.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "heap_log.h"
#include "ru_cfg.h"
#include "ru_console_esp.h"
#include "ru_wifi_esp.h"

static const char *TAG = "svc";

#define MODEL_CAP 512

static ru_kv_t *g_kv;
static dz_service_t *g_svc;
static dz_transport_t *g_probe_tr;
static dz_probe_ctx_t g_probe;
static atomic_bool g_cfg_set, g_wifi_set;
static bool g_with_service;

#if CONFIG_RU_HEAP_LOG
/* Diagnostics only: wraps the esp_http_client transport so each Domoticz HTTP request logs a begin and an end heap mark (internal
 * free/largest/lowest-ever, then status, error code, bytes and elapsed ms). The tag is a fixed word chosen from the "param=" and
 * "filter=" parts of the URL; the URL itself, its query values and the headers are never logged. */
typedef struct { dz_transport_t base; dz_transport_t *inner; bool probe; } mark_tr_t;

typedef struct { const char *needle; const char *begin; const char *end; } mark_tag_t;
static const mark_tag_t k_poll_tags[] = {
    {"param=getdevices&filter=light", "get devs light begin", "get devs light end"},
    {"param=getdevices&filter=temp", "get devs temp begin", "get devs temp end"},
    {"param=getdevices&filter=weather", "get devs weather begin", "get devs weather end"},
    {"param=getdevices&filter=utility", "get devs utility begin", "get devs utility end"},
    {"param=getdevices&rid=", "get dev one begin", "get dev one end"},
    {"lastupdate=", "get devs delta begin", "get devs delta end"},
    {"param=getdevices", "get devs full begin", "get devs full end"},
    {"param=getscenes", "get scenes begin", "get scenes end"},
    {"param=getplans", "get plans begin", "get plans end"},
    {"param=getconfig", "get config begin", "get config end"},
    {"param=getversion", "get version begin", "get version end"},
    {"param=switchlight", "cmd switch begin", "cmd switch end"},
    {"param=switchscene", "cmd scene begin", "cmd scene end"},
    {"param=setsetpoint", "cmd setpoint begin", "cmd setpoint end"},
    {"param=thermostatstate", "cmd thermostat begin", "cmd thermostat end"},
};

static dz_net_err_t mark_get(dz_transport_t *t, const char *url, const char *const *headers, uint32_t timeout_ms, char *buf,
                             size_t cap, dz_http_result_t *out) {
  mark_tr_t *m = (mark_tr_t *)t;
  const char *begin = m->probe ? "probe begin" : "get other begin", *end = m->probe ? "probe end" : "get other end";
  for (size_t i = 0; !m->probe && url && i < sizeof k_poll_tags / sizeof k_poll_tags[0]; i++)
    if (strstr(url, k_poll_tags[i].needle)) { begin = k_poll_tags[i].begin; end = k_poll_tags[i].end; break; }
  ru_heap_log_mark(begin);
  int64_t t0 = esp_timer_get_time();
  dz_net_err_t e = m->inner->get(m->inner, url, headers, timeout_ms, buf, cap, out);
  ru_heap_log_mark_res(end, out->status, (int)e, (unsigned)out->len, (unsigned)((esp_timer_get_time() - t0) / 1000));
  return e;
}
/* MCP: the initialize call is the one that captures the session header. Neither the URL, the body nor any header is logged. */
static dz_net_err_t mark_post(dz_transport_t *t, const char *url, const char *const *headers, const dz_post_t *p, uint32_t timeout_ms,
                              char *buf, size_t cap, dz_http_result_t *out) {
  mark_tr_t *m = (mark_tr_t *)t;
  bool init = p && p->capture_header;
  ru_heap_log_mark(init ? "cmd mcp init begin" : "cmd mcp call begin");
  int64_t t0 = esp_timer_get_time();
  dz_net_err_t e = m->inner->post(m->inner, url, headers, p, timeout_ms, buf, cap, out);
  ru_heap_log_mark_res(init ? "cmd mcp init end" : "cmd mcp call end", out->status, (int)e, (unsigned)out->len,
                       (unsigned)((esp_timer_get_time() - t0) / 1000));
  return e;
}
static void mark_destroy(dz_transport_t *t) {
  mark_tr_t *m = (mark_tr_t *)t;
  if (m->inner && m->inner->destroy) m->inner->destroy(m->inner);
  free(m);
}
static void mark_set_insecure(dz_transport_t *t, bool allow) { dz_transport_esp_set_insecure_lan(((mark_tr_t *)t)->inner, allow); }
static dz_transport_t *transport_create(bool probe) {
  dz_transport_t *in = dz_transport_esp_create(false);
  mark_tr_t *m = calloc(1, sizeof *m);
  if (!in || !m) { if (in) in->destroy(in); free(m); return NULL; }
  m->base.get = mark_get;
  m->base.post = mark_post;
  m->base.destroy = mark_destroy;
  m->inner = in;
  m->probe = probe;
  return &m->base;
}
#define TRANSPORT_SET_INSECURE mark_set_insecure
#else
static dz_transport_t *transport_create(bool probe) { (void)probe; return dz_transport_esp_create(false); }
#define TRANSPORT_SET_INSECURE dz_transport_esp_set_insecure_lan
#endif

dz_service_t *svc_service(void) { return g_svc; }
bool svc_cfg_set(void) { return atomic_load(&g_cfg_set); }
bool svc_provisioned(void) { return atomic_load(&g_cfg_set) && atomic_load(&g_wifi_set); }

static void *psram_calloc(size_t n, size_t sz) { return heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

bool svc_init(ru_kv_t *kv, ru_bus_t *bus, bool with_service) {
  g_kv = kv;
  g_with_service = with_service;
  atomic_store(&g_cfg_set, ru_cfg_is_set(kv));
  ru_wifi_creds_t wc;
  atomic_store(&g_wifi_set, ru_wifi_creds_load(kv, &wc) == RU_CFG_OK);
  ru_wifi_creds_wipe(&wc);
  g_probe_tr = transport_create(true); /* the RU+CFG connection test must not share the polling transport */
  if (!with_service) return true;

  dz_service_cfg_t c;
  memset(&c, 0, sizeof c);
  c.transport = transport_create(false);
  c.set_insecure_lan = TRANSPORT_SET_INSECURE;
  c.bus = bus;
  c.entries_cap = MODEL_CAP;
  c.stage_cap = MODEL_CAP;
  c.entries = psram_calloc(MODEL_CAP, sizeof(dz_entry_t));
  c.stage = psram_calloc(MODEL_CAP, sizeof(dz_device_t));
  c.buf = psram_calloc(1, DZ_SERVICE_MIN_BUF_FULL);
  c.buf_cap = DZ_SERVICE_MIN_BUF_FULL;
  if (!c.buf) { /* no PSRAM: favourites only, small internal buffer */
    c.favorites_only = true;
    c.buf_cap = DZ_SERVICE_MIN_BUF_FAVS;
    c.buf = heap_caps_calloc(1, c.buf_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGW(TAG, "no PSRAM buffer: favourites only");
  }
  g_svc = psram_calloc(1, sizeof *g_svc);
  if (!c.transport || !c.entries || !c.stage || !c.buf || !g_svc || !dz_service_init(g_svc, &c)) {
    ESP_LOGE(TAG, "service init failed");
    g_svc = NULL;
    return false;
  }
  ESP_LOGI(TAG, "service ready: model %d entries, buffer %u KB%s", MODEL_CAP, (unsigned)(c.buf_cap / 1024),
           c.favorites_only ? " (favourites only)" : "");
  return true;
}

/* Re-read the stored config and hand it to the service. Runs on the console task (RU+CFG "saved") or at boot. */
static void apply_stored_config(void) {
  ru_cfg_t c;
  if (ru_cfg_load(g_kv, &c) == RU_CFG_OK) {
    atomic_store(&g_cfg_set, true);
    if (g_svc) {
      ru_cfg_err_t e = dz_service_configure(g_svc, &c, true);
      if (e != RU_CFG_OK) ESP_LOGW(TAG, "stored config refused: %s", ru_cfg_err_name(e));
    }
  }
  ru_cfg_wipe(&c);
}

static void update_improv(void) { ru_console_esp_set_improv(!svc_provisioned()); }

static void on_cfg_saved(void *u) {
  (void)u;
  ESP_LOGI(TAG, "configuration saved over the console");
  apply_stored_config();
  update_improv();
}

static void on_wifi_state(void *u, ru_wifi_state_t from, ru_wifi_state_t to) {
  (void)u;
  (void)from;
  if (g_svc) dz_service_set_net_up(g_svc, to == RU_WIFI_CONNECTED);
}
static void on_wifi_ap(void *u) { (void)u; ESP_LOGW(TAG, "Wi-Fi setup portal requested: not implemented, use the installer page"); }
static void on_wifi_provision(void *u, bool ok) {
  (void)u;
  if (ok) atomic_store(&g_wifi_set, true);
  ru_console_esp_wifi_result(ok);
  update_improv();
}

static bool wifi_up_cb(void *u) { (void)u; return ru_wifi_esp_connected(); }
static bool improv_connect(void *u, const char *ssid, const char *pass) { (void)u; return ru_wifi_esp_try_new(ssid, pass); }
/* RU+CFG "wifi" block: blocking join (<= 20 s), saved only on got-IP, else the previous credentials return. */
static ru_wifi_apply_t cfgline_wifi_apply(void *u, const char *ssid, const char *pass, uint32_t timeout_ms) {
  (void)u;
  ru_wifi_apply_t r = ru_wifi_esp_apply_wait(ssid, pass, timeout_ms);
  if (r == RU_WIFI_APPLY_OK) {
    atomic_store(&g_wifi_set, true);
    update_improv();
    ESP_LOGI(TAG, "Wi-Fi saved over the console");
  } else ESP_LOGW(TAG, "Wi-Fi from the console failed (%d)", (int)r); /* never the credentials */
  return r;
}
static size_t improv_scan(void *u, ru_improv_net_t *out, size_t max) { (void)u; return ru_wifi_esp_scan(out, max); }
static bool improv_connected(void *u) { (void)u; return ru_wifi_esp_connected(); }

void svc_start(void) {
  ru_wifi_esp_cfg_t wc;
  memset(&wc, 0, sizeof wc);
  wc.kv = g_kv;
  wc.on_state = on_wifi_state;
  wc.on_ap_requested = on_wifi_ap;
  wc.on_provision_result = on_wifi_provision;
  if (!ru_wifi_esp_start(&wc)) ESP_LOGE(TAG, "Wi-Fi start failed");

  g_probe.transport = g_probe_tr;
  g_probe.set_insecure = TRANSPORT_SET_INSECURE;
  g_probe.wifi_up = wifi_up_cb;
  ru_console_esp_cfg_t cc;
  memset(&cc, 0, sizeof cc);
  cc.kv = g_kv;
  cc.cfgline.test = dz_service_cfgline_test;
  cc.cfgline.saved = on_cfg_saved;
  cc.cfgline.user = &g_probe; /* test() receives this; saved() and wifi_apply() ignore it */
  cc.cfgline.wifi_apply = cfgline_wifi_apply;
  cc.improv.wifi_connect = improv_connect;
  cc.improv.wifi_scan = improv_scan;
  cc.improv.wifi_connected = improv_connected;
  cc.improv.fw_name = "dzPanel";
  cc.improv.fw_version = esp_app_get_description()->version;
  cc.improv.chip = "ESP32-S3";
  cc.improv.device_name = "dzPanel";
  cc.improv_enabled = !svc_provisioned(); /* only while unprovisioned (docs/research/flashing.md) */
  cc.improv_authorized = true; /* REVIEW: no physical authorisation step yet; USB access is already physical access */
  if (!ru_console_esp_start(&cc)) ESP_LOGE(TAG, "console start failed");

  if (g_svc) {
    if (!dz_service_start(g_svc)) ESP_LOGE(TAG, "service task failed");
    apply_stored_config();
  }
}

void svc_factory_reset(void) {
  if (g_svc) dz_service_clear_config(g_svc);
  (void)ru_cfg_clear(g_kv);
  ru_wifi_esp_forget();
  (void)ru_wifi_creds_clear(g_kv);
  atomic_store(&g_cfg_set, false);
  atomic_store(&g_wifi_set, false);
  bh_clear();
  ESP_LOGW(TAG, "factory reset done");
}
