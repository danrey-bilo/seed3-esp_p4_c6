#include "pedal_layout.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>

int pedal_layout_x(const pedal_layout_t *l,unsigned i) { return l->nodes[i].x; }
int pedal_layout_y(const pedal_layout_t *l,unsigned i) { return l->nodes[i].y; }
static int node_index(const SeedFxGraphDefinition *g,uint16_t id)
{
    if(id) for(unsigned i=0;i<g->node_count;++i) if(g->nodes[i].id==id) return i;
    return -1;
}
static int min(int a,int b) { return a<b?a:b; }
static int max(int a,int b) { return a>b?a:b; }
static int socket_x(const pedal_layout_t *l,int n,bool source)
{ return n<0?(source?57:967):l->nodes[n].x+(source?l->nodes[n].width:0); }
static int escape_x(const pedal_layout_t *l,int n,bool source,unsigned port)
{
    if(n<0) return source?60+(int)port*4:964-(int)port*4;
    return socket_x(l,n,source)+(source?1:-1)*(6+(int)port*4);
}
static int socket_y(const pedal_layout_t *l,const SeedFxGraphDefinition *g,int n,
                    bool source,unsigned port,int scroll_y)
{
    if(n<0) return scroll_y+140+port*204;
    const unsigned channels=source?seedfx_output_channels(g->nodes[n].effect_type)
                                    :seedfx_input_channels(g->nodes[n].effect_type);
    return l->nodes[n].y+(channels==1?174:128+(int)port*46);
}
/* Separate parallel segments by at least four pixels. A perpendicular
 * crossing is allowed, and does not imply an audio junction. */
static bool parallel_overlap(pedal_point_t a,pedal_point_t b,pedal_point_t c,pedal_point_t d)
{
    if((a.x==b.x && a.y==b.y) || (c.x==d.x && c.y==d.y)) return false;
    const bool vertical=a.x==b.x;
    if(vertical!=(c.x==d.x)) return false;
    if(abs(vertical?a.x-c.x:a.y-c.y)>=4) return false;
    return vertical?min(max(a.y,b.y),max(c.y,d.y))>max(min(a.y,b.y),min(c.y,d.y))
                   :min(max(a.x,b.x),max(c.x,d.x))>max(min(a.x,b.x),min(c.x,d.x));
}
static bool clear_segment(const pedal_layout_t *l,const SeedFxGraphDefinition *g,
                           unsigned edge,pedal_point_t a,pedal_point_t b,int scroll_y)
{
    for(unsigned e=0;e<g->edge_count;++e) if(e!=edge) {
        const pedal_route_t *r=&l->routes[e];
        const SeedFxEdgeDefinition *def=&g->edges[e],*current=&g->edges[edge];
        for(unsigned j=1;j<r->count;++j) {
            /* Imported fan-out shares a physical socket stem. The editor
             * itself permits one wire per port and uses Splitter nodes. */
            if(j<=2 && current->source_id==def->source_id && current->source_port==def->source_port) continue;
            if(j+2>=r->count && current->destination_id==def->destination_id
                && current->destination_port==def->destination_port) continue;
            if(parallel_overlap(a,b,r->points[j-1],r->points[j])) return false;
        }
        /* Reserve short endpoint leads even for wires not routed yet.
         * They move with the fixed I/O rails while the board scrolls. */
        if(!def->source_id) {
            const int y=scroll_y+140+def->source_port*204;
            if(parallel_overlap(a,b,(pedal_point_t){57,y},
                (pedal_point_t){60+def->source_port*4,y})) return false;
        }
        if(!def->destination_id) {
            const int y=scroll_y+140+def->destination_port*204;
            if(parallel_overlap(a,b,(pedal_point_t){964-def->destination_port*4,y},
                (pedal_point_t){967,y})) return false;
        }
    }
    return true;
}
enum { ROW_LANES=12, BUS_TRACKS=30 };
static int lane_y(int row,unsigned lane)
{ return 16+row*PEDAL_GRID_PITCH_Y+PEDAL_CARD_HEIGHT+10+(int)lane*4; }
static int bus_x(unsigned track)
{ return track<15?68+(int)track*4:900+((int)track-15)*4; }
static int endpoint_row(const pedal_layout_t *l,int node,int y)
{ return node<0?min(PEDAL_GRID_ROWS-1,max(0,y/PEDAL_GRID_PITCH_Y)):l->nodes[node].row; }

