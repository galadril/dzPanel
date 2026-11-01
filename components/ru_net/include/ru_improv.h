/* Improv Serial protocol handler (pure C). Reference: github.com/improv-wifi/sdk-cpp (src/improv.cpp,
 * improv.h), cloned and read; the serial framing below follows parse_improv_serial_byte() and
 * build_rpc_response() there.
 *
 *   packet = "IMPROV" 0x01 type len data[len] checksum      checksum = sum of all previous bytes mod 256
 *   type   : 0x01 current state, 0x02 error state, 0x03 RPC command, 0x04 RPC result
 *   RPC command data : command, n, payload[n]        (n must equal len-2)
 *   RPC result  data : command, n, (strlen, bytes)*  (n = bytes after these two)
 *   commands: 0x01 set wifi (ssid_len ssid pass_len pass), 0x02 get current state, 0x03 get device info,
 *             0x04 scan wifi (one result per network, then an empty result)
 *
 * Output: every packet is handed to ops->write in ONE call, framed as "\n" + packet + "\n" so it starts and ends on
 * a line boundary even when ordinary log text is interleaved on the same console (see ru_console.h).
 * Input is fed byte by byte; framing is length based, so 0x0A or 'R' inside a packet are just data.
 * Bad checksum or an invalid RPC: ERROR_INVALID_RPC packet and the packet is dropped. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RU_IMPROV_MAX_PACKET 265 /* 9 header + 255 data + 1 checksum */
#define RU_IMPROV_SSID_MAX 32
#define RU_IMPROV_PASS_MAX 64
#define RU_IMPROV_MAX_NETWORKS 16

typedef enum { RU_IMPROV_STATE_AUTH_REQUIRED = 1, RU_IMPROV_STATE_AUTHORIZED = 2, RU_IMPROV_STATE_PROVISIONING = 3, RU_IMPROV_STATE_PROVISIONED = 4 } ru_improv_state_t;
typedef enum {
  RU_IMPROV_ERR_NONE = 0, RU_IMPROV_ERR_INVALID_RPC = 1, RU_IMPROV_ERR_UNKNOWN_RPC = 2, RU_IMPROV_ERR_UNABLE_TO_CONNECT = 3,
  RU_IMPROV_ERR_NOT_AUTHORIZED = 4, RU_IMPROV_ERR_UNKNOWN = 0xFF
} ru_improv_error_t;

typedef struct {
  char ssid[RU_IMPROV_SSID_MAX + 1];
  int rssi;
  bool secured;
} ru_improv_net_t;

typedef struct {
  /* Emit one complete frame. Required. */
  void (*write)(void *user, const uint8_t *data, size_t len);
  /* Start joining; the result is reported later with ru_improv_wifi_result(). false = could not even start. Required. */
  bool (*wifi_connect)(void *user, const char *ssid, const char *pass);
  /* Blocking scan, returns the count (<= max). Optional (NULL = no networks). */
  size_t (*wifi_scan)(void *user, ru_improv_net_t *out, size_t max);
  /* Is Wi-Fi up now? Decides PROVISIONED vs AUTHORIZED in get-current-state. Optional. */
  bool (*wifi_connected)(void *user);
  void *user;
  const char *fw_name, *fw_version, *chip, *device_name; /* NULL becomes "" */
  const char *redirect_url; /* NULL or "" = none (recommended: fewer ways to leak a token, docs/provisioning.md) */
} ru_improv_ops_t;

typedef struct {
  ru_improv_ops_t ops;
  ru_improv_state_t state;
  ru_improv_error_t error;
  bool authorized;
  uint8_t buf[RU_IMPROV_MAX_PACKET];
  size_t pos;
  uint32_t bad_packets;
  uint32_t errs; /* error packets emitted (internal) */
} ru_improv_t;

void ru_improv_init(ru_improv_t *im, const ru_improv_ops_t *ops, bool authorized);
/* Authorization: when false, state is AUTH_REQUIRED and set-wifi answers ERROR_NOT_AUTHORIZED. */
void ru_improv_set_authorized(ru_improv_t *im, bool authorized);

typedef enum {
  RU_IMPROV_MORE,    /* byte consumed, packet incomplete */
  RU_IMPROV_DONE,    /* byte consumed, a packet ended here (valid or not) */
  RU_IMPROV_NOMATCH  /* byte NOT part of an Improv packet (header or version mismatch): the caller reprocesses it */
} ru_improv_feed_t;
ru_improv_feed_t ru_improv_feed(ru_improv_t *im, uint8_t byte);
void ru_improv_reset(ru_improv_t *im); /* drop a partial packet (idle timeout) */
bool ru_improv_in_packet(const ru_improv_t *im);

/* Outcome of the join started by ops->wifi_connect. ok: PROVISIONED + result packet; else error 0x03 + AUTHORIZED. */
void ru_improv_wifi_result(ru_improv_t *im, bool ok);
