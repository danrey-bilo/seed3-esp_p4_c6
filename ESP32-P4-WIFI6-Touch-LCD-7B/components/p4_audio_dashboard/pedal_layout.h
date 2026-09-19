#pragma once
#include "seedfx_routing.h"
#include "pedal_grid.h"

/* Orthogonal channel router. Pure bounded C; no LVGL, heap or audio work.
 * Parallel wire segments get distinct tracks. Perpendicular crossings are
 * allowed, and never represent a junction. Fixed lanes run between grid rows. */
typedef struct { int16_t x,y; } pedal_point_t;
typedef struct { pedal_point_t points[8]; uint8_t count; } pedal_route_t;
typedef struct {
    pedal_route_t routes[SEEDFX_MAX_EDGES];
    int16_t canvas_height;
    struct { int16_t x,y,width; uint8_t row,column,rank; } nodes[SEEDFX_MAX_NODES];
    uint8_t lanes,rows,columns;
} pedal_layout_t;
static inline int pedal_layout_node_width(uint16_t type)
{ return pedal_grid_width(pedal_grid_span(type)); }
int pedal_layout_x(const pedal_layout_t *layout,unsigned index);
int pedal_layout_y(const pedal_layout_t *layout,unsigned index);
static inline bool pedal_layout_forward(const pedal_layout_t *layout,unsigned index)
{ (void)layout;(void)index;return true; }
void pedal_layout_build(pedal_layout_t *layout,const SeedFxGraphDefinition *graph,
                        uint8_t input_mask,uint8_t output_mask);
void pedal_layout_build_grid(pedal_layout_t *layout,const SeedFxGraphDefinition *graph,
                            const pedal_grid_t *grid,uint8_t input_mask,uint8_t output_mask,
                            int physical_scroll_y);
void pedal_layout_scroll(pedal_layout_t *layout,const SeedFxGraphDefinition *graph,
                         uint8_t input_mask,uint8_t output_mask,int physical_scroll_y);
