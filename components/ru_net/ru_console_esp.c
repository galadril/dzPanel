#include "ru_console_esp.h"

#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "ru_console.h"
#include "ru_osal.h"

static const char *TAG = "ru_console";

static struct {
  ru_console_esp_cfg_t cfg;
  ru_console_t con;
  ru_improv_t improv;
  ru_mutex_t *tx;
  volatile int wifi_result; /* 0 none, 1 ok, 2 failed */
  volatile bool improv_on;
  bool started;
} C;

static void tx_write(const void *d, size_t n) {
  ru_mutex_lock(C.tx);
  const uint8_t *p = d;
  while (n) {
    int w = usb_serial_jtag_write_bytes(p, n, pdMS_TO_TICKS(200));
    if (w <= 0) break; /* host not reading: drop rather than block the device */
    p += w;
    n -= (size_t)w;
  }
  ru_mutex_unlock(C.tx);
}

static void improv_write(void *u, const uint8_t *d, size_t n) { (void)u; tx_write(d, n); }

static void reply_line(const char *r, size_t n) {
  char out[RU_CFGLINE_REPLY_MAX + 2];
  if (n > RU_CFGLINE_REPLY_MAX) n = RU_CFGLINE_REPLY_MAX;
  out[0] = '\n'; /* start on a line boundary even if a log line is half written */
  memcpy(out + 1, r, n);
  out[n + 1] = '\n';
  tx_write(out, n + 2);
}

static void on_line(void *u, const char *json, size_t len) {
  (void)u;
  char reply[RU_CFGLINE_REPLY_MAX];
  size_t n = ru_cfgline_handle(C.cfg.kv, &C.cfg.cfgline, json, len, reply, sizeof reply);
  reply_line(reply, n);
}
static void on_overflow(void *u) {
  (void)u;
  char reply[RU_CFGLINE_REPLY_MAX];
  size_t n = ru_cfgline_reply_too_long(reply, sizeof reply);
  reply_line(reply, n);
}

static void console_task(void *arg) {
  (void)arg;
  uint8_t buf[128];
  for (;;) {
    int n = usb_serial_jtag_read_bytes(buf, sizeof buf, pdMS_TO_TICKS(20));
    uint32_t now = ru_now_ms();
    if (n > 0) {
      if (!C.improv_on) {
        /* Improv disabled: hide 'I' from the demux by feeding through a console without improv attached */
        ru_improv_t *keep = C.con.improv;
        C.con.improv = NULL;
        ru_console_feed(&C.con, buf, (size_t)n, now);
        C.con.improv = keep;
      } else ru_console_feed(&C.con, buf, (size_t)n, now);
      ru_secure_zero(buf, sizeof buf); /* the tail of an RU+CFG line (token) sits here */
    }
    ru_console_tick(&C.con, now);
    int r = C.wifi_result;
    if (r) { C.wifi_result = 0; ru_improv_wifi_result(&C.improv, r == 1); }
  }
}

bool ru_console_esp_start(const ru_console_esp_cfg_t *cfg) {
  if (C.started || !cfg || !cfg->kv) return false;
  memset(&C, 0, sizeof C);
  C.cfg = *cfg;
  C.cfg.improv.write = improv_write;
  C.cfg.improv.user = NULL;
  C.improv_on = cfg->improv_enabled;
  C.tx = ru_mutex_create();
  if (!C.tx) return false;
  usb_serial_jtag_driver_config_t dc = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  if (usb_serial_jtag_driver_install(&dc) != ESP_OK) { ESP_LOGE(TAG, "usb_serial_jtag driver install failed"); return false; }
  usb_serial_jtag_vfs_use_driver();
  ru_improv_init(&C.improv, &C.cfg.improv, cfg->improv_authorized);
  ru_console_ops_t ops = {on_line, on_overflow, NULL};
  ru_console_init(&C.con, &ops, &C.improv);
  C.started = true;
  return ru_task_create("ru_console", console_task, NULL) != NULL;
}

void ru_console_esp_wifi_result(bool ok) { C.wifi_result = ok ? 1 : 2; }
void ru_console_esp_set_improv(bool enabled) { C.improv_on = enabled; }
