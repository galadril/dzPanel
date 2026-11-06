/* Pure colour maths for lamps (no LVGL, no allocation). Domoticz facts (read in its source, not yet checked on a real lamp):
 * set_color_brightness builds RGB with hsb2rgb(hue, 1.0, 1.0), saturation and value are ALWAYS full; set_color_temperature takes
 * kelvin 2700..6500 and stores t = round(100 * (6500 - K) / 3800 * 2.55): t 0 = coolest, t 255 = warmest. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "dz_parse.h"

/* What the Color JSON says the lamp shows */
typedef enum { DZ_LM_NONE = 0, DZ_LM_WHITE, DZ_LM_TEMP, DZ_LM_COLOR } dz_light_mode_t;
/* m 1 -> WHITE; m 2 -> TEMP (knob at t); m 3 or 4 -> COLOR (hue from RGB; cw/ww ignored); m 0 or no colour -> NONE */
dz_light_mode_t dz_light_mode(const dz_device_t *d);

/* Domoticz hsb2rgb with saturation 1.0 and value 1.0. hue is wrapped into 0..360 */
void dz_hsb2rgb(float hue, uint8_t *r, uint8_t *g, uint8_t *b);
/* Hue 0..360 and saturation 0..1 of an RGB triple. false (hue untouched) when there is no hue to show: max == 0 or saturation < 0.15 */
bool dz_rgb_to_hue(uint8_t r, uint8_t g, uint8_t b, float *hue, float *sat);
/* circular distance between two hues, 0..180 */
float dz_hue_dist(float a, float b);

/* Temperature scale. Warmth is 0 (coolest, 6500 K) .. 100 (warmest, 2700 K). */
int dz_warmth_to_kelvin(int warmth);   /* 6500 - 38 * L, clamped */
int dz_kelvin_to_warmth(int kelvin);   /* inverse, rounded, clamped 0..100 */
int dz_warmth_to_t(int warmth);        /* round(L * 2.55) */
int dz_t_to_warmth(int t);             /* round(t / 2.55), clamped */
/* Gradient of the temperature ring: p = 0 warm (left end) .. 1 cool (right end): amber FF9F45 -> FFE3BF -> cool D6E6FF. 0xRRGGBB */
uint32_t dz_temp_rgb(float p);
/* The colour a lamp shows for a warmth */
uint32_t dz_warmth_rgb(int warmth);

/* Words for the caption; never Kelvin numbers */
const char *dz_hue_word(float hue);    /* RED ORANGE YELLOW GREEN CYAN BLUE PURPLE PINK */
const char *dz_warmth_word(int warmth); /* WARM SOFT NEUTRAL COOL DAYLIGHT */
