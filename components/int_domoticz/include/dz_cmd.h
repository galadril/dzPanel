/* Command layer: validates a command against the model, re-reads the device (pre-flight: Protected devices are refused here
 * because the MCP endpoint skips Domoticz's own check), sends it through the Domoticz MCP endpoint (dz_client_mcp_call; there
 * is NO legacy json.htm fallback for commands: it returns 403 for User-rights access tokens), then re-reads the device
 * (read-after-write) and reports CONFIRMED / UNCONFIRMED / FAILED. MCP results are text only: the reply is never trusted
 * beyond "it did not say Error".
 * Driven by dz_cmd_pump from the same task as dz_client_step (they share the client buffer). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "dz_client.h"
#include "dz_model.h"

typedef enum {
  DZ_RISK_SAFE,     /* one tap */
  DZ_RISK_CONFIRM,  /* caller must have obtained a visible hold / confirmation: set req.confirmed */
  DZ_RISK_LOCKED    /* never sent: protected (needs a passcode the device never stores) or not controllable */
} dz_risk_t;

typedef enum {
  DZ_CMD_ON, DZ_CMD_OFF, DZ_CMD_TOGGLE,     /* on/off, dimmers, locks (On = lock, Off = unlock), scenes use SCENE_* */
  DZ_CMD_SET_LEVEL,                          /* dimmers: level in device units 0..MaxDimLevel, clamped; sent to the MCP tool as a rounded percentage */
  DZ_CMD_BLINDS_OPEN, DZ_CMD_BLINDS_CLOSE, DZ_CMD_BLINDS_STOP,
  DZ_CMD_SELECTOR,                           /* level = LevelInt (index * 10), must name an existing level */
  DZ_CMD_SCENE_ON, DZ_CMD_SCENE_OFF,         /* scenes and groups (a Scene has no Off) */
  DZ_CMD_SETPOINT,                           /* setpoint in the server's own unit (TempSign or vunit), clamped to min..max */
  DZ_CMD_PUSH,                               /* push buttons */
  DZ_CMD_COLOR_HUE,                          /* colour lights: hue 0..359 (full saturation: Domoticz hsb2rgb(h, 1, 1)), level = brightness 1..100 (0 refused) */
  DZ_CMD_COLOR_WHITE,                        /* RGBW lights: the lamp's own white (hue 0, iswhite), level = brightness 1..100 */
  DZ_CMD_COLOR_TEMP,                         /* tunable white: level = warmth 0..100 (0 coolest 6500 K, 100 warmest 2700 K) */
  DZ_CMD_MODE                                /* thermostat / fan mode: level = a Mode index from Modes. VIEW ONLY: the Domoticz MCP
                                                endpoint has no tool for it, so dz_cmd_supported is false and submit refuses */
} dz_cmd_kind_t;

typedef struct {
  dz_cmd_kind_t kind;
  const char *idx;     /* device idx, or scene idx for SCENE_*: separate namespaces */
  int level;
  float setpoint;
  int16_t hue;         /* DZ_CMD_COLOR_HUE: 0..359 */
  bool confirmed;      /* the caller obtained the confirmation required by dz_cmd_risk */
} dz_cmd_req_t;

/* What a colour or temperature command does for a lamp that is OFF. VERIFIED on a Philips Hue bridge behind Domoticz 2026.4
 * (build 18605), three lamps: set_color_brightness and set_color_temperature on an Off lamp switch it ON at its stored level.
 * 1 = the server switches the lamp on: the UI sends nothing extra and the verify read requires the lamp to be On (an Off lamp that
 * stays Off is UNCONFIRMED). 0 = (other bridges) the UI follows the colour command with an On and the lamp is not required to be On.
 * ONE constant so it can be flipped. A stored level of 0 or unknown makes the UI send brightness 100. */
#ifndef DZ_COLOR_SWITCHES_ON_LAMP
#define DZ_COLOR_SWITCHES_ON_LAMP 1
#endif

