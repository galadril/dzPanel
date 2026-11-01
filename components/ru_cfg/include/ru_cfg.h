/* ru_cfg: typed device configuration on top of ru_kv. Schema v1 from docs/provisioning.md, validated with the same
 * rules as site/lib.js (normaliseUrl, validateConfig). Pure C, no IDF.
 *
 * Secrets: Domoticz url + token and the Wi-Fi credentials live in the "secrets" namespace. The token is never logged,
 * never returned by an API meant for display (use ru_cfg_token_masked), and RAM copies are wiped with ru_secure_zero
 * after use. Nothing in this component prints. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "ru_kv.h"

#define RU_CFG_NS_SECRETS "secrets"
#define RU_CFG_URL_MAX 192   /* includes NUL; matches DZ_URL_MAX */
#define RU_CFG_TOKEN_MAX 768 /* includes NUL; matches DZ_TOKEN_MAX */
#define RU_CFG_SCHEMA 1

typedef struct {
  char url[RU_CFG_URL_MAX];     /* normalised: scheme://host[:port][/path], no trailing slash */
  char token[RU_CFG_TOKEN_MAX]; /* SECRET */
  bool insecure_lan;            /* explicit consent to plain http (or a LAN install); device shows a warning icon */
} ru_cfg_t;

typedef struct {
  char ssid[33];
  char pass[65]; /* SECRET */
} ru_wifi_creds_t;

typedef enum {
  RU_CFG_OK = 0,
  RU_CFG_BAD_JSON,
  RU_CFG_BAD_VERSION, /* v missing-as-number is bad_json; v != 1 is bad_version ("update the firmware" when higher) */
  RU_CFG_BAD_URL,     /* includes: plain http without insecure_lan */
  RU_CFG_TOO_LONG,
  RU_CFG_NOT_SET,     /* load: nothing (complete) stored */
  RU_CFG_STORE,       /* kv failure */
  RU_CFG_BAD_WIFI     /* wifi block present but ssid/password invalid (see ru_wifi_ssid_check, ru_wifi_pass_check) */
} ru_cfg_err_t;
const char *ru_cfg_err_name(ru_cfg_err_t e); /* "bad_json", "bad_version", "bad_url", "too_long", ... */

/* Result of a connection test (getversion with the token). Shared by the RU+CFG handler and the Domoticz service. */
typedef enum {
  RU_CFG_TEST_OK = 0,
  RU_CFG_TEST_UNREACHABLE,
  RU_CFG_TEST_TLS,
  RU_CFG_TEST_TOKEN_REJECTED,
  RU_CFG_TEST_NO_WIFI,
  RU_CFG_TEST_BAD_RESPONSE /* not Domoticz: reported as "unreachable" with a detail */
} ru_cfg_test_t;

/* Overwrites p with zeros in a way the compiler may not remove. */
void ru_secure_zero(void *p, size_t n);

/* Same rules as normaliseUrl() in site/lib.js, deliberately a little stricter: any '?' or '#' is refused, path
 * segments "." and ".." are refused, non-ASCII or control characters are refused (no IDN, no percent-decoding).
 * Adds "http://" when no scheme is present, lowercases scheme and host, drops the default port, strips trailing
 * slashes, keeps a sub-path. Returns RU_CFG_OK, RU_CFG_BAD_URL or RU_CFG_TOO_LONG. *https may be NULL. */
ru_cfg_err_t ru_cfg_normalise_url(const char *in, char *out, size_t cap, bool *https);

/* Token rules: non-empty, no whitespace/control/non-ASCII, < RU_CFG_TOKEN_MAX. Returns OK, BAD_JSON or TOO_LONG.
 * *jwt_like (optional) says whether it looks like a signed JWT (a soft warning only). */
ru_cfg_err_t ru_cfg_check_token(const char *token, bool *jwt_like);

/* Validates a full config (url already normalised and re-checked, http needs insecure_lan, token rules). */
ru_cfg_err_t ru_cfg_validate(const ru_cfg_t *c);

/* Parses one RU+CFG JSON object (the text after "RU+CFG "), depth capped at RU_CFG_JSON_DEPTH. Unknown keys are
 * ignored. On any error *out is zeroed. *force (optional) receives "force":true. Never copies input into messages. */
#define RU_CFG_JSON_DEPTH 6
ru_cfg_err_t ru_cfg_parse_json(const char *json, size_t len, ru_cfg_t *out, bool *force); /* Domoticz-only view: BAD_JSON when the line has no "domoticz" */

/* Full RU+CFG line (v1): optional "wifi":{"ssid","password"} and/or "domoticz":{...} (at least one), optional force.
 * Wi-Fi errors are RU_CFG_BAD_WIFI (checked before the Domoticz fields). On any error *out is zeroed. */
typedef struct {
  bool have_wifi, have_dz, force;
  ru_wifi_creds_t wifi; /* SECRET password */
  ru_cfg_t dz;          /* SECRET token */
} ru_cfg_line_t;
ru_cfg_err_t ru_cfg_parse_line(const char *json, size_t len, ru_cfg_line_t *out);
void ru_cfg_line_wipe(ru_cfg_line_t *l);

/* Storage. Save writes url, insecure flag and token, then a schema marker last; a failure erases the namespace
 * entries again so a half-written config is never loaded. */
ru_cfg_err_t ru_cfg_save(ru_kv_t *kv, const ru_cfg_t *c);
ru_cfg_err_t ru_cfg_load(ru_kv_t *kv, ru_cfg_t *out); /* RU_CFG_NOT_SET when absent/incomplete/invalid */
ru_cfg_err_t ru_cfg_clear(ru_kv_t *kv);               /* erases Domoticz keys only */
bool ru_cfg_is_set(ru_kv_t *kv);                      /* cheap check, does not copy the token out */

/* UI-safe views. The masked token never contains any character of the real token. */
void ru_cfg_token_masked(const ru_cfg_t *c, char *out, size_t cap); /* "(none)" or "********" */
void ru_cfg_wipe(ru_cfg_t *c);

/* RU+CFG Wi-Fi rules (same vectors in site/lib.js validateWifi). Control characters are U+0000..U+001F, U+007F and
 * U+0080..U+009F.
 *  ssid: 1..32 BYTES of well-formed UTF-8, no control characters.
 *  password: "" (open network), or 8..63 printable ASCII (0x20..0x7E, WPA2 passphrase), or exactly 64 hex digits (raw PSK). */
bool ru_wifi_ssid_check(const char *ssid);
bool ru_wifi_pass_check(const char *pass);
/* "(none)" for an open network, else "********": reveals neither content nor length. */
void ru_wifi_pass_masked(const ru_wifi_creds_t *c, char *out, size_t cap);

/* Storage-level check for Wi-Fi credentials (same namespace). ssid 1..32 bytes, pass 0 or 8..64 bytes (open networks allowed). */
bool ru_wifi_creds_valid(const char *ssid, const char *pass);
ru_cfg_err_t ru_wifi_creds_save(ru_kv_t *kv, const char *ssid, const char *pass);
ru_cfg_err_t ru_wifi_creds_load(ru_kv_t *kv, ru_wifi_creds_t *out);
ru_cfg_err_t ru_wifi_creds_clear(ru_kv_t *kv);
void ru_wifi_creds_wipe(ru_wifi_creds_t *c);
