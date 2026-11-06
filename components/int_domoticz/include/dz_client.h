/* Domoticz client: configuration, request helper and a polling state machine.
 *
 * Threading: one task owns the client. dz_client_step and dz_cmd_pump (dz_cmd.h) both use the
 * client's single buffer and must be called from that task. Readers use dz_model.h from any task.
 * The token is held in the client, sent only as an Authorization header, and never logged. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dz_model.h"
#include "dz_parse.h"
#include "dz_transport.h"

typedef enum {
  DZ_CS_CONNECTING,      /* no successful poll yet (or retrying after a reset) */
  DZ_CS_OK,
  DZ_CS_OFFLINE,         /* transport failures (timeout, connect, TLS) for several attempts in a row */
  DZ_CS_TOKEN_REJECTED,  /* 401/403 on a poll: polling STOPS until dz_client_set_token / dz_client_retry */
  DZ_CS_SERVER_ERROR,    /* 5xx, or JSON with "status" != OK */
  DZ_CS_BAD_RESPONSE     /* not Domoticz JSON: HTML proxy page, truncated, wrong URL (404), too large */
} dz_conn_state_t;

typedef enum {
  DZ_CERR_NONE,
  DZ_CERR_TIMEOUT,
  DZ_CERR_CONNECT,
  DZ_CERR_TLS,
  DZ_CERR_TOO_LARGE,  /* response exceeded the buffer: use favourites only or a bigger buffer */
  DZ_CERR_HTTP,       /* see last_http_status */
  DZ_CERR_PARSE,
  DZ_CERR_STATUS      /* JSON "status" was not OK */
} dz_client_err_t;

typedef struct {
  bool valid;
  int year, minor, build; /* "2026.4 (build 18568)" -> 2026, 4, 18568 */
} dz_version_t;
/* Lenient: ignores leading text ("v"), a missing build, trailing text. */
bool dz_version_parse(const char *s, dz_version_t *out);
bool dz_version_at_least(const dz_version_t *v, int year, int minor);

typedef struct { char idx[12]; char name[32]; } dz_plan_t;
#define DZ_MAX_PLANS 16

typedef struct {
  const char *base_url;    /* "http://host:8080" or "https://host", no trailing path */
  const char *token;       /* bearer token, may be NULL/empty when the server trusts the network */
  dz_transport_t *transport;
  dz_model_t *model;
  char *buf;               /* response buffer, owned by the caller */
  size_t buf_cap;
  bool favorites_only;     /* Domoticz favourites are the watch list */
  uint32_t poll_interval_ms;   /* default 10000, floor 1000 */
  uint32_t startup_gap_ms;     /* gap between the initial requests, default 2000, floor 1000 */
  uint32_t full_refresh_ms;    /* full re-read period, default 600000 */
  uint32_t timeout_ms;         /* per request, default 8000 */
  uint32_t backoff_max_ms;     /* default 300000 */
  unsigned scenes_every;       /* read scenes every N delta polls, default 3 */
  void (*on_state)(void *user, dz_conn_state_t from, dz_conn_state_t to);
  void *user;
} dz_client_cfg_t;

#define DZ_TOKEN_MAX 768
#define DZ_URL_MAX 192

typedef struct {
  dz_client_cfg_t cfg;
  char base[DZ_URL_MAX];
  char token[DZ_TOKEN_MAX];
  dz_conn_state_t state;
  dz_client_err_t last_err;
  int last_http_status;
  unsigned failures;           /* consecutive */
  uint32_t next_due_ms;
  bool halted;
  int phase;
  bool ever_ok;
  uint32_t last_full_ms;
  unsigned delta_count;
  int64_t cursor;              /* ActTime of the last good device response, 0 = none */
  dz_version_t version;
  char sunrise[24], sunset[24];
  dz_plan_t plans[DZ_MAX_PLANS];
  size_t n_plans;
  uint32_t requests;           /* cumulative HTTP requests made */
  /* MCP control path (see dz_client_mcp_call). The session id is a credential like the token: never logged, wiped with it. */
  char mcp_sid[65];            /* "" = no session */
  uint32_t mcp_last_ms;        /* last successful use, to retire a session idle for more than 55 minutes */
  uint32_t mcp_rpc_id;         /* JSON-RPC id counter */
  char mcp_body[256];          /* initialize / notification body (kept off the task stack) */
  char mcp_call[256];          /* tools/call body: encode into it directly, pass it to dz_client_mcp_call */
} dz_client_t;

