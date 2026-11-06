#include "ru_fb.h"

#include <stdio.h>
#include <string.h>

static void set(ru_fb_t *f, ru_fb_state_t st, const char *text, uint32_t now) {
  f->st = st;
  f->since_ms = now;
  snprintf(f->text, sizeof f->text, "%s", text);
}

void ru_fb_init(ru_fb_t *f) { memset(f, 0, sizeof *f); }

void ru_fb_begin(ru_fb_t *f, const char *idx, bool scene, uint32_t now_ms) {
  snprintf(f->idx, sizeof f->idx, "%s", idx ? idx : "");
  f->scene = scene;
  set(f, RU_FB_PENDING, "SENDING...", now_ms);
}

static const char *fail_text(dz_fail_t x) {
  switch (x) {
    case DZ_FAIL_FORBIDDEN: return "NO SWITCH RIGHTS";
    case DZ_FAIL_UNAUTHORIZED: return "TOKEN REJECTED";
    case DZ_FAIL_TRANSPORT: return "NO CONNECTION";
    case DZ_FAIL_SERVER: return "SERVER ERROR";
    case DZ_FAIL_BAD_RESPONSE: return "BAD RESPONSE";
    case DZ_FAIL_NO_MCP: return "MCP NOT AVAIL"; /* the label buffer is 32 chars; kept short */
    case DZ_FAIL_REJECTED: return "REJECTED";
    case DZ_FAIL_PROTECTED: return "PROTECTED";
    default: return "FAILED";
  }
}

void ru_fb_on_result(ru_fb_t *f, const dz_cmd_result_t *r, uint32_t now_ms) {
  if (r->idx[0]) {
    snprintf(f->idx, sizeof f->idx, "%s", r->idx);
    f->scene = r->kind == DZ_CMD_SCENE_ON || r->kind == DZ_CMD_SCENE_OFF;
  }
  switch (r->outcome) {
    case DZ_OUT_CONFIRMED: set(f, RU_FB_CONFIRMED, "DONE", now_ms); break;
    case DZ_OUT_UNCONFIRMED: {
      /* a Group that answers Mixed after we asked for On/Off says so */
      bool mixed = r->have_after && dz_device_state(&r->after) == DZ_STATE_MIXED;
      set(f, RU_FB_UNCONFIRMED, mixed ? "STILL MIXED" : "NOT CONFIRMED", now_ms);
      break;
    }
    case DZ_OUT_NO_ACTION: set(f, RU_FB_UNCONFIRMED, "ALREADY PRESSED", now_ms); break; /* the server skipped the push */
    default: set(f, RU_FB_FAILED, fail_text(r->fail), now_ms); break;
  }
}

void ru_fb_on_refused(ru_fb_t *f, dz_cmd_err_t e, uint32_t now_ms) {
  const char *t;
  switch (e) {
    case DZ_CMDE_CONTROL_DISABLED: t = "VIEW ONLY"; break;
    case DZ_CMDE_PROTECTED: t = "PROTECTED"; break;
    case DZ_CMDE_NOT_CONTROLLABLE: t = "NOT CONTROLLABLE"; break;
    case DZ_CMDE_STALE: t = "DATA IS STALE"; break;
    case DZ_CMDE_QUEUE_FULL: t = "BUSY"; break;
    case DZ_CMDE_UNKNOWN_DEVICE: t = "DEVICE GONE"; break;
    case DZ_CMDE_UNSUPPORTED: t = "VIEW ONLY HERE"; break;
    default: t = "REFUSED"; break;
  }
  set(f, RU_FB_FAILED, t, now_ms);
}

ru_fb_state_t ru_fb_tick(ru_fb_t *f, uint32_t now_ms) {
  uint32_t age = now_ms - f->since_ms; /* wrap-safe */
  switch (f->st) {
    case RU_FB_PENDING:
      if (age >= RU_FB_PENDING_TIMEOUT_MS) set(f, RU_FB_UNCONFIRMED, "NO ANSWER", now_ms);
      break;
    case RU_FB_CONFIRMED:
      if (age >= RU_FB_CONFIRMED_SHOW_MS) { f->st = RU_FB_IDLE; f->text[0] = 0; }
      break;
    case RU_FB_UNCONFIRMED:
    case RU_FB_FAILED:
      if (age >= RU_FB_PROBLEM_SHOW_MS) { f->st = RU_FB_IDLE; f->text[0] = 0; }
      break;
    default: break;
  }
  return f->st;
}

const char *ru_fb_text(const ru_fb_t *f) { return f->st == RU_FB_IDLE ? "" : f->text; }

const char *ru_fb_state_name(ru_fb_state_t s) {
  switch (s) {
    case RU_FB_IDLE: return "idle";
    case RU_FB_PENDING: return "pending";
    case RU_FB_CONFIRMED: return "confirmed";
    case RU_FB_UNCONFIRMED: return "unconfirmed";
    case RU_FB_FAILED: return "failed";
  }
  return "?";
}
