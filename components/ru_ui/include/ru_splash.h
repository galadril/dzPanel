/* Boot splash (docs/research/ux.md 10.1): a thin ring draws clockwise while a bead leads it, then the wordmark
 * fades in. Original artwork only: a ring, plain text, no third-party logo. Depends on LVGL and ru_theme. */
#pragma once
#include "lvgl.h"

typedef struct {
  const char *name;     /* wordmark, default "dzPanel" */
  const char *tagline;  /* default "for Domoticz" (plain text, no logo) */
  const char *version;  /* small text at the bottom, NULL for none */
} ru_splash_cfg_t;

#define RU_SPLASH_DRAW_MS 900  /* ring draws over this long */
#define RU_SPLASH_FADE_MS 300  /* wordmark fades in after the ring */
#define RU_SPLASH_TOTAL_MS (RU_SPLASH_DRAW_MS + RU_SPLASH_FADE_MS)

/* A new circle-sized black screen (not loaded). cfg may be NULL. Free it with lv_obj_delete: the animations stop and
 * the state is released with it. */
lv_obj_t *ru_splash_create(const ru_splash_cfg_t *cfg);

/* The same splash as an overlay on lv_layer_top(): the real first screen can be built and loaded underneath while the
 * splash plays, with no changes to how that screen is loaded. It covers everything and absorbs touches until
 * ru_splash_dismiss. Returns NULL if memory is short. */
lv_obj_t *ru_splash_overlay_create(const ru_splash_cfg_t *cfg);

/* Fade the splash out over fade_ms and delete it (0 = delete now). Safe to call while it is animating. */
void ru_splash_dismiss(lv_obj_t *splash, uint32_t fade_ms);

/* Draw one deterministic frame: 0 = nothing, 1000 = ring complete and wordmark fully visible. Used by tests and
 * screenshots; also what the animation calls every frame. Ignored once an error is shown. */
void ru_splash_set_progress(lv_obj_t *splash, int permille);

/* Play the draw-and-fade animation (RU_SPLASH_TOTAL_MS). If boot is slower the bead keeps orbiting slowly until the
 * splash is deleted or ru_splash_set_error is called. */
void ru_splash_start(lv_obj_t *splash);

/* Boot error: the ring turns into the error colour and stays open, the bead stops, a short code and a hint appear.
 * Either text may be NULL. */
void ru_splash_set_error(lv_obj_t *splash, const char *code, const char *hint);

/* True once the draw-and-fade animation has finished (or the progress was set to 1000). */
bool ru_splash_is_complete(lv_obj_t *splash);
