#include "ru_gesture.h"
#include <math.h>
#include <string.h>

enum { S_IDLE, S_PENDING, S_DRAG, S_RIM_DRAG, S_EDGE, S_DONE, S_REJECT };

void ru_gesture_cfg_default(ru_gesture_cfg_t *c, int diameter) {
  float s = (float)diameter / 466.0f;
  c->slop_px = 10 * s; c->swipe_min_px = 36 * s; c->swipe_min_vel = 150 * s;
  c->edge_min_px = 36 * s; c->edge_cone_deg = 50; c->axis_ratio = 1.2f;
  c->annulus_frac = 0.90f; c->outside_slack_px = 2 * s;
  c->max_speed = 8000 * s; c->teleport_px = 80 * s;
  c->long_press_ms = 500; c->rim_long_press_ms = 800; c->stale_ms = 1500; c->vel_window_ms = 100;
  c->palm_size = 60;
}

void ru_gesture_cfg_square(ru_gesture_cfg_t *c, int diameter) {
  ru_gesture_cfg_default(c, diameter);
  c->outside_slack_px = (float)diameter; /* the farthest corner is R*sqrt(2) from the centre, i.e. 0.42R beyond R */
}

void ru_gesture_init(ru_gesture_t *g, int diameter, const ru_gesture_cfg_t *cfg) {
  memset(g, 0, sizeof *g);
  ru_geom_init(&g->geom, diameter);
  if (cfg) g->cfg = *cfg; else ru_gesture_cfg_default(&g->cfg, diameter);
}

void ru_gesture_reset(ru_gesture_t *g) { g->state = S_IDLE; g->started = false; g->nhist = 0; }

ru_zone_t ru_gesture_zone_of(const ru_gesture_t *g, float x, float y) {
  float dx = x - g->geom.r, dy = y - g->geom.r;
  return sqrtf(dx * dx + dy * dy) >= g->cfg.annulus_frac * g->geom.r ? RU_ZONE_RIM : RU_ZONE_CONTENT;
}

typedef struct { ru_gesture_event_t *out; int max, n; } sink_t;

static void emit(ru_gesture_t *g, sink_t *k, ru_gtype_t type, uint32_t now) {
  if (k->n >= k->max) return;
  ru_gesture_event_t *e = &k->out[k->n++];
  memset(e, 0, sizeof *e);
  e->type = type; e->zone = g->zone; e->x = g->x; e->y = g->y; e->x0 = g->x0; e->y0 = g->y0;
  e->dx = g->x - g->x0; e->dy = g->y - g->y0; e->t_ms = now; e->dur_ms = now - g->t0;
}

static void velocity(const ru_gesture_t *g, uint32_t now, float *vx, float *vy) {
  *vx = *vy = 0;
  if (g->nhist < 2) return;
  if (now - g->t_last > g->cfg.vel_window_ms) return; /* paused before lifting */
  const ru_gpt_t *b = &g->hist[g->nhist - 1], *a = b;
  for (int i = g->nhist - 2; i >= 0; i--) {
    if (b->t - g->hist[i].t > g->cfg.vel_window_ms) break;
    a = &g->hist[i];
  }
  uint32_t dt = b->t - a->t;
  if (dt < 8) return;
  *vx = (b->x - a->x) * 1000.0f / (float)dt; *vy = (b->y - a->y) * 1000.0f / (float)dt;
}

static void hist_push(ru_gesture_t *g, float x, float y, uint32_t t) {
  if (g->nhist == 8) { memmove(&g->hist[0], &g->hist[1], 7 * sizeof g->hist[0]); g->nhist = 7; }
  g->hist[g->nhist++] = (ru_gpt_t){ x, y, t };
}

static float bearing(const ru_gesture_t *g, float x, float y) { /* 0 = 12 o'clock, clockwise */
  float a = atan2f(x - g->geom.r, -(y - g->geom.r)) * 180.0f / (float)M_PI;
  return a < 0 ? a + 360.0f : a;
}

static ru_edge_action_t sector_action(float b) {
  if (b >= 315 || b < 45) return RU_ACT_QUICK;
  if (b < 135) return RU_ACT_CONTEXT;
  if (b < 225) return RU_ACT_HOME;
  return RU_ACT_BACK;
}

