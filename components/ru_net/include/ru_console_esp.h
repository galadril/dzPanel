/* Console glue: reads RX, runs ru_console (Improv + RU+CFG demux), writes replies and Improv frames.
 * ESP-IDF only. Compile-checked, not run on a device.
 *
 * The transport follows the IDF console choice (logs and replies share the same port):
 *   CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y  usb_serial_jtag driver (amoled-175); a write drops when the host is not reading.
 *   CONFIG_ESP_CONSOLE_UART=y             uart driver on CONFIG_ESP_CONSOLE_UART_NUM, 115200 8N1 (guition-4848s040, via
 *                                         its CH341 bridge); a write that does not fit the TX buffer is dropped.
 * Either way the driver is installed here and the VFS routed through it; a write never blocks for long. ONE task (ru_console) owns the Improv handler and the RU+CFG
 * handler, so neither needs a lock; every console write goes through one mutex. The RU+CFG connection test runs on
 * this task and needs ~10 KB of stack (TLS); the task is created with that.
 * Accepted on the console only, never on a network port. */
#pragma once
#include <stdbool.h>

#include "ru_cfgline.h"
#include "ru_improv.h"

typedef struct {
  ru_kv_t *kv;
  ru_cfgline_ops_t cfgline; /* test = e.g. dz_service_cfgline_test, saved = tell dz_service to reload */
  ru_improv_ops_t improv;   /* write is filled in here; connect/scan/connected = ru_wifi_esp_* wrappers */
  bool improv_enabled;      /* only while unprovisioned or in setup mode (docs/research/flashing.md) */
  bool improv_authorized;
} ru_console_esp_cfg_t;

bool ru_console_esp_start(const ru_console_esp_cfg_t *cfg);
/* Thread safe: outcome of the join started by improv.wifi_connect (wire ru_wifi_esp on_provision_result to this). */
void ru_console_esp_wifi_result(bool ok);
/* Thread safe: toggle Improv acceptance at runtime (e.g. only in setup mode). */
void ru_console_esp_set_improv(bool enabled);
