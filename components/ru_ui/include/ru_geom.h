/* Round-screen geometry. Pure C, no LVGL. See DESIGN.md section 8. */
#pragma once
#include <stdbool.h>

typedef struct { int diameter; float r; float rs; float u; } ru_geom_t;
typedef struct { float x, y; } ru_pt_t;
typedef struct { float x, y, w, h; } ru_rect_t; /* origin = top-left, screen px */

#define RU_SAFE_FACTOR 0.92f

void ru_geom_init(ru_geom_t *g, int diameter);          /* u = D/100, Rs = 0.92 R */
float ru_geom_chord(const ru_geom_t *g, float d);       /* 2*sqrt(Rs^2-d^2); 0 if |d|>=Rs */
float ru_geom_row_width(const ru_geom_t *g, float y_top, float y_bottom); /* worst edge, y from top */
ru_pt_t ru_geom_polar(const ru_geom_t *g, float radius, float deg); /* 0deg = 12 o'clock, clockwise */
ru_rect_t ru_geom_inscribed(const ru_geom_t *g, float aspect); /* largest centred w/h=aspect rect inside Rs */
bool ru_geom_rect_in_safe(const ru_geom_t *g, ru_rect_t r);    /* all 4 corners within Rs */
