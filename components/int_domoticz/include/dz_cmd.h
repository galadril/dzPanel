/* Command layer: validates a command against the model, sends it, then re-reads the device
 * (read-after-write) and reports CONFIRMED / UNCONFIRMED / FAILED. The HTTP "OK" is never trusted.
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
  DZ_CMD_SET_LEVEL,                          /* dimmers: level in device units 0..MaxDimLevel, clamped */
  DZ_CMD_BLINDS_OPEN, DZ_CMD_BLINDS_CLOSE, DZ_CMD_BLINDS_STOP,
  DZ_CMD_SELECTOR,                           /* level = LevelInt (index * 10), must name an existing level */
  DZ_CMD_SCENE_ON, DZ_CMD_SCENE_OFF,         /* scenes and groups (a Scene has no Off) */
  DZ_CMD_SETPOINT,                           /* setpoint in the server's own unit (TempSign or vunit), clamped to min..max */
  DZ_CMD_PUSH,                               /* push buttons */
  DZ_CMD_MODE                                /* thermostat / fan mode: level = a Mode index from Modes. UNVERIFIED: never
                                                exercised against a live server (OpenZWave only) */
} dz_cmd_kind_t;

typedef struct {
  dz_cmd_kind_t kind;
  const char *idx;     /* device idx, or scene idx for SCENE_*: separate namespaces */
  int level;
  float setpoint;
  bool confirmed;      /* the caller obtained the confirmation required by dz_cmd_risk */
} dz_cmd_req_t;

typedef enum {
  DZ_CMD_ACCEPTED = 0,
  DZ_CMDE_CONTROL_DISABLED,  /* client configured read-only (viewer token) */
  DZ_CMDE_UNKNOWN_DEVICE,
  DZ_CMDE_PROTECTED,
  DZ_CMDE_NOT_CONTROLLABLE,  /* capability cannot do this command */
  DZ_CMDE_BAD_ARG,           /* missing idx, level not valid for a selector, non-finite setpoint... */
  DZ_CMDE_NEEDS_CONFIRM,
  DZ_CMDE_STALE,             /* model data for the device is not fresh: refuse rather than guess */
  DZ_CMDE_QUEUE_FULL
} dz_cmd_err_t;
const char *dz_cmd_err_name(dz_cmd_err_t e);

typedef enum { DZ_OUT_CONFIRMED, DZ_OUT_UNCONFIRMED, DZ_OUT_FAILED } dz_outcome_t;
typedef enum {
  DZ_FAIL_NONE,
  DZ_FAIL_UNAUTHORIZED,  /* 401: token rejected (polling is stopped too) */
  DZ_FAIL_FORBIDDEN,     /* 403: token lacks switch rights (viewer) */
  DZ_FAIL_SERVER,        /* HTTP 5xx or "status" != OK */
  DZ_FAIL_TRANSPORT,     /* could not reach the server */
  DZ_FAIL_BAD_RESPONSE   /* HTML page, unexpected HTTP status */
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
} dz_cmd_result_t;

typedef void (*dz_cmd_cb)(const dz_cmd_result_t *r, void *user);

#define DZ_CMD_QUEUE 8

typedef struct {
  uint32_t id;
  dz_cmd_req_t req;
  char idx[12];
  int level;
  dz_device_t pre;
  int state; /* 0 free, 1 queued, 2 waiting verify */
  uint32_t due_ms;
  int reads;
  bool sent_ok, uncertain;
  dz_fail_t fail;
} dz_cmd_slot_t;

typedef struct {
  dz_client_t *client;
  dz_model_t *model;
  bool allow_control;
  uint32_t verify_delay_ms;  /* default 1500; a second read follows after the same delay */
  dz_cmd_cb cb;
  void *user;
  uint32_t next_id;
  dz_cmd_slot_t q[DZ_CMD_QUEUE];
} dz_cmd_t;

void dz_cmd_init(dz_cmd_t *c, dz_client_t *client, dz_model_t *model, bool allow_control, dz_cmd_cb cb, void *user);

/* Risk of a command on a device, so the UI can decide whether to demand a hold before submitting. */
dz_risk_t dz_cmd_risk(const dz_device_t *d, dz_cmd_kind_t kind);

/* The command a tap should send for a scene or group: a Scene is always pushed (On), a Group switches Off when it is
 * fully On and On otherwise (Off or Mixed). Returns false if d is neither. */
bool dz_cmd_scene_tap_kind(const dz_device_t *d, dz_cmd_kind_t *kind);

/* Validate and queue. On DZ_CMD_ACCEPTED *id is the request id; the result arrives via the callback. */
dz_cmd_err_t dz_cmd_submit(dz_cmd_t *c, const dz_cmd_req_t *req, uint32_t now_ms, uint32_t *id);

/* Does at most one HTTP request. Returns true if it did. */
bool dz_cmd_pump(dz_cmd_t *c, uint32_t now_ms);
size_t dz_cmd_pending(const dz_cmd_t *c);
uint32_t dz_cmd_ms_until_due(const dz_cmd_t *c, uint32_t now_ms); /* UINT32_MAX when idle */

/* Percent-encodes everything outside [A-Za-z0-9-._~]. Returns length, or -1 when it does not fit. */
int dz_url_encode(char *dst, size_t cap, const char *src);
