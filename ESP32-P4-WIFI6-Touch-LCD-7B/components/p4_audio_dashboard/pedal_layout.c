#include "pedal_layout.h"
#include <string.h>

int pedal_layout_x(const pedal_layout_t *l,unsigned i) { return l->nodes[i].x; }
int pedal_layout_y(const pedal_layout_t *l,unsigned i)
{ return !l->nodes[i].row?24:l->second_row_y+(l->nodes[i].row-1)*230; }
static int node_index(const SeedFxGraphDefinition *g,uint16_t id)
{
    if(!id) return -1;
    for(unsigned i=0;i<g->node_count;++i) if(g->nodes[i].id==id) return i;
    return -1;
}
static int min(int a,int b) { return a<b?a:b; }
static int max(int a,int b) { return a>b?a:b; }
static bool parallel_collision(const pedal_route_t *a,const pedal_route_t *b)
{
    for(unsigned i=1;i<a->count;++i) for(unsigned j=1;j<b->count;++j) {
        const pedal_point_t a0=a->points[i-1],a1=a->points[i];
        const pedal_point_t b0=b->points[j-1],b1=b->points[j];
        const bool vertical=a0.x==a1.x;
        if(vertical!=(b0.x==b1.x)) continue;
        const int distance=vertical?a0.x-b0.x:a0.y-b0.y;
        if(distance<=-4 || distance>=4) continue;
        const int alo=vertical?min(a0.y,a1.y):min(a0.x,a1.x);
        const int ahi=vertical?max(a0.y,a1.y):max(a0.x,a1.x);
        const int blo=vertical?min(b0.y,b1.y):min(b0.x,b1.x);
        const int bhi=vertical?max(b0.y,b1.y):max(b0.x,b1.x);
        if(min(ahi,bhi)>max(alo,blo)) return true;
    }
    return false;
}
static void place_nodes(pedal_layout_t *l,const SeedFxGraphDefinition *g)
{
    int widths[6]={0};
    unsigned columns=g->node_count<6?g->node_count:6;
    if(!columns) columns=1;
    for(;columns;--columns) {
        memset(widths,0,sizeof(widths));
        const unsigned rows=(g->node_count+columns-1)/columns;
        for(unsigned i=0;i<g->node_count;++i) {
            const unsigned row=i/columns;
            const unsigned col=(row&1)?columns-1-i%columns:i%columns;
            widths[col]=max(widths[col],pedal_layout_node_width(g->nodes[i].effect_type));
        }
        int span=(columns-1)*(4+rows*20);
        for(unsigned col=0;col<columns;++col) span+=widths[col];
        if(span>884-(int)rows*20 && columns>1) continue;
        l->columns=columns;l->rows=rows;
        for(unsigned i=0;i<g->node_count;++i) {
            const unsigned row=i/columns;
            const unsigned col=(row&1)?columns-1-i%columns:i%columns;
            int x=70+rows*10;
            for(unsigned c=0;c<col;++c) x+=widths[c]+4+rows*20;
            l->nodes[i].width=pedal_layout_node_width(g->nodes[i].effect_type);
            l->nodes[i].column_x=x;l->nodes[i].column_width=widths[col];
            l->nodes[i].x=x+(widths[col]-l->nodes[i].width)/2;
            l->nodes[i].row=row;
        }
        break;
    }
}
static int socket_x(const pedal_layout_t *l,int n,bool source)
{
    return n<0?(source?57:967):pedal_layout_x(l,n)
        +(pedal_layout_forward(l,n)==source?l->nodes[n].width:0);
}
static int escape_x(const pedal_layout_t *l,int n,bool source,unsigned port)
{
    if(n<0) return source?61+(int)port*4:963-(int)port*4;
    const bool right=pedal_layout_forward(l,n)==source;
    const int boundary=l->nodes[n].column_x+(right?l->nodes[n].column_width:0);
    return boundary+(right?1:-1)*(5+(int)port*5+l->nodes[n].row*10);
}
static int socket_y(const pedal_layout_t *l,const SeedFxGraphDefinition *g,int n,
                    bool source,unsigned port)
{
    if(n<0) return 140+port*204;
    const unsigned channels=source?seedfx_output_channels(g->nodes[n].effect_type)
                                    :seedfx_input_channels(g->nodes[n].effect_type);
    return pedal_layout_y(l,n)+(channels==1?172:138+(int)port*46);
}

