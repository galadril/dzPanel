/* ru_portal: the SoftAP setup portal as a pure state machine (no IDF, no sockets, no clock of its own).
 * Spec: docs/provisioning.md, DESIGN.md sections 10 and 11. The IDF glue is ru_portal_esp.c.
 *
 * Flow (one phone at a time, server side state, no JavaScript):
 *   GET /            step 1: choose a Wi-Fi network (scan callback) and its password
 *   POST /wifi       join it (ops.wifi_join), then step 2
 *   POST /domoticz   Domoticz address + access token + "I understand" for plain http. Validated, tested and saved by the
 *                    SAME code as the USB path (ru_cfgline_handle: ru_cfg validation, ops.cfg.test = the connection
 *                    test, ru_cfg_save). Then the done page; the portal stops done_grace_ms later.
 *   anything else    302 to http://<ap_ip>/ (captive portal probes: /generate_204, /hotspot-detect.html, /ncsi.txt ...)
 *
 * Security rules, all tested: never runs unless ru_portal_start / ru_portal_start_from_wifi is called; random AP
 * password per session (never logged); the token and the Wi-Fi password are never in any page, log call or reply and
 * their RAM copies are wiped; every dynamic value in a page is HTML-escaped; per-session CSRF token on every POST;
 * one client at a time; failed attempts are rate limited (lock until restart); idle timeout; plain http warning. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ru_captive.h"
#include "ru_cfg.h"
#include "ru_cfgline.h"
#include "ru_http.h"
#include "ru_wifi_sm.h"

#define RU_PORTAL_PAGE_MAX 6144 /* size the response buffer to this */
#define RU_PORTAL_MAX_NETS 16
#define RU_PORTAL_URL_MAX 40
#define RU_PORTAL_CSRF_BYTES 16

typedef struct { char ssid[33]; int8_t rssi; bool secured; } ru_portal_net_t;

typedef enum { RU_PORTAL_SRC_NONE = 0, RU_PORTAL_SRC_WIFI_SM, RU_PORTAL_SRC_USER } ru_portal_src_t;
typedef enum { RU_PORTAL_OFF = 0, RU_PORTAL_STEP_WIFI, RU_PORTAL_STEP_DOMOTICZ, RU_PORTAL_DONE } ru_portal_step_t;
typedef enum { RU_PORTAL_STOP_NONE = 0, RU_PORTAL_STOP_IDLE, RU_PORTAL_STOP_SUCCESS, RU_PORTAL_STOP_USER } ru_portal_stop_t;

typedef struct {
  bool (*random)(void *user, uint8_t *buf, size_t n); /* required */
  /* Strongest first, at most max entries; may be NULL (then only the manual SSID field works). */
  size_t (*scan)(void *user, ru_portal_net_t *out, size_t max);
  /* Join the network (STA) and report whether an IP was obtained, within timeout_ms. Must not log the password. */
  bool (*wifi_join)(void *user, const char *ssid, const char *pass, uint32_t timeout_ms);
  ru_cfgline_ops_t cfg;                  /* .test = connection test (dz_service_cfgline_test), .saved optional */
  void (*log)(void *user, const char *msg); /* optional; fixed strings only, never any user data */
  void *user;                            /* passed to every callback except cfg.* (those use cfg.user) */
} ru_portal_ops_t;

typedef struct { /* zero fields mean the default in brackets */
  uint32_t idle_ms;        /* [600000] stop after this long without a page request or form post */
  uint32_t client_idle_ms; /* [120000] the one client slot is released after this much silence */
  uint32_t done_grace_ms;  /* [15000] stop this long after success so the phone gets the done page */
  uint32_t join_timeout_ms;/* [20000] */
  uint32_t test_timeout_ms;/* [RU_CFGLINE_TIMEOUT_MS] */
  unsigned max_failures;   /* [5] failed form posts, then locked until the portal restarts */
  unsigned pw_len;         /* [12] AP password length, RU_PW_MIN..RU_PW_MAX */
  const char *ap_ip;       /* ["192.168.4.1"] */
} ru_portal_cfg_t;