bool dz_client_init(dz_client_t *c, const dz_client_cfg_t *cfg);
/* Host helper: copies DOMOTICZ_ACCESSTOKEN into out. false when unset or too long. Never prints it. */
bool dz_client_token_from_env(char *out, size_t cap);

/* Makes at most one request if one is due. Returns true if it did. Never blocks longer than timeout_ms. */
bool dz_client_step(dz_client_t *c, uint32_t now_ms);
uint32_t dz_client_ms_until_due(const dz_client_t *c, uint32_t now_ms); /* UINT32_MAX when halted */

dz_conn_state_t dz_client_state(const dz_client_t *c);
const char *dz_conn_state_name(dz_conn_state_t s);
const char *dz_client_err_name(dz_client_err_t e);

/* Replace the token and leave TOKEN_REJECTED; next step polls immediately. NULL keeps the old one. */
void dz_client_set_token(dz_client_t *c, const char *token);
void dz_client_retry_now(dz_client_t *c, uint32_t now_ms);
void dz_client_set_poll_interval(dz_client_t *c, uint32_t ms); /* foreground 10 s / glance 30 s / dimmed 120 s */

/* getconfig body -> display units. Only TempSign, TempScale, WindSign, WindScale and CurrencySign are read; everything
 * else in that document (it includes the user's coordinates) is ignored. false when none of the five was usable. */
bool dz_units_parse(const char *json, size_t len, dz_units_t *out);

/* One GET of /json.htm?type=command&<query>. Body is left in cfg.buf (len in r). Used by dz_cmd. */
dz_net_err_t dz_client_request(dz_client_t *c, const char *query, dz_http_result_t *r);
/* ---- MCP: POST <base>/mcp, JSON-RPC 2.0 (Domoticz MCP endpoint; the control path). Same single-task rule and the same buffer
 * as dz_client_request; the reply is left in cfg.buf. The result is text only (dz_mcp_reply_t). ---- */
typedef enum { DZ_MCP_STAGE_INIT, DZ_MCP_STAGE_CALL } dz_mcp_stage_t;
typedef struct {
  dz_net_err_t net;        /* transport result of the LAST request made (initialize or the call) */
  int http;                /* its HTTP status, 0 if none */
  dz_mcp_stage_t stage;    /* which request that was: a timeout in INIT is a plain transport failure, in CALL it is uncertain */
  dz_err_t parse;          /* dz_mcp_parse_reply of its body (DZ_OK when a JSON-RPC reply came back) */
  dz_mcp_reply_t reply;
  bool reinit;             /* an expired / missing session was replaced and the call retried ONCE */
  bool session_lost;       /* still 'Missing session' / 'Session not found' after that retry */
} dz_mcp_result_t;

uint32_t dz_client_mcp_next_id(dz_client_t *c);
/* Makes sure a session exists (initialize + notifications/initialized when there is none, it is older than 55 minutes idle, or
 * force). true when a session id is held afterwards; otherwise *res says why (res->stage == INIT). */
bool dz_client_mcp_open(dz_client_t *c, uint32_t now_ms, bool force, dz_mcp_result_t *res);
/* tools/call (or any request) with `body` (used from c->mcp_call, copied there unless it already is; at most 255 bytes). Opens a session lazily and recovers from
 * 'Missing session' / 'Session not found' with ONE re-initialize and ONE retry. A transport error or timeout on the call itself
 * is NEVER retried (it may have run). HTTP 401 wipes the session. Returns true when a well-formed JSON-RPC reply (result or
 * error) arrived with HTTP 200 in res->reply; everything else is in res. */
bool dz_client_mcp_call(dz_client_t *c, uint32_t now_ms, const char *body, dz_mcp_result_t *res);
/* Forget the session (wiped). */
void dz_client_mcp_reset(dz_client_t *c);

/* Maps a transport result to a terminal client reaction for a request that was not a poll (401 stops polling). */
void dz_client_note_auth_failure(dz_client_t *c);
