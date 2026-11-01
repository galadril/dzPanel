/* Command feedback shared by every control screen. Pure C, no LVGL, no network.
 *
 *   IDLE -> PENDING (command queued, nothing known yet)
 *        -> CONFIRMED    only when dz_cmd read the device back and it matches
 *        -> UNCONFIRMED  sent, but the re-read did not show the wanted state (e.g. a Group still Mixed after On),
 *                        or no answer arrived in time
 *        -> FAILED       refused or not delivered (reason in the text)
 * "DONE" is only ever produced for CONFIRMED. UNCONFIRMED and FAILED stay visible longer than CONFIRMED. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "dz_cmd.h"

typedef enum { RU_FB_IDLE, RU_FB_PENDING, RU_FB_CONFIRMED, RU_FB_UNCONFIRMED, RU_FB_FAILED } ru_fb_state_t;

#define RU_FB_PENDING_TIMEOUT_MS 15000u
#define RU_FB_CONFIRMED_SHOW_MS 2500u
#define RU_FB_PROBLEM_SHOW_MS 7000u

typedef struct {
  ru_fb_state_t st;
  uint32_t since_ms;
  char idx[12];
  bool scene;
  char text[32];
} ru_fb_t;

void ru_fb_init(ru_fb_t *f);
void ru_fb_begin(ru_fb_t *f, const char *idx, bool scene, uint32_t now_ms);
/* The outcome callback of dz_cmd (via the app's queue). */
void ru_fb_on_result(ru_fb_t *f, const dz_cmd_result_t *r, uint32_t now_ms);
/* dz_cmd_submit refused the request (protected, stale, view only...). */
void ru_fb_on_refused(ru_fb_t *f, dz_cmd_err_t e, uint32_t now_ms);
/* Expire old messages and time out a PENDING one into UNCONFIRMED. Returns the state after. */
ru_fb_state_t ru_fb_tick(ru_fb_t *f, uint32_t now_ms);
/* Visible text, "" for IDLE. Upper case, at most 20 characters. */
const char *ru_fb_text(const ru_fb_t *f);
const char *ru_fb_state_name(ru_fb_state_t s);
