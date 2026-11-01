/* Simulator core: an LVGL display backed by a full-frame XRGB8888 buffer, a virtual or real clock,
 * a synthetic pointer, the circular mask and PNG export. No SDL in here. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct { int size; uint32_t *fb; /* size*size, XRGB8888, unmasked */ } sim_t;

/* size = 466 or 480 (any 100..1000 works). Initialises LVGL, display, pointer and the Halo theme. */
bool sim_init(sim_t *s, int size);
void sim_shutdown(sim_t *s);

/* virtual time: advance the clock by ms in steps of at most 16 and run LVGL. Deterministic. */
void sim_advance(uint32_t ms);
/* switch to the wall clock (interactive window) */
void sim_use_real_clock(void);
uint32_t sim_now_ms(void);

/* synthetic pointer; takes effect on the next sim_advance */
void sim_pointer(int x, int y, bool down);

/* force a full redraw and run LVGL until settled */
void sim_render(void);

/* circle test: pixel centre farther than R from the centre */
bool sim_outside_circle(int size, int x, int y);
/* counts pixels that are not pure black and lie outside the circle (the unmasked buffer) */
long sim_count_outside_lit(const sim_t *s);
/* counts non-black pixels inside the circle */
long sim_count_lit(const sim_t *s);
/* copy fb into dst with everything outside the circle set to black */
void sim_masked_copy(const sim_t *s, uint32_t *dst);

/* PNG (RGB, stored deflate blocks, no zlib dependency). masked = apply the circular mask. */
bool sim_save_png(const sim_t *s, const char *path, bool masked);