void pedal_layout_build(pedal_layout_t *l,const SeedFxGraphDefinition *g,
                        uint8_t input_mask,uint8_t output_mask)
{
    memset(l,0,sizeof(*l)); l->second_row_y=254; l->canvas_height=490;
    place_nodes(l,g);
    int8_t lane[SEEDFX_MAX_EDGES]; memset(lane,-1,sizeof(lane));
    for(unsigned e=0;e<g->edge_count;++e) {
        const SeedFxEdgeDefinition *r=&g->edges[e];
        if((!r->source_id && !(input_mask&(1U<<r->source_port)))
           || (!r->destination_id && !(output_mask&(1U<<r->destination_port)))) continue;
        const int src=node_index(g,r->source_id),dst=node_index(g,r->destination_id);
        const int sx=socket_x(l,src,true),dx=socket_x(l,dst,false);
        const int sy=socket_y(l,g,src,true,r->source_port);
        const int dy=socket_y(l,g,dst,false,r->destination_port);
        pedal_route_t *route=&l->routes[e];
        /* Only a genuinely straight neighbouring cable can skip the corridor. */
        if(src>=0 && dst>=0 && l->nodes[src].row==l->nodes[dst].row && dst==src+1 && sy==dy) {
            route->count=2; route->points[0]=(pedal_point_t){sx,sy};
            route->points[1]=(pedal_point_t){dx,dy}; continue;
        }
        const int sl=escape_x(l,src,true,r->source_port),dl=escape_x(l,dst,false,r->destination_port);
        route->count=6; route->points[0]=(pedal_point_t){sx,sy};
        route->points[1]=(pedal_point_t){sl,sy};
        route->points[2]=(pedal_point_t){sl,0};
        route->points[3]=(pedal_point_t){dl,0};
        route->points[4]=(pedal_point_t){dl,dy};
        route->points[5]=(pedal_point_t){dx,dy};
        if(src<0 && dst<0) {
            route->points[2].y=route->points[3].y=8+r->source_port*8;
            continue;
        }
        /* Interval coloring: disjoint horizontal runs may reuse a track.
         * Overlapping runs are separated by six pixels, never modulo-wrapped. */
        unsigned candidate=0;
        for(;candidate<SEEDFX_MAX_EDGES;++candidate) {
            bool busy=false;
            for(unsigned prev=0;prev<e;++prev) if(lane[prev]==(int)candidate) {
                const pedal_route_t *p=&l->routes[prev];
                if(max(sl,dl)+6>=min(p->points[2].x,p->points[3].x)
                   && max(p->points[2].x,p->points[3].x)+6>=min(sl,dl)) busy=true;
            }
            if(!busy) break;
        }
        lane[e]=candidate;
        l->lanes=max(l->lanes,candidate+1);
        route->points[2].y=route->points[3].y=244+candidate*6;
    }
    l->second_row_y=max(254,250+l->lanes*6);
    if(l->rows>1) l->canvas_height=max(490,l->second_row_y+(l->rows-2)*230+226);
    else l->canvas_height=max(490,260+l->lanes*6);
    for(unsigned e=0;e<g->edge_count;++e) {
        pedal_route_t *r=&l->routes[e]; if(!r->count) continue;
        const SeedFxEdgeDefinition *edge=&g->edges[e];
        const int sy=socket_y(l,g,node_index(g,edge->source_id),true,edge->source_port);
        const int dy=socket_y(l,g,node_index(g,edge->destination_id),false,edge->destination_port);
        r->points[0].y=sy; r->points[r->count-1].y=dy;
        if(r->count==6) {r->points[1].y=sy;r->points[4].y=dy;}
    }
    /* Short local doglegs are preferable to a trip through the centre bus,
     * but only after checking all reserved tracks (including crossed stereo). */
    for(unsigned e=0;e<g->edge_count;++e) {
        pedal_route_t *r=&l->routes[e]; if(r->count!=6) continue;
        const pedal_point_t from=r->points[0],to=r->points[5];
        const int dx=to.x-from.x,dy=to.y-from.y;
        if(!dx || dx<-50 || dx>50 || dy<-44 || dy>44) continue;
        pedal_route_t candidate={.points={from,r->points[1],
            {r->points[1].x,to.y},to},.count=4};
        bool busy=false;
        for(unsigned other=0;other<g->edge_count;++other)
            if(other!=e && parallel_collision(&candidate,&l->routes[other])) busy=true;
        if(!busy) *r=candidate;
    }
}
