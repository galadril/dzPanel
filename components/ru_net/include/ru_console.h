/* Console demultiplexer: ONE byte stream (USB-Serial/JTAG RX) carries two protocols that must never confuse
 * each other: Improv Serial packets and RU+CFG text lines.
 *
 * Rules (all covered by tests/test_net.c):
 *  - Improv packets start with "IMPROV" and are consumed by LENGTH; their data (which may contain 'R', 'I', 0x0A)
 *    is never examined for anything else. A packet may be followed by an optional newline, which is ignored.
 *  - An RU+CFG line starts with the exact 7 bytes "RU+CFG " and runs to the next '\n' ('\r' before it is dropped).
 *    Inside a line, the bytes "IMPROV" mean nothing.
 *  - Anything else outside a packet or line (log text echoed by a terminal, stray bytes) is ignored byte by byte, so
 *    garbage never blocks the next packet or line. A candidate that mismatches is re-examined from the byte that
 *    broke it, so "IMPIMPROV..." and "RRU+CFG ..." both still work.
 *  - A line over RU_CFGLINE_MAX bytes calls on_overflow once and the rest of it is discarded up to '\n'.
 *  - If no byte arrives for RU_CONSOLE_IDLE_MS while a packet or line is incomplete, it is dropped (ru_console_tick).
 *  - The line buffer is zeroed after every line (it held a token).
 * Output side (not in this file): device -> host text. Improv frames are emitted whole between newlines (see
 * ru_improv.h); RU+CFG replies are one whole line. Ordinary log lines must not start with "RU+CFG " or "IMPROV". The
 * host (site/lib.js parseCfgReply, ESP Web Tools) ignores lines that do not parse, so logs and both protocols can share
 * the console. Use ONE writer (mutex) for frames and replies; log lines are whole-line writes by the IDF. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "ru_cfgline.h"
#include "ru_improv.h"

#define RU_CONSOLE_IDLE_MS 3000

typedef struct {
  /* json = payload after "RU+CFG " (NUL terminated, len bytes). The buffer is wiped right after the call returns. */
  void (*on_cfg_line)(void *user, const char *json, size_t len);
  void (*on_cfg_overflow)(void *user);
  void *user;
} ru_console_ops_t;

typedef struct {
  ru_console_ops_t ops;
  ru_improv_t *improv; /* may be NULL */
  int st;
  size_t pos;
  uint32_t last_ms;
  uint32_t lines, overflows, packets_dropped, idle_resets;
  char line[RU_CFGLINE_MAX - RU_CFGLINE_PREFIX_LEN + 1];
} ru_console_t;

void ru_console_init(ru_console_t *c, const ru_console_ops_t *ops, ru_improv_t *improv);
void ru_console_feed(ru_console_t *c, const uint8_t *data, size_t n, uint32_t now_ms);
void ru_console_tick(ru_console_t *c, uint32_t now_ms);