static void abort_touch(ru_gesture_t *g, sink_t *k, uint32_t now) {
  if (g->started) emit(g, k, RU_G_CANCEL, now);
  g->started = false; g->state = S_REJECT; g->rejected_touches++;
}

static void check_long(ru_gesture_t *g, sink_t *k, uint32_t now) {
  if (g->state != S_PENDING) return;
  uint32_t need = g->zone == RU_ZONE_RIM ? g->cfg.rim_long_press_ms : g->cfg.long_press_ms;
  if (now - g->t0 >= need) {
    emit(g, k, RU_G_LONG_PRESS, now);
    if (k->n && k->out[k->n - 1].type == RU_G_LONG_PRESS && g->zone == RU_ZONE_RIM) k->out[k->n - 1].action = RU_ACT_HOME;
    g->state = S_DONE;
  }
}

static ru_dir_t dominant_dir(const ru_gesture_t *g, float dx, float dy) {
  float ax = fabsf(dx), ay = fabsf(dy), r = g->cfg.axis_ratio;
  if (ax >= r * ay) return dx < 0 ? RU_DIR_LEFT : RU_DIR_RIGHT;
  if (ay >= r * ax) return dy < 0 ? RU_DIR_UP : RU_DIR_DOWN;
  return RU_DIR_NONE;
}

static void on_move(ru_gesture_t *g, sink_t *k, uint32_t now) {
  float dx = g->x - g->x0, dy = g->y - g->y0, d = sqrtf(dx * dx + dy * dy);
  float cx = g->geom.r - g->x0, cy = g->geom.r - g->y0, r0 = sqrtf(cx * cx + cy * cy);
  float nx = r0 > 0.5f ? cx / r0 : 0, ny = r0 > 0.5f ? cy / r0 : 0; /* inward unit */
  switch (g->state) {
  case S_PENDING:
    if (d < g->cfg.slop_px) return;
    if (g->zone == RU_ZONE_CONTENT) {
      g->state = S_DRAG; g->started = true; emit(g, k, RU_G_DRAG_START, now);
    } else {
      float inward = dx * nx + dy * ny, tg = fabsf(-dx * ny + dy * nx);
      if (inward < 0 && -inward > tg) { g->state = S_DONE; return; } /* outward: nothing */
      if (inward >= tg) { g->state = S_EDGE; on_move(g, k, now); }
      else {
        g->state = S_RIM_DRAG; g->started = true;
        g->ang_last = bearing(g, g->x0, g->y0); g->ang_cum = 0;
        emit(g, k, RU_G_DRAG_START, now);
        on_move(g, k, now);
      }
    }
    return;
  case S_DRAG: emit(g, k, RU_G_DRAG_MOVE, now); return;
  case S_RIM_DRAG: {
    float rx = g->x - g->geom.r, ry = g->y - g->geom.r;
    if (rx * rx + ry * ry < 4.0f) return;
    float a = bearing(g, g->x, g->y), da = a - g->ang_last;
    while (da > 180) da -= 360;
    while (da < -180) da += 360;
    g->ang_last = a;
    if (da == 0) return;
    g->ang_cum += da;
    emit(g, k, RU_G_DRAG_MOVE, now);
    if (k->n) { k->out[k->n - 1].angle_deg = g->ang_cum; k->out[k->n - 1].dangle_deg = da; }
    return;
  }
  case S_EDGE: {
    float inward = dx * nx + dy * ny;
    if (d <= 0) return;
    float cosang = inward / d;
    if (inward >= g->cfg.edge_min_px && cosang >= cosf(g->cfg.edge_cone_deg * (float)M_PI / 180.0f)) {
      emit(g, k, RU_G_EDGE_SWIPE, now);
      if (k->n) {
        ru_gesture_event_t *e = &k->out[k->n - 1];
        e->action = sector_action(bearing(g, g->x0, g->y0));
        e->dir = dominant_dir(g, dx, dy);
      }
      g->state = S_DONE;
    }
    return;
  }
  default: return;
  }
}