typedef enum {
  DZ_CMD_ACCEPTED = 0,
  DZ_CMDE_CONTROL_DISABLED,  /* client configured read-only (viewer token) */
  DZ_CMDE_UNKNOWN_DEVICE,
  DZ_CMDE_PROTECTED,
  DZ_CMDE_NOT_CONTROLLABLE,  /* capability cannot do this command */
  DZ_CMDE_BAD_ARG,           /* missing idx, level not valid for a selector, non-finite setpoint... */
  DZ_CMDE_NEEDS_CONFIRM,
  DZ_CMDE_STALE,             /* model data for the device is not fresh: refuse rather than guess */
  DZ_CMDE_QUEUE_FULL,
  DZ_CMDE_UNSUPPORTED        /* no MCP tool for it: thermostat MODE, SETPOINT on a Fahrenheit server (shown read-only) */
} dz_cmd_err_t;
const char *dz_cmd_err_name(dz_cmd_err_t e);

/* DZ_OUT_NO_ACTION: only for PUSH. The server said "No action taken" (the button already reads the state the call asked for, so the
 * tool skipped it) and the re-read shows no new LastUpdate: the button did NOT fire. Shown as "ALREADY PRESSED", not "NOT CONFIRMED". */
typedef enum { DZ_OUT_CONFIRMED, DZ_OUT_UNCONFIRMED, DZ_OUT_FAILED, DZ_OUT_NO_ACTION } dz_outcome_t;

/* How a PUSH button is fired through MCP. Which call really forces a push (set_switch_state On skips when the state already matches)
 * is decided by a live test; the choice lives in ONE small function (push_call in dz_cmd.c) selected by this constant. */
typedef enum {
  DZ_PUSH_SET_STATE,    /* set_switch_state with the type's own word (On, Off for a Push Off button) */
  DZ_PUSH_TOGGLE,       /* toggle_switch_state */
  DZ_PUSH_OFF_THEN_ON   /* set_switch_state with the opposite word, then the type's own word (two calls) */
} dz_push_mode_t;
#ifndef DZ_PUSH_MODE_DEFAULT
#define DZ_PUSH_MODE_DEFAULT DZ_PUSH_SET_STATE
#endif
typedef enum {
  DZ_FAIL_NONE,
  DZ_FAIL_UNAUTHORIZED,  /* 401: token rejected (polling is stopped too) */
  DZ_FAIL_FORBIDDEN,     /* 403: token lacks switch rights (viewer) */
  DZ_FAIL_SERVER,        /* HTTP 5xx or "status" != OK */
  DZ_FAIL_TRANSPORT,     /* could not reach the server */
  DZ_FAIL_BAD_RESPONSE,  /* HTML page, unexpected HTTP status, session error after the retry, JSON-RPC parse/request error */
  DZ_FAIL_NO_MCP,        /* the server has no MCP endpoint (HTTP 404 / 405 / 503, or JSON-RPC -32601): started with -nomcp or too old */
  DZ_FAIL_REJECTED,      /* isError true or -32602: the device / scene is gone or the arguments were refused */
  DZ_FAIL_PROTECTED      /* the pre-flight re-read shows the device is Protected: MCP skips Domoticz's own check, so we refuse */
} dz_fail_t;
const char *dz_outcome_name(dz_outcome_t o);

typedef struct {
  uint32_t id;
  dz_cmd_kind_t kind;
  char idx[12];
  dz_outcome_t outcome;
  dz_fail_t fail;
  int level_sent;           /* after clamping, -1 if not a level command */
  bool have_after;
  dz_device_t after;        /* the re-read state, when we got one */
  bool sent;                /* the MCP call was answered and reported no error */
  bool uncertain;           /* the call may or may not have run (transport error after the send): the re-read decided */
  bool no_action;           /* the server answered "No action taken" */
} dz_cmd_result_t;

typedef void (*dz_cmd_cb)(const dz_cmd_result_t *r, void *user);

#define DZ_CMD_QUEUE 8

typedef struct {
  uint32_t id;
  dz_cmd_req_t req;
  char idx[12];
  int level;
  dz_device_t pre;
  int state; /* 0 free, 1 queued (pre-flight read due), 2 waiting verify, 3 pre-flight done (MCP call due) */
  uint32_t due_ms;
  int reads;
  bool sent_ok, uncertain;
  bool no_action;            /* the server said "already ... No action taken" */
  int push_step;             /* PUSH: which call of the sequence is next (DZ_PUSH_OFF_THEN_ON has two) */
  dz_fail_t fail;
} dz_cmd_slot_t;

