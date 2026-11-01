#include "ru_console.h"

#include <string.h>

enum { S_IDLE, S_IMPROV, S_PREFIX, S_LINE, S_DISCARD };
#define LINE_MAX_PAYLOAD (RU_CFGLINE_MAX - RU_CFGLINE_PREFIX_LEN)

void ru_console_init(ru_console_t *c, const ru_console_ops_t *ops, ru_improv_t *improv) {
  memset(c, 0, sizeof *c);
  c->ops = *ops;
  c->improv = improv;
}

static void wipe_line(ru_console_t *c) {
  ru_secure_zero(c->line, sizeof c->line);
  c->pos = 0;
}

static void byte(ru_console_t *c, uint8_t b) {
  for (;;) {
    switch (c->st) {
      case S_IDLE:
        if (b == 'I' && c->improv) {
          c->st = S_IMPROV;
          ru_improv_feed(c->improv, b); /* position 0 always matches 'I' */
        } else if (b == 'R') {
          c->st = S_PREFIX;
          c->pos = 1;
        }
        return;
      case S_IMPROV: {
        ru_improv_feed_t r = ru_improv_feed(c->improv, b);
        if (r == RU_IMPROV_MORE) return;
        c->st = S_IDLE;
        if (r == RU_IMPROV_DONE) return;
        continue; /* NOMATCH: this byte starts over in IDLE */
      }
      case S_PREFIX:
        if (b == (uint8_t)RU_CFGLINE_PREFIX[c->pos]) {
          if (++c->pos == RU_CFGLINE_PREFIX_LEN) { c->st = S_LINE; c->pos = 0; }
          return;
        }
        c->st = S_IDLE;
        c->pos = 0;
        continue;
      case S_LINE:
        if (b == '\n') {
          size_t n = c->pos;
          if (n && c->line[n - 1] == '\r') n--;
          c->line[n] = 0;
          c->lines++;
          if (c->ops.on_cfg_line) c->ops.on_cfg_line(c->ops.user, c->line, n);
          wipe_line(c);
          c->st = S_IDLE;
        } else if (c->pos >= LINE_MAX_PAYLOAD) {
          wipe_line(c);
          c->overflows++;
          c->st = S_DISCARD;
          if (c->ops.on_cfg_overflow) c->ops.on_cfg_overflow(c->ops.user);
        } else c->line[c->pos++] = (char)b;
        return;
      default: /* S_DISCARD */
        if (b == '\n') c->st = S_IDLE;
        return;
    }
  }
}

void ru_console_feed(ru_console_t *c, const uint8_t *d, size_t n, uint32_t now_ms) {
  if (n) c->last_ms = now_ms;
  for (size_t i = 0; i < n; i++) byte(c, d[i]);
}

void ru_console_tick(ru_console_t *c, uint32_t now_ms) {
  if (c->st == S_IDLE || c->st == S_DISCARD) return;
  if ((uint32_t)(now_ms - c->last_ms) < RU_CONSOLE_IDLE_MS) return;
  if (c->st == S_IMPROV) { if (c->improv) ru_improv_reset(c->improv); c->packets_dropped++; }
  wipe_line(c);
  c->idle_resets++;
  c->st = S_IDLE;
}