static void on_release(ru_gesture_t *g, sink_t *k, uint32_t now) {
  float vx, vy;
  switch (g->state) {
  case S_PENDING: emit(g, k, RU_G_TAP, now); break;
  case S_DRAG:
  case S_RIM_DRAG: {
    velocity(g, now, &vx, &vy);
    emit(g, k, RU_G_DRAG_END, now);
    if (k->n) {
      ru_gesture_event_t *e = &k->out[k->n - 1]; e->vx = vx; e->vy = vy;
      if (g->state == S_RIM_DRAG) { e->angle_deg = g->ang_cum; e->dangle_deg = 0; }
    }
    if (g->state == S_DRAG) {
      float dx = g->x - g->x0, dy = g->y - g->y0;
      float dist = sqrtf(dx * dx + dy * dy), sp = sqrtf(vx * vx + vy * vy);
      ru_dir_t dir = dominant_dir(g, dx, dy);
      if (dist >= g->cfg.swipe_min_px && sp >= g->cfg.swipe_min_vel && dir != RU_DIR_NONE) {
        emit(g, k, RU_G_SWIPE, now);
        if (k->n) { ru_gesture_event_t *e = &k->out[k->n - 1]; e->dir = dir; e->vx = vx; e->vy = vy; }
      }
    }
    break;
  }
  default: break;
  }
  g->started = false; g->state = S_IDLE; g->nhist = 0;
}

int ru_gesture_feed(ru_gesture_t *g, const ru_touch_t *s, ru_gesture_event_t *out, int max) {
  sink_t k = { out, max, 0 };
  bool palm = s->contacts > 1 || (g->cfg.palm_size && s->size >= g->cfg.palm_size);
  if (g->state == S_REJECT) { if (!s->down) g->state = S_IDLE; return 0; }
  if (g->state == S_IDLE) {
    if (!s->down) return 0;
    float x = s->x, y = s->y, dx = x - g->geom.r, dy = y - g->geom.r;
    if (palm || sqrtf(dx * dx + dy * dy) > g->geom.r + g->cfg.outside_slack_px) {
      g->state = S_REJECT; g->rejected_touches++; return 0;
    }
    g->x0 = g->x = x; g->y0 = g->y = y; g->t0 = g->t_last = s->t_ms;
    g->zone = ru_gesture_zone_of(g, x, y); g->started = false; g->nhist = 0;
    g->state = S_PENDING; hist_push(g, x, y, s->t_ms);
    return 0;
  }
  /* active touch */
  if (s->t_ms < g->t_last) { g->bad_samples++; return 0; } /* time went backwards */
  if (palm) { abort_touch(g, &k, s->t_ms); return k.n; }
  if (!s->down) {
    check_long(g, &k, s->t_ms);
    on_release(g, &k, s->t_ms);
    return k.n;
  }
  float dx = s->x - g->x, dy = s->y - g->y, dist = sqrtf(dx * dx + dy * dy);
  uint32_t dt = s->t_ms - g->t_last; if (dt < 1) dt = 1;
  if (dist > g->cfg.teleport_px && dist * 1000.0f / (float)dt > g->cfg.max_speed) {
    abort_touch(g, &k, s->t_ms); return k.n;
  }
  check_long(g, &k, s->t_ms);
  g->x = s->x; g->y = s->y; g->t_last = s->t_ms;
  hist_push(g, g->x, g->y, s->t_ms);
  on_move(g, &k, s->t_ms);
  return k.n;
}

int ru_gesture_tick(ru_gesture_t *g, uint32_t now, ru_gesture_event_t *out, int max) {
  sink_t k = { out, max, 0 };
  if (g->state == S_IDLE || g->state == S_REJECT) return 0;
  if ((int32_t)(now - g->t_last) < 0) return 0; /* clock went backwards */
  check_long(g, &k, now);
  if (now - g->t_last >= g->cfg.stale_ms && g->state != S_PENDING) { /* release was lost */
    if (g->started) emit(g, &k, RU_G_CANCEL, now);
    g->started = false; g->state = S_IDLE; g->nhist = 0;
  }
  return k.n;
}
