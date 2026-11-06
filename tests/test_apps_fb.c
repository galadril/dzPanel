/* Command feedback: pending -> confirmed / unconfirmed / failed; DONE only for CONFIRMED. */
#include <string.h>

#include "mini_test.h"
#include "ru_fb.h"

static dz_cmd_result_t res(dz_outcome_t o, dz_fail_t f) {
  dz_cmd_result_t r;
  memset(&r, 0, sizeof r);
  r.outcome = o;
  r.fail = f;
  return r;
}

int main(void) {
  ru_fb_t f;
  ru_fb_init(&f);
  CHECK(f.st == RU_FB_IDLE && !strcmp(ru_fb_text(&f), ""));

  ru_fb_begin(&f, "5", true, 1000);
  CHECK(f.st == RU_FB_PENDING && !strcmp(ru_fb_text(&f), "SENDING..."));
  CHECK(strcmp(ru_fb_text(&f), "DONE") != 0); /* never success before CONFIRMED */

  dz_cmd_result_t r = res(DZ_OUT_CONFIRMED, DZ_FAIL_NONE);
  ru_fb_on_result(&f, &r, 2000);
  CHECK(f.st == RU_FB_CONFIRMED && !strcmp(ru_fb_text(&f), "DONE"));
  CHECK(ru_fb_tick(&f, 2000 + RU_FB_CONFIRMED_SHOW_MS - 1) == RU_FB_CONFIRMED);
  CHECK(ru_fb_tick(&f, 2000 + RU_FB_CONFIRMED_SHOW_MS) == RU_FB_IDLE);

  /* UNCONFIRMED is never "DONE", and stays visible longer than a confirmation */
  ru_fb_begin(&f, "5", true, 10000);
  r = res(DZ_OUT_UNCONFIRMED, DZ_FAIL_NONE);
  ru_fb_on_result(&f, &r, 11000);
  CHECK(f.st == RU_FB_UNCONFIRMED && !strcmp(ru_fb_text(&f), "NOT CONFIRMED"));
  CHECK(ru_fb_tick(&f, 11000 + RU_FB_CONFIRMED_SHOW_MS + 1) == RU_FB_UNCONFIRMED);
  CHECK(ru_fb_tick(&f, 11000 + RU_FB_PROBLEM_SHOW_MS) == RU_FB_IDLE);

  /* a Group that answers Mixed after the command says so */
  r = res(DZ_OUT_UNCONFIRMED, DZ_FAIL_NONE);
  r.have_after = true;
  strcpy(r.after.status, "Mixed");
  r.after.cap = DZ_CAP_GROUP;
  ru_fb_on_result(&f, &r, 20000);
  CHECK(f.st == RU_FB_UNCONFIRMED && !strcmp(ru_fb_text(&f), "STILL MIXED"));

  /* a push the server skipped is not "NOT CONFIRMED" */
  r = res(DZ_OUT_NO_ACTION, DZ_FAIL_NONE);
  ru_fb_on_result(&f, &r, 30000);
  CHECK(f.st == RU_FB_UNCONFIRMED && !strcmp(ru_fb_text(&f), "ALREADY PRESSED") && strlen(ru_fb_text(&f)) <= 16);

  /* failures name the reason */
  r = res(DZ_OUT_FAILED, DZ_FAIL_FORBIDDEN);
  ru_fb_on_result(&f, &r, 30000);
  CHECK(f.st == RU_FB_FAILED && !strcmp(ru_fb_text(&f), "NO SWITCH RIGHTS"));
  r = res(DZ_OUT_FAILED, DZ_FAIL_TRANSPORT);
  ru_fb_on_result(&f, &r, 30000);
  CHECK(!strcmp(ru_fb_text(&f), "NO CONNECTION"));
  r = res(DZ_OUT_FAILED, DZ_FAIL_NO_MCP);
  ru_fb_on_result(&f, &r, 30000);
  CHECK(!strcmp(ru_fb_text(&f), "MCP NOT AVAIL") && strlen(ru_fb_text(&f)) <= 16);
  r = res(DZ_OUT_FAILED, DZ_FAIL_REJECTED);
  ru_fb_on_result(&f, &r, 30000);
  CHECK(!strcmp(ru_fb_text(&f), "REJECTED"));
  r = res(DZ_OUT_FAILED, DZ_FAIL_PROTECTED);
  ru_fb_on_result(&f, &r, 30000);
  CHECK(!strcmp(ru_fb_text(&f), "PROTECTED"));
  ru_fb_on_refused(&f, DZ_CMDE_UNSUPPORTED, 31000);
  CHECK(f.st == RU_FB_FAILED && !strcmp(ru_fb_text(&f), "VIEW ONLY HERE") && strlen(ru_fb_text(&f)) <= 16);
  ru_fb_on_refused(&f, DZ_CMDE_PROTECTED, 31000);
  CHECK(f.st == RU_FB_FAILED && !strcmp(ru_fb_text(&f), "PROTECTED"));
  ru_fb_on_refused(&f, DZ_CMDE_CONTROL_DISABLED, 31000);
  CHECK(!strcmp(ru_fb_text(&f), "VIEW ONLY"));

  /* no answer at all: PENDING times out into UNCONFIRMED, not into success, across a clock wrap */
  uint32_t t0 = 0xFFFFFF00u;
  ru_fb_begin(&f, "7", false, t0);
  CHECK(ru_fb_tick(&f, t0 + RU_FB_PENDING_TIMEOUT_MS - 1) == RU_FB_PENDING);
  CHECK(ru_fb_tick(&f, t0 + RU_FB_PENDING_TIMEOUT_MS) == RU_FB_UNCONFIRMED);
  CHECK(!strcmp(ru_fb_text(&f), "NO ANSWER"));
  MT_DONE();
}
