/* Colour ring image for lv_arc: an RGB565 "conic" gradient generated at RUNTIME into ONE buffer (PSRAM on the device, malloc in the
 * simulator). lv_draw_sw_arc fills the arc mask from an image centred on the arc (lv_obj_set_style_arc_image_src), so a plain lv_arc
 * with this image as the background arc draws the hue wheel or the temperature scale. Only the annulus is generated (the arc mask
 * never reads the rest). The buffer is reused for every kind: asking for another kind regenerates it, asking again for the same
 * (diameter, width, kind) returns the cached image without touching the pixels. */
#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { RU_RING_HUE = 0, RU_RING_TEMP = 1 } ru_ring_kind_t;

/* position p in 0..1 along the 270 degree sweep (0 = start at the lower left, 1 = end at the lower right) -> 0xRRGGBB */
typedef uint32_t (*ru_ring_color_fn)(float p);

/* The sweep geometry is the one the dials use: rotation 135, sweep 270 (open at the bottom). d = outer diameter of the arc, w = its width.
 * Returns NULL if the buffer cannot be allocated (PSRAM only on the device, never internal RAM). The pointer stays valid until the next call with other parameters. */
const lv_image_dsc_t *ru_ring_img_get(int d, int w, ru_ring_kind_t kind, ru_ring_color_fn fn);

/* how many times pixels were generated since start (tests: a rebuild must not generate again) */
uint32_t ru_ring_img_generations(void);
/* frees the buffer (tests, low memory); the next get regenerates */
void ru_ring_img_free(void);
/* tests: make the buffer allocation fail (true) or work again (false) */
#ifdef RU_TEST_HOOKS
void ru_ring_img_test_fail_alloc(bool fail);
#endif
