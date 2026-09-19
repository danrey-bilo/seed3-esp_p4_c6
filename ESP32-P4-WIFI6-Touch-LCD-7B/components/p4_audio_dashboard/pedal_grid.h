#pragma once
#include "seedfx_graph_protocol.h"

/* View-only placement, keyed by stable DSP node ID. Empty cells stay empty. */
enum {
    PEDAL_GRID_COLUMNS=5, PEDAL_GRID_ROWS=8,
    PEDAL_GRID_CELLS=PEDAL_GRID_COLUMNS*PEDAL_GRID_ROWS,
    PEDAL_GRID_LEFT=112, PEDAL_GRID_PITCH_X=160, PEDAL_GRID_PITCH_Y=266,
    PEDAL_CARD_WIDTH=104, PEDAL_CARD_HEIGHT=198, PEDAL_CARD_RADIUS=12,
    PEDAL_GRID_HEIGHT=16+PEDAL_GRID_ROWS*PEDAL_GRID_PITCH_Y
};
typedef struct { uint16_t cells[PEDAL_GRID_CELLS]; } pedal_grid_t;
static inline int pedal_grid_x(unsigned cell)
{ return PEDAL_GRID_LEFT+28+(cell%PEDAL_GRID_COLUMNS)*PEDAL_GRID_PITCH_X; }
static inline int pedal_grid_y(unsigned cell)
{ return 16+(cell/PEDAL_GRID_COLUMNS)*PEDAL_GRID_PITCH_Y; }
int pedal_grid_find(const pedal_grid_t *grid,uint16_t id);
int pedal_grid_hit(int x,int y);
void pedal_grid_sync(pedal_grid_t *grid,const SeedFxGraphDefinition *graph);
bool pedal_grid_move(pedal_grid_t *grid,uint16_t id,int cell);
int pedal_grid_drop_cell(const pedal_grid_t *grid,uint16_t id,int cell);
bool pedal_grid_can_move(const pedal_grid_t *grid,uint16_t id,int cell);
unsigned pedal_grid_used_rows(const pedal_grid_t *grid);
int pedal_grid_canvas_height(const pedal_grid_t *grid,bool dragging);
static inline unsigned pedal_grid_span(uint16_t type)
{ return type==SEEDFX_EFFECT_CABINET_SIM?2:1; }
static inline int pedal_grid_width(unsigned span)
{ return PEDAL_CARD_WIDTH+(span-1)*PEDAL_GRID_PITCH_X; }
