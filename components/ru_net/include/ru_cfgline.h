/* RU+CFG line handler (pure C). Spec: docs/provisioning.md.
 *
 *   host   -> RU+CFG {"v":1,"domoticz":{"url":"...","token":"...","insecure_lan":false}[,"force":true]}\n
 *   device -> RU+CFG OK <info>\n   |   RU+CFG ERR <code> [detail]\n
 *
 * The handler gets the text after "RU+CFG " (the framing, the 2 KiB cap and the idle timeout live in ru_console.h),
 * validates it with ru_cfg, asks the test callback to run getversion with the token, and saves to the "secrets"
 * namespace only when that succeeds (or when it was "unreachable" and the line said "force":true).
 *
 * Optional Wi-Fi block (v stays 1; a line without "wifi" behaves exactly as before):
 *   RU+CFG {"v":1,"wifi":{"ssid":"...","password":"..."},"domoticz":{...}[,"force":true]}
 * At least one of wifi / domoticz. Wi-Fi is applied FIRST (ops.wifi_apply, saved only on got-IP), then the Domoticz test
 * and save. Replies: wifi only "RU+CFG OK wifi_saved"; both "RU+CFG OK wifi_saved saved[ info|untested]"; Wi-Fi failure
 * "RU+CFG ERR wifi_failed [no_ap|auth|timeout]" (Domoticz not attempted); bad block "RU+CFG ERR bad_wifi"; a Domoticz
 * error after a successful join keeps the Wi-Fi and appends " wifi_saved" to that error reply. force applies only to
 * the Domoticz "unreachable" case.
 *
 * Hard rules, all tested: the reply never contains any byte of the line (no echo, no token, no URL); every call
 * produces exactly one reply; nothing is saved on any error; the parsed token is wiped before returning. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "ru_cfg.h"

#define RU_CFGLINE_PREFIX "RU+CFG "
#define RU_CFGLINE_PREFIX_LEN 7
#define RU_CFGLINE_MAX 2048        /* whole line including the prefix, excluding the newline */
#define RU_CFGLINE_TIMEOUT_MS 15000 /* budget the test callback gets */
#define RU_CFGLINE_REPLY_MAX 96
#define RU_CFGLINE_WIFI_TIMEOUT_MS 20000 /* budget the Wi-Fi join gets (before the 15 s connection test) */

/* Result of applying Wi-Fi credentials from the line. Detail words are only reported when the driver says so. */
typedef enum {
  RU_WIFI_APPLY_OK = 0, /* got an IP and the credentials are saved */
  RU_WIFI_APPLY_NO_AP,  /* network not found */
  RU_WIFI_APPLY_AUTH,   /* rejected: wrong password or security mode */
  RU_WIFI_APPLY_TIMEOUT,/* no answer within the budget */
  RU_WIFI_APPLY_FAILED, /* joined nothing, reason unknown (also: no Wi-Fi driver) */
  RU_WIFI_APPLY_STORE,  /* joined, but the credentials could not be stored */
  RU_WIFI_APPLY_BUSY    /* another attempt is still running: nothing was started, nothing changed */
} ru_wifi_apply_t;

typedef struct {
  /* Runs the real connection test (getversion with the token). Must return within timeout_ms. info (optional)
   * receives a short server version text on success; it is sanitised before it is printed. */
  ru_cfg_test_t (*test)(void *user, const ru_cfg_t *cfg, uint32_t timeout_ms, char *info, size_t info_cap);
  /* Called after a successful save so the service can apply the new config. Optional. Do not keep the pointer. */
  void (*saved)(void *user);
  void *user;
  /* Wi-Fi from the line: try the new credentials, save them only when an IP was obtained, else go back to the previous
   * ones. Blocks up to timeout_ms. Optional: when NULL a line with "wifi" answers wifi_failed. Must not keep the
   * pointers. Added last so older initialisers {test, saved, user} stay valid. */
  ru_wifi_apply_t (*wifi_apply)(void *user, const char *ssid, const char *pass, uint32_t timeout_ms);
} ru_cfgline_ops_t;

/* Handles one payload. Writes the reply line WITHOUT the trailing newline into reply and returns its length.
 * cap should be >= RU_CFGLINE_REPLY_MAX. */
size_t ru_cfgline_handle(ru_kv_t *kv, const ru_cfgline_ops_t *ops, const char *json, size_t len, char *reply, size_t cap);
/* The reply for a line that exceeded RU_CFGLINE_MAX. */
size_t ru_cfgline_reply_too_long(char *reply, size_t cap);