static void route_edge(pedal_layout_t *l,const SeedFxGraphDefinition *g,unsigned e,
                        uint8_t input_mask,uint8_t output_mask,int scroll_y)
{
    const SeedFxEdgeDefinition *r=&g->edges[e];
    if((!r->source_id && !(input_mask&(1U<<r->source_port)))
        || (!r->destination_id && !(output_mask&(1U<<r->destination_port)))) return;
    const int src=node_index(g,r->source_id),dst=node_index(g,r->destination_id);
    const int sx=socket_x(l,src,true),dx=socket_x(l,dst,false);
    const int sy=socket_y(l,g,src,true,r->source_port,scroll_y);
    const int dy=socket_y(l,g,dst,false,r->destination_port,scroll_y);
    const int sl=escape_x(l,src,true,r->source_port),dl=escape_x(l,dst,false,r->destination_port);
    const int sr=endpoint_row(l,src,sy),dr=endpoint_row(l,dst,dy);
    pedal_route_t *out=&l->routes[e];
    if(src>=0 && dst>=0 && sr==dr && sx<dx && dx-sx<PEDAL_GRID_PITCH_X && sy==dy
       && clear_segment(l,g,e,(pedal_point_t){sx,sy},(pedal_point_t){dx,dy},scroll_y)) {
        *out=(pedal_route_t){.points={{sx,sy},{dx,dy}},.count=2};return;
    }
    int cost=INT_MAX;
    /* Same-row wires stay in that row's gutter. Endpoint stems are local to
     * their row, so they never run through the interior of a two-cell pedal. */
    if(sr==dr) for(unsigned lane=0;lane<ROW_LANES;++lane) {
        const int y=lane_y(sr,lane),c=abs(y-sy)+abs(y-dy)+abs(sl-dl);
        if(c>=cost) continue;
        if(!clear_segment(l,g,e,(pedal_point_t){sl,sy},(pedal_point_t){sl,y},scroll_y)
           || !clear_segment(l,g,e,(pedal_point_t){sl,y},(pedal_point_t){dl,y},scroll_y)
           || !clear_segment(l,g,e,(pedal_point_t){dl,y},(pedal_point_t){dl,dy},scroll_y)) continue;
        *out=(pedal_route_t){.points={{sx,sy},{sl,sy},{sl,y},{dl,y},{dl,dy},{dx,dy}},.count=6};
        cost=c;
    }
    /* Longer wires use outside buses, clear of every single/wide card.
     * Lane search is bounded; only physical endpoint wires reroute on scroll. */
    for(unsigned track=0;track<BUS_TRACKS;++track) {
        const int x=bus_x(track),base=abs(x-sl)+abs(x-dl);
        if(base+abs(sy-dy)>=cost) continue;
        bool sc[ROW_LANES],dc[ROW_LANES];
        for(unsigned lane=0;lane<ROW_LANES;++lane) {
            const int a=lane_y(sr,lane),b=lane_y(dr,lane);
            sc[lane]=clear_segment(l,g,e,(pedal_point_t){sl,sy},(pedal_point_t){sl,a},scroll_y)
                && clear_segment(l,g,e,(pedal_point_t){sl,a},(pedal_point_t){x,a},scroll_y);
            dc[lane]=clear_segment(l,g,e,(pedal_point_t){dl,dy},(pedal_point_t){dl,b},scroll_y)
                && clear_segment(l,g,e,(pedal_point_t){dl,b},(pedal_point_t){x,b},scroll_y);
        }
        for(unsigned a=0;a<ROW_LANES;++a) if(sc[a])
            for(unsigned b=0;b<ROW_LANES;++b) if(dc[b]) {
                const int ay=lane_y(sr,a),by=lane_y(dr,b);
                const int c=base+abs(ay-sy)+abs(by-dy)+abs(ay-by);
                if(c>=cost || (sr==dr && a==b)) continue;
                if(!clear_segment(l,g,e,(pedal_point_t){x,ay},(pedal_point_t){x,by},scroll_y)) continue;
                *out=(pedal_route_t){.points={{sx,sy},{sl,sy},{sl,ay},{x,ay},
                                             {x,by},{dl,by},{dl,dy},{dx,dy}},.count=8};
                cost=c;
            }
    }
}
void pedal_layout_scroll(pedal_layout_t *l,const SeedFxGraphDefinition *g,
                         uint8_t input_mask,uint8_t output_mask,int scroll_y)
{
    for(unsigned e=0;e<g->edge_count;++e)
        if(!g->edges[e].source_id || !g->edges[e].destination_id) l->routes[e].count=0;
    for(unsigned e=0;e<g->edge_count;++e)
        if(!g->edges[e].source_id || !g->edges[e].destination_id)
            route_edge(l,g,e,input_mask,output_mask,scroll_y);
}
void pedal_layout_build(pedal_layout_t *l,const SeedFxGraphDefinition *g,
                        uint8_t input_mask,uint8_t output_mask)
{
    pedal_grid_t grid={0};pedal_grid_sync(&grid,g);
    pedal_layout_build_grid(l,g,&grid,input_mask,output_mask,0);
}
void pedal_layout_build_grid(pedal_layout_t *l,const SeedFxGraphDefinition *g,
                            const pedal_grid_t *grid,uint8_t input_mask,uint8_t output_mask,int scroll_y)
{
    memset(l,0,sizeof(*l));l->canvas_height=PEDAL_GRID_HEIGHT;
    l->columns=PEDAL_GRID_COLUMNS;l->rows=pedal_grid_used_rows(grid);
    for(unsigned n=0;n<g->node_count;++n) {
        int cell=pedal_grid_find(grid,g->nodes[n].id);if(cell<0) cell=n;
        l->nodes[n].x=pedal_grid_x(cell);l->nodes[n].y=pedal_grid_y(cell);
        l->nodes[n].width=pedal_layout_node_width(g->nodes[n].effect_type);
        l->nodes[n].row=cell/PEDAL_GRID_COLUMNS;l->nodes[n].column=cell%PEDAL_GRID_COLUMNS;
    }
    for(unsigned e=0;e<g->edge_count;++e)
        if(g->edges[e].source_id && g->edges[e].destination_id)
            route_edge(l,g,e,input_mask,output_mask,scroll_y);
    pedal_layout_scroll(l,g,input_mask,output_mask,scroll_y);
}
