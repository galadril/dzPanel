/* ru_gesture: touch-sample gesture recogniser for a circular screen. Pure C11, no allocation.
 *
 * Input: ru_touch_t samples (x,y,t,down + contact count and contact size when the controller
 * has them). The coordinates of a release sample (down == false) are IGNORED (some controllers
 * report garbage there); the last down position is used.
 *
 * Zones: a touch that STARTS at r >= annulus_frac * R is a RIM touch, otherwise CONTENT.
 * Apps must only receive events with zone == RU_ZONE_CONTENT; the shell consumes RIM events.
 * A touch that starts outside the circle (r > R + slack), a palm (contacts > 1 or
 * size >= palm_size), or a teleporting contact (implausible speed) is rejected until release.
 *
 * Events (zone CONTENT unless stated)
 *   TAP          release within slop, before long-press time (RIM too: zone RIM)
 *   LONG_PRESS   held in slop; fires from ru_gesture_tick or the next sample. RIM => action HOME
 *   DRAG_START/MOVE/END  continuous tracking after leaving the slop circle. DRAG_END carries the
 *                release velocity (0 if the finger paused before lifting)
 *   SWIPE        discrete flick, emitted AFTER DRAG_END when distance, speed and axis dominance pass
 *   EDGE_SWIPE   RIM only: started in the annulus and travelled inward; action chosen by the
 *                start sector: left=BACK, top=QUICK, bottom=HOME (launcher/home), right=CONTEXT
 *   DRAG_* with zone RIM: rim "crown" drag; angle_deg = cumulative clockwise degrees, dangle_deg = step
 *   CANCEL       a started drag was aborted (palm, extra contact, teleport, stale touch)
 * The tap slop and the drag dead-zone are one value (slop_px, default 10 px at 466).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ru_geom.h"

#define RU_GESTURE_MAX_EVENTS 4 /* size of the out[] array callers should supply */

typedef struct {
  int16_t x, y;
  uint32_t t_ms;
  bool down;
  uint8_t contacts; /* 0 = unknown (treated as 1) */
  uint16_t size;    /* contact area/size, 0 = unknown */
} ru_touch_t;

typedef enum { RU_G_NONE = 0, RU_G_TAP, RU_G_LONG_PRESS, RU_G_SWIPE, RU_G_EDGE_SWIPE,
               RU_G_DRAG_START, RU_G_DRAG_MOVE, RU_G_DRAG_END, RU_G_CANCEL } ru_gtype_t;
typedef enum { RU_ZONE_CONTENT = 0, RU_ZONE_RIM } ru_zone_t;
typedef enum { RU_DIR_NONE = 0, RU_DIR_LEFT, RU_DIR_RIGHT, RU_DIR_UP, RU_DIR_DOWN } ru_dir_t; /* finger motion */
typedef enum { RU_ACT_NONE = 0, RU_ACT_BACK, RU_ACT_HOME, RU_ACT_QUICK, RU_ACT_CONTEXT } ru_edge_action_t;

typedef struct {
  ru_gtype_t type;
  ru_zone_t zone;
  ru_dir_t dir;
  ru_edge_action_t action;
  float x, y;        /* current */
  float x0, y0;      /* touch start */
  float dx, dy;      /* current - start */
  float vx, vy;      /* px/s */
  float angle_deg, dangle_deg; /* rim drags */
  uint32_t t_ms, dur_ms;
} ru_gesture_event_t;

typedef struct {
  float slop_px, swipe_min_px, swipe_min_vel, edge_min_px, edge_cone_deg, axis_ratio;
  float annulus_frac, outside_slack_px, max_speed, teleport_px;
  uint32_t long_press_ms, rim_long_press_ms, stale_ms, vel_window_ms;
  uint16_t palm_size; /* 0 disables size-based rejection */
} ru_gesture_cfg_t;

void ru_gesture_cfg_default(ru_gesture_cfg_t *c, int diameter); /* scaled by diameter/466 */
/* Square panel (D x D): the default config for the same diameter, but touches in the corners (beyond R) are accepted
 * and count as RIM: outside_slack_px = D. Bearing sectors (BACK/QUICK/HOME/CONTEXT) work the same from the corners. */
void ru_gesture_cfg_square(ru_gesture_cfg_t *c, int diameter);

typedef struct { float x, y; uint32_t t; } ru_gpt_t;

typedef struct {
  ru_gesture_cfg_t cfg;
  ru_geom_t geom;
  int state;
  ru_zone_t zone;
  bool started; /* a DRAG_START has been emitted */
  float x0, y0, x, y, ang_last, ang_cum;
  uint32_t t0, t_last;
  ru_gpt_t hist[8];
  int nhist;
  uint32_t rejected_touches, bad_samples;
} ru_gesture_t;

void ru_gesture_init(ru_gesture_t *g, int diameter, const ru_gesture_cfg_t *cfg /*NULL = defaults*/);
void ru_gesture_reset(ru_gesture_t *g);
/* Both return the number of events written to out (<= max). */
int ru_gesture_feed(ru_gesture_t *g, const ru_touch_t *s, ru_gesture_event_t *out, int max);
int ru_gesture_tick(ru_gesture_t *g, uint32_t now_ms, ru_gesture_event_t *out, int max);
ru_zone_t ru_gesture_zone_of(const ru_gesture_t *g, float x, float y);
