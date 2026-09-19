#include "pedal_grid.h"
#include <string.h>
int pedal_grid_find(const pedal_grid_t *g,uint16_t id)
{
    if(id) for(unsigned i=0;i<PEDAL_GRID_CELLS;++i) if(g->cells[i]==id) return (int)i;
    return -1;
}
int pedal_grid_hit(int x,int y)
{
    x-=PEDAL_GRID_LEFT;
    if(x<0 || x>=PEDAL_GRID_COLUMNS*PEDAL_GRID_PITCH_X || y<0
        || y>=PEDAL_GRID_ROWS*PEDAL_GRID_PITCH_Y) return -1;
    return (y/PEDAL_GRID_PITCH_Y)*PEDAL_GRID_COLUMNS+x/PEDAL_GRID_PITCH_X;
}
static bool fits(const pedal_grid_t *g,int cell,unsigned span)
{
    if(cell<0 || cell>=PEDAL_GRID_CELLS || cell%PEDAL_GRID_COLUMNS+(int)span>PEDAL_GRID_COLUMNS) return false;
    for(unsigned i=0;i<span;++i) if(g->cells[cell+i]) return false;
    return true;
}
static void put(pedal_grid_t *g,uint16_t id,int cell,unsigned span)
{ for(unsigned i=0;i<span;++i) g->cells[cell+i]=id; }
static unsigned span_of(const pedal_grid_t *g,uint16_t id)
{
    unsigned span=0;for(unsigned i=0;i<PEDAL_GRID_CELLS;++i) span+=g->cells[i]==id;
    return span;
}
void pedal_grid_sync(pedal_grid_t *g,const SeedFxGraphDefinition *graph)
{
    pedal_grid_t next={0};
    for(unsigned n=0;n<graph->node_count;++n) {
        const uint16_t id=graph->nodes[n].id;
        const int cell=pedal_grid_find(g,id);
        const unsigned span=pedal_grid_span(graph->nodes[n].effect_type);
        if(id && fits(&next,cell,span)) put(&next,id,cell,span);
    }
    for(unsigned n=0;n<graph->node_count;++n) {
        const uint16_t id=graph->nodes[n].id;
        const unsigned span=pedal_grid_span(graph->nodes[n].effect_type);
        if(!id || pedal_grid_find(&next,id)>=0) continue;
        for(unsigned cell=0;cell<PEDAL_GRID_CELLS;++cell) if(fits(&next,cell,span)) {
            put(&next,id,cell,span);break;
        }
    }
    *g=next;
}
int pedal_grid_drop_cell(const pedal_grid_t *g,uint16_t id,int cell)
{
    if(cell<0 || cell>=PEDAL_GRID_CELLS) return -1;
    return g->cells[cell] && g->cells[cell]!=id ? pedal_grid_find(g,g->cells[cell]):cell;
}
bool pedal_grid_move(pedal_grid_t *g,uint16_t id,int cell)
{
    const int from=pedal_grid_find(g,id);
    cell=pedal_grid_drop_cell(g,id,cell);
    if(from<0 || cell<0 || cell==from) return false;
    const uint16_t other=g->cells[cell]==id?0:g->cells[cell];
    const unsigned span=span_of(g,id),other_span=other?span_of(g,other):0;
    pedal_grid_t next=*g;
    for(unsigned i=0;i<PEDAL_GRID_CELLS;++i) if(next.cells[i]==id || (other && next.cells[i]==other)) next.cells[i]=0;
    if(!fits(&next,cell,span)) return false;
    put(&next,id,cell,span);
    if(other) {
        if(!fits(&next,from,other_span)) return false;
        put(&next,other,from,other_span);
    }
    *g=next;return true;
}
bool pedal_grid_can_move(const pedal_grid_t *g,uint16_t id,int cell)
{
    if(pedal_grid_find(g,id)<0 || cell<0 || cell>=PEDAL_GRID_CELLS) return false;
    if(pedal_grid_drop_cell(g,id,cell)==pedal_grid_find(g,id)) return true;
    pedal_grid_t copy=*g;return pedal_grid_move(&copy,id,cell);
}
unsigned pedal_grid_used_rows(const pedal_grid_t *g)
{
    for(int cell=PEDAL_GRID_CELLS-1;cell>=0;--cell) if(g->cells[cell]) return cell/PEDAL_GRID_COLUMNS+1;
    return 1;
}
int pedal_grid_canvas_height(const pedal_grid_t *g,bool dragging)
{
    unsigned rows=pedal_grid_used_rows(g)+(dragging?1:0);
    if(rows>PEDAL_GRID_ROWS) rows=PEDAL_GRID_ROWS;
    const int height=16+rows*PEDAL_GRID_PITCH_Y;
    return height<490?490:height;
}
