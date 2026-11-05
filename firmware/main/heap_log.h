#pragma once
#include "sdkconfig.h"
/* Starts the periodic heap/stack log line and the failed-allocation hook when CONFIG_RU_HEAP_LOG=y; otherwise a no-op. */
void ru_heap_log_start(void);

#if CONFIG_RU_HEAP_LOG
/* Logs one "mark <tag> ..." line with internal free/largest/lowest-ever and remembers the tag as the most recent one (it is
 * named by the low-memory line). `tag` must be a string literal or other static string, never a URL, token, SSID or name.
 * ru_heap_log_mark_res adds the numbers of a finished request: status, error code, bytes, elapsed ms. Task context only. */
void ru_heap_log_mark(const char *tag);
void ru_heap_log_mark_res(const char *tag, int status, int err, unsigned bytes, unsigned elapsed_ms);
#else
static inline void ru_heap_log_mark(const char *tag) { (void)tag; }
static inline void ru_heap_log_mark_res(const char *tag, int status, int err, unsigned bytes, unsigned elapsed_ms) {
  (void)tag; (void)status; (void)err; (void)bytes; (void)elapsed_ms;
}
#endif
