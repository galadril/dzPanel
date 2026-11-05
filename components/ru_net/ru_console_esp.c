#include "ru_console_esp.h"

#include <string.h>

#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#elif CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#else
#error "ru_console_esp needs CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG or CONFIG_ESP_CONSOLE_UART"
#endif
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
  volatile uint32_t tx_drops;
  uint32_t tx_drops_logged;
  uint32_t tx_drop_log_ms;
} C;

#if CONFIG_ESP_CONSOLE_UART
enum { UART_RX_BUF = 4096 /* RU+CFG lines reach RU_CFGLINE_MAX (2048); the task can stall 15-35 s in Wi-Fi join */, UART_TX_BUF = 2048 };
#define CON_UART ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)
#endif

/* Transport: write n bytes, never block the caller for long. */
static void tx_write(const void *d, size_t n) {
  ru_mutex_lock(C.tx);
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  const uint8_t *p = d;
  while (n) {
    int w = usb_serial_jtag_write_bytes(p, n, pdMS_TO_TICKS(200));
    if (w <= 0) break; /* host not reading: drop rather than block the device */
    p += w;
    n -= (size_t)w;
  }
#else
  /* uart_write_bytes() blocks when the TX ring buffer is full (a stalled line, logs piling up). Only write what fits
   * right now; a message that does not fit is dropped whole, so a frame is never cut in half. */
  size_t room = 0;
  /* The ring stores item headers/alignment and logs share it, so keep a margin or uart_write_bytes could still block. */
  if (uart_get_tx_buffer_free_size(CON_UART, &room) == ESP_OK && room >= n + 16) uart_write_bytes(CON_UART, d, n);
  else C.tx_drops++; /* reported from console_task, never from here (logging would re-enter this path) */
#endif
  ru_mutex_unlock(C.tx);
}

static int rx_read(uint8_t *buf, size_t cap, uint32_t ms) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  return usb_serial_jtag_read_bytes(buf, cap, pdMS_TO_TICKS(ms));
#else
  return uart_read_bytes(CON_UART, buf, cap, pdMS_TO_TICKS(ms));
#endif
}

static bool transport_install(void) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  usb_serial_jtag_driver_config_t dc = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  if (usb_serial_jtag_driver_install(&dc) != ESP_OK) { ESP_LOGE(TAG, "usb_serial_jtag driver install failed"); return false; }
  usb_serial_jtag_vfs_use_driver();
#else
  /* The IDF console already configured the port (115200 8N1 by default, CONFIG_ESP_CONSOLE_UART_BAUDRATE); the driver
   * adds the RX/TX ring buffers and the VFS then routes logs through it. */
  if (uart_driver_install(CON_UART, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0) != ESP_OK) {
    ESP_LOGE(TAG, "uart driver install failed");
    return false;
  }
  uart_vfs_dev_use_driver(CON_UART);
#endif
  return true;
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
    int n = rx_read(buf, sizeof buf, 20);
    uint32_t now = ru_now_ms();
    if (n > 0) {
      if (!C.improv_on) {
        /* Parser state is only ever touched by this task (feed, tick, wifi_result and the ops all run here;
         * ru_console_esp_set_improv only flips the volatile flag), so the wipe below cannot race. */
        if (ru_improv_in_packet(&C.improv)) ru_improv_reset(&C.improv);
        /* Improv disabled: hide 'I' from the demux by feeding through a console without improv attached */
        ru_improv_t *keep = C.con.improv;
        C.con.improv = NULL;
        ru_console_feed(&C.con, buf, (size_t)n, now);
        C.con.improv = keep;
      } else ru_console_feed(&C.con, buf, (size_t)n, now);
      ru_secure_zero(buf, sizeof buf); /* the tail of an RU+CFG line (token) sits here */
    }
    if (!C.improv_on && ru_improv_in_packet(&C.improv)) ru_improv_reset(&C.improv); /* detached with no more input */
    ru_console_tick(&C.con, now);
#if CONFIG_ESP_CONSOLE_UART
    if (C.tx_drops != C.tx_drops_logged && now - C.tx_drop_log_ms >= 10000) {
      uint32_t d = C.tx_drops;
      C.tx_drop_log_ms = now;
      ESP_LOGW(TAG, "console TX dropped %u message(s) (ring full)", (unsigned)(d - C.tx_drops_logged));
      C.tx_drops_logged = d;
    }
#endif
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
  if (!transport_install()) return false;
  ru_improv_init(&C.improv, &C.cfg.improv, cfg->improv_authorized);
  ru_console_ops_t ops = {on_line, on_overflow, NULL};
  ru_console_init(&C.con, &ops, &C.improv);
  C.started = true;
  return ru_task_create("ru_console", console_task, NULL) != NULL;
}

void ru_console_esp_wifi_result(bool ok) { C.wifi_result = ok ? 1 : 2; }
void ru_console_esp_set_improv(bool enabled) { C.improv_on = enabled; }