#define DZ_CMD_SENT 8
#define DZ_CMD_MIN_INTERVAL_MS 300 /* between two sends of light commands (level, colour, on/off, toggle) to one device */
typedef struct { char idx[12]; uint32_t t; bool used; } dz_cmd_sent_t;

typedef struct {
  dz_client_t *client;
  dz_model_t *model;
  bool allow_control;
  uint32_t min_interval_ms;  /* per-device spacing of light-command sends; 0 = off (tests) */
  dz_cmd_sent_t sent[DZ_CMD_SENT];
  uint32_t verify_delay_ms;  /* default 1500; a second read follows after the same delay */
  dz_push_mode_t push_mode;  /* default DZ_PUSH_MODE_DEFAULT; the live test may set another */
  dz_cmd_cb cb;
  void *user;
  uint32_t next_id;
  dz_cmd_slot_t q[DZ_CMD_QUEUE];
} dz_cmd_t;

void dz_cmd_init(dz_cmd_t *c, dz_client_t *client, dz_model_t *model, bool allow_control, dz_cmd_cb cb, void *user);

/* Can this device take this command through the MCP tools at all? false for MODE (no tool), SETPOINT when the device's value
 * unit is Fahrenheit or the SERVER temperature unit is not known to be Celsius (u NULL or u->have_temp false: the setpoint tool
 * takes Celsius only), and a capability that does not take the command. The UI renders such controls read-only. u comes from
 * dz_model_units; it is only looked at for SETPOINT. */
bool dz_cmd_supported(const dz_device_t *d, dz_cmd_kind_t kind, const dz_units_t *u);

/* Risk of a command on a device, so the UI can decide whether to demand a hold before submitting. */
dz_risk_t dz_cmd_risk(const dz_device_t *d, dz_cmd_kind_t kind, const dz_units_t *u);

/* The command a tap should send for a scene or group: a Scene is always pushed (On), a Group switches Off when it is
 * fully On and On otherwise (Off or Mixed). Returns false if d is neither. */
bool dz_cmd_scene_tap_kind(const dz_device_t *d, dz_cmd_kind_t *kind);

/* Validate and queue. On DZ_CMD_ACCEPTED *id is the request id; the result arrives via the callback. */
dz_cmd_err_t dz_cmd_submit(dz_cmd_t *c, const dz_cmd_req_t *req, uint32_t now_ms, uint32_t *id);

/* Does one step of one command: the pre-flight GET, the MCP call (initialize + call when there is no session yet) or a verify GET.
 * Returns true if it did. */
bool dz_cmd_pump(dz_cmd_t *c, uint32_t now_ms);
size_t dz_cmd_pending(const dz_cmd_t *c);
uint32_t dz_cmd_ms_until_due(const dz_cmd_t *c, uint32_t now_ms); /* UINT32_MAX when idle */

/* The Domoticz MCP tools/call body for a command (see dz_cmd.c). `pre` is the device as last read (lock type, push type).
 * Returns the length, or 0 if the command has no MCP tool (MODE), idx is not a number, a level exceeds 100, or cap is too small. */
/* The body of step `step` (0-based) of a PUSH in `mode` (see dz_push_mode_t). */
size_t dz_mcp_encode_push(char *out, size_t cap, uint32_t rpc_id, const char *idx, const dz_device_t *pre, dz_push_mode_t mode, int step);
size_t dz_cmd_push_steps(dz_push_mode_t mode); /* calls in a push sequence */
size_t dz_mcp_encode(char *out, size_t cap, uint32_t rpc_id, dz_cmd_kind_t kind, const char *idx, const dz_device_t *pre,
                     int level, float setpoint);
/* Same, with the hue of DZ_CMD_COLOR_HUE (0..359). */
size_t dz_mcp_encode_hue(char *out, size_t cap, uint32_t rpc_id, dz_cmd_kind_t kind, const char *idx, const dz_device_t *pre,
                         int level, int hue, float setpoint);
/* The commands of the same family replace a queued one for the same device (see dz_cmd_submit): level (SET_LEVEL) or colour (HUE, WHITE, TEMP). */
bool dz_cmd_is_colour(dz_cmd_kind_t k);
