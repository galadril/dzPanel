/* Domoticz service: ONE network task body that owns the client, the model and the command layer.
 *
 *   UI / other tasks                        service task (dz_service_task, or run_once in tests)
 *   ---------------------                   -------------------------------------------------
 *   dz_service_configure()  --pending-->    rebuild client from the new url/token, cmd layer re-init
 *   dz_service_submit()     --queue----->   dz_cmd_submit; dz_cmd_pump (read-after-write); results -> bus
 *   dz_service_set_net_up() / retry() ->    pause polling while Wi-Fi is down / leave TOKEN_REJECTED
 *   reads: dz_model_* (any task)  <------   dz_client_step commits into the model (internal mutex)
 *
 * Everything that touches dz_client_t, dz_cmd_t or the response buffer happens on the service task, as dz_client.h
 * demands. The only cross-task state is a small mutex-protected mailbox (pending config, command queue, flags).
 *
 * Events on ru_bus (topic = cfg.topic, coalescing is switched on for it; see the key rules below):
 *   DZ_EV_DEVICE_CHANGED  payload dz_ev_device_t   key = DZ_EV_KEY_DEVICE(idx, scene)   a device/scene entry changed
 *   DZ_EV_MODEL_CHANGED   payload dz_ev_model_t    key = DZ_EV_KEY_MODEL                  generation moved (removals too)
 *   DZ_EV_CONN_STATE      payload dz_ev_conn_t     key = DZ_EV_KEY_CONN                   dz_conn_state_t changed
 *   DZ_EV_CMD_RESULT      payload dz_ev_cmd_t      key = 0 (never coalesced)              one per submitted command
 *   DZ_EV_AUTH_REJECTED   payload dz_ev_auth_t     key = DZ_EV_KEY_AUTH                   401/403: polling has stopped
 * Payloads are memcpy'd structs of at most RU_BUS_PAYLOAD bytes.
 *
 * Rules honoured here: poll interval and backoff come from dz_client; TOKEN_REJECTED stops polling until a new
 * config or dz_service_retry(); the response buffer must be >= 128 KB (a full device list on a real server is about
 * 75 KB) unless favorites_only is set (then >= 16 KB); Scenes are push-only and Groups toggle with Mixed state
 * (dz_cmd decides: submit DZ_CMD_SCENE_ON/OFF with the idx from the scene namespace, dz_cmd_scene_tap_kind picks the
 * kind); the token only ever lives in the client's own buffer, a pending mailbox slot (wiped after use) and the HTTP
 * header; it is never logged and no function here returns it. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dz_client.h"
#include "dz_cmd.h"
#include "dz_model.h"
#include "ru_bus.h"
#include "ru_cfg.h"
#include "ru_osal.h"

#define DZ_SERVICE_MIN_BUF_FULL (128u * 1024u)
#define DZ_SERVICE_MIN_BUF_FAVS (16u * 1024u)
#define DZ_SERVICE_TOPIC_DEFAULT 8 /* bus topic id used when cfg.topic == 0; ask the shell owner to reserve it */
#define DZ_SERVICE_MAX_SLEEP_MS 30000u
#define DZ_SERVICE_SUBMIT_QUEUE 8

enum { DZ_EV_DEVICE_CHANGED = 1, DZ_EV_MODEL_CHANGED, DZ_EV_CONN_STATE, DZ_EV_CMD_RESULT, DZ_EV_AUTH_REJECTED };
enum { DZ_EV_KEY_MODEL = 1, DZ_EV_KEY_CONN = 2, DZ_EV_KEY_AUTH = 3 };
uint32_t dz_ev_key_device(const char *idx, bool scene); /* never collides with the fixed keys, never 0 */

typedef struct { char idx[12]; uint8_t is_scene; } dz_ev_device_t;
typedef struct { uint32_t generation; uint32_t count; } dz_ev_model_t;
typedef struct { uint8_t state; uint8_t prev; uint8_t err; uint8_t pad; int16_t http_status; } dz_ev_conn_t; /* dz_conn_state_t, dz_client_err_t */
typedef struct {
  uint32_t req_id;   /* the id dz_service_submit returned */
  uint8_t outcome;   /* dz_outcome_t; meaningful when reject == 0 */
  uint8_t fail;      /* dz_fail_t */
  uint8_t kind;      /* dz_cmd_kind_t */
  uint8_t reject;    /* dz_cmd_err_t: non-zero = refused before anything was sent (outcome is DZ_OUT_FAILED) */
  int16_t level;     /* level sent, -1 if none */
} dz_ev_cmd_t;
typedef struct { int16_t http_status; } dz_ev_auth_t;

