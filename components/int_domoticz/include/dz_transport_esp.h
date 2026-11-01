/* esp_http_client implementation of dz_transport_t. ESP-IDF only. Compile-checked, not run on a device.
 *
 * - Authorization (and any header) values are passed to esp_http_client and never logged; "HTTP_CLIENT" and
 *   "esp-tls" log tags are capped at WARN at create time so a debug build cannot print them.
 * - https:// uses the Mozilla CA bundle (esp_crt_bundle_attach) with certificate and hostname verification ON.
 * - http:// only while the insecure_lan flag is set (see dz_service.h set_insecure_lan hook); otherwise the request
 *   is refused with DZ_NET_CONNECT without touching the network. There is no way to switch off certificate checks.
 * - Redirects are never followed (a redirect would carry the token to another host): 3xx comes back as the status.
 * - Body is bounded by the caller's buffer; one byte more than fits gives DZ_NET_TOO_LARGE with a valid status.
 * - timeout_ms is both the per-socket timeout and the overall deadline (checked between reads).
 * - TLS failures (handshake, certificate verify, hostname mismatch) map to DZ_NET_TLS; DNS, refused, reset and
 *   malformed HTTP to DZ_NET_CONNECT; no complete answer in time to DZ_NET_TIMEOUT.
 * - get() is reentrant: every call makes its own client, so the RU+CFG probe may use a second instance.
 * Needs sdkconfig: CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y (default) and a few KB of task stack (>= 6 KB; TLS needs
 * ~8 KB on the calling task; the service task should be created with a 10 KB stack). */
#pragma once
#include <stdbool.h>

#include "dz_transport.h"

dz_transport_t *dz_transport_esp_create(bool insecure_lan);
void dz_transport_esp_set_insecure_lan(dz_transport_t *t, bool allow); /* matches dz_service_cfg_t.set_insecure_lan */
