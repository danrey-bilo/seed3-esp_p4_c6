#pragma once
#include "seedfx_routing.h"

/* Orthogonal channel router. Pure bounded C; no LVGL, heap or audio work.
 * Parallel wire segments get distinct tracks. Perpendicular crossings are
 * allowed, and never represent a junction. The centre corridor grows if needed. */
typedef struct { int16_t x,y; } pedal_point_t;
typedef struct { pedal_point_t points[6]; uint8_t count; } pedal_route_t;
typedef struct {
    pedal_route_t routes[SEEDFX_MAX_EDGES];
    int16_t second_row_y, canvas_height;
    struct { int16_t x,width,column_x,column_width; uint8_t row; } nodes[SEEDFX_MAX_NODES];
    uint8_t lanes,rows,columns;
} pedal_layout_t;
static inline int pedal_layout_node_width(uint16_t type)
{ return type==SEEDFX_EFFECT_CABINET_SIM?208:104; }
int pedal_layout_x(const pedal_layout_t *layout,unsigned index);
int pedal_layout_y(const pedal_layout_t *layout,unsigned index);
static inline bool pedal_layout_forward(const pedal_layout_t *layout,unsigned index)
{ return !(layout->nodes[index].row&1); }
void pedal_layout_build(pedal_layout_t *layout,const SeedFxGraphDefinition *graph,
                        uint8_t input_mask,uint8_t output_mask);