typedef struct {
  dz_transport_t *transport;      /* required; get() must tolerate being called from this task only */
  /* Optional policy hook, see dz_transport_esp.h: tells the transport whether plain http is allowed (insecure_lan). */
  void (*set_insecure_lan)(dz_transport_t *t, bool allow);
  ru_bus_t *bus;                  /* NULL = no events */
  uint16_t topic;                 /* 0 = DZ_SERVICE_TOPIC_DEFAULT */
  char *buf;                      /* response buffer, caller owned (PSRAM on the device) */
  size_t buf_cap;
  dz_entry_t *entries; size_t entries_cap;   /* model storage, caller owned */
  dz_device_t *stage; size_t stage_cap;
  dz_model_cfg_t model_cfg;
  bool favorites_only;
  uint32_t poll_interval_ms;      /* 0 = client default (10 s) */
  uint32_t timeout_ms;            /* per request, 0 = client default */
} dz_service_cfg_t;

typedef struct {
  dz_service_cfg_t cfg;
  dz_model_t model;
  dz_client_t client;
  dz_cmd_t cmd;
  bool client_ready;
  bool allow_control;
  /* mailbox, guarded by mu */
  ru_mutex_t *mu;
  ru_queue_t *wake;
  struct { bool pending; ru_cfg_t cfg; bool allow_control; } mb_cfg;
  bool mb_clear, mb_retry, mb_poll_set, net_up, stop;
  uint32_t mb_poll_ms;
  struct { uint32_t id; dz_cmd_req_t req; char idx[12]; } sq[DZ_SERVICE_SUBMIT_QUEUE];
  size_t sq_n;
  uint32_t next_req_id;
  /* task-only */
  struct { uint32_t svc_id, cmd_id; bool used; } idmap[DZ_CMD_QUEUE];
  uint32_t last_posted_gen;
  uint32_t now_ms;
  uint32_t requests_seen;
  uint32_t events_posted, events_failed;
  volatile uint8_t conn_state; /* mirror of client.state for other tasks */
  ru_task_t *task;
} dz_service_t;

/* false on bad config: transport/model storage missing, buffer too small for the mode (see above). */
bool dz_service_init(dz_service_t *s, const dz_service_cfg_t *cfg);
void dz_service_deinit(dz_service_t *s); /* stops the task if running, wipes secrets */

/* Thread safe. Validates (ru_cfg_validate: normalised url, http needs insecure_lan, token rules) and queues the new
 * configuration; the task rebuilds the client (leaving TOKEN_REJECTED). Returns RU_CFG_OK or the validation error.
 * The caller should wipe its copy. allow_control: false for read-only (viewer) installs. */
ru_cfg_err_t dz_service_configure(dz_service_t *s, const ru_cfg_t *cfg, bool allow_control);
void dz_service_clear_config(dz_service_t *s); /* stop polling and forget url/token (factory reset) */
void dz_service_set_net_up(dz_service_t *s, bool up); /* Wi-Fi state; polling pauses while false */
void dz_service_set_poll_interval(dz_service_t *s, uint32_t ms); /* foreground 10 s / glance 30 s / dimmed 120 s */
void dz_service_retry(dz_service_t *s); /* leave TOKEN_REJECTED / backoff now */

/* Thread safe. Queues a command (idx is copied). Returns a request id (>0); the verdict, including refusals such as
 * protected, stale or needs-confirm, arrives as DZ_EV_CMD_RESULT. Returns 0 when the queue is full. */
uint32_t dz_service_submit(dz_service_t *s, const dz_cmd_req_t *req);

/* One scheduling round at now_ms: applies the mailbox, makes at most one HTTP request, posts events. Returns how long
 * the caller may sleep (ms) before the next round unless woken. Host tests drive this directly with a fake clock. */
uint32_t dz_service_run_once(dz_service_t *s, uint32_t now_ms);

/* The task body: loops run_once, sleeping on the wake queue. Create with dz_service_start. */
void dz_service_task(void *arg);
bool dz_service_start(dz_service_t *s);
void dz_service_stop(dz_service_t *s); /* joins */

dz_model_t *dz_service_model(dz_service_t *s);
dz_conn_state_t dz_service_conn_state(dz_service_t *s); /* task-safe snapshot */

/* Connection test for the RU+CFG handler: getversion with the candidate token. Uses the given transport (a dedicated
 * one: not the polling transport) and its own small buffer. set_insecure may be NULL. timeout_ms bounds the request.
 * Matches ru_cfgline_ops_t.test when wrapped (see dz_service_cfgline_test). */
ru_cfg_test_t dz_service_probe(dz_transport_t *t, void (*set_insecure)(dz_transport_t *, bool), const ru_cfg_t *cfg,
                               uint32_t timeout_ms, char *info, size_t info_cap);
typedef struct { dz_transport_t *transport; void (*set_insecure)(dz_transport_t *, bool); bool (*wifi_up)(void *); void *user; } dz_probe_ctx_t;
/* Adapter with the ru_cfgline_ops_t.test signature; user = dz_probe_ctx_t*. Returns NO_WIFI when wifi_up says so. */
ru_cfg_test_t dz_service_cfgline_test(void *user, const ru_cfg_t *cfg, uint32_t timeout_ms, char *info, size_t info_cap);