typedef struct {
  ru_portal_cfg_t cfg;
  ru_portal_ops_t ops;
  ru_kv_t *kv;
  ru_portal_step_t step;
  ru_portal_stop_t last_stop;
  bool locked;
  unsigned failures;
  uint32_t last_activity_ms, done_ms, started_ms;
  uint32_t owner;           /* client slot, 0 = free */
  uint32_t owner_seen_ms;
  char ap_ssid[33];
  char ap_pass[RU_PW_MAX + 1]; /* SECRET */
  char csrf[2 * RU_PORTAL_CSRF_BYTES + 1];
  char joined_ssid[33];
  char url[RU_PORTAL_URL_MAX];
} ru_portal_t;

void ru_portal_init(ru_portal_t *p, const ru_portal_cfg_t *cfg, const ru_portal_ops_t *ops, ru_kv_t *kv);

/* Explicit start. Returns false when already running, the source is NONE, the random source fails or the config is bad.
 * Resets the failure counter (this is what "restart" unlocks). */
bool ru_portal_start(ru_portal_t *p, ru_portal_src_t src, uint32_t now_ms);
/* Start only when the Wi-Fi manager asked for it (state AP_REQUESTED). Otherwise returns false and does nothing. */
bool ru_portal_start_from_wifi(ru_portal_t *p, const ru_wifi_sm_t *sm, uint32_t now_ms);
void ru_portal_stop(ru_portal_t *p, ru_portal_stop_t why);
/* Applies the timeouts; returns true while the portal is still running. Call about once a second. */
bool ru_portal_tick(ru_portal_t *p, uint32_t now_ms);
bool ru_portal_running(const ru_portal_t *p);

/* ---- what a 'Set up this device' screen needs (see docs/progress/portal.md) ---- */
typedef struct {
  bool running;
  ru_portal_step_t step;
  bool locked;
  ru_portal_stop_t last_stop;
  char ssid[33];
  char password[RU_PW_MAX + 1]; /* SECRET: draw it, never log it */
  char url[RU_PORTAL_URL_MAX];  /* http://192.168.4.1/ */
  uint32_t remaining_ms;        /* until the idle timeout */
  unsigned remaining_min;       /* rounded up, 0 when not running */
} ru_portal_status_t;
void ru_portal_status(const ru_portal_t *p, uint32_t now_ms, ru_portal_status_t *out);
bool ru_portal_qr_wifi(const ru_portal_t *p, char *out, size_t cap); /* "WIFI:T:WPA;S:..;P:..;;" */
bool ru_portal_qr_url(const ru_portal_t *p, char *out, size_t cap);  /* the portal URL */

/* ---- one request, already parsed (ru_http or esp_http_server) ---- */
typedef struct {
  ru_http_method_t method;
  const char *path;       /* NUL terminated, no query */
  const char *body;       /* NUL terminated; NULL for GET */
  size_t body_len;
  bool form_ct;           /* Content-Type was application/x-www-form-urlencoded */
  uint32_t client;        /* peer IPv4 as a nonzero integer; 0 is treated as one anonymous client */
} ru_portal_req_t;

typedef struct {
  int status;               /* 200, 302, 303, 400, 403, 405, 413, 415, 429, 500, 503 */
  char location[RU_PORTAL_URL_MAX]; /* set for 302/303 */
  char *body;               /* caller buffer, NUL terminated */
  size_t body_cap, body_len;
  bool html;                /* false: no body */
} ru_portal_resp_t;

/* Security headers every response must carry (the glue adds them; they are also what the tests expect). */
#define RU_PORTAL_CSP "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'"
#define RU_PORTAL_CACHE "no-store"

/* Handles one request. Always fills resp (never leaves it undefined). Returns false when the portal is not running. */
bool ru_portal_handle(ru_portal_t *p, const ru_portal_req_t *rq, uint32_t now_ms, ru_portal_resp_t *resp);
