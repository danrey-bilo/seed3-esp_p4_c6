#pragma once
/* Pure bounded graph operations shared by the P4 editor and Seed3 runtime.
 * No allocation, audio transport or UI dependencies. */
#include "seedfx_ports.h"
#include <math.h>
#include <string.h>

static inline int seedfx_find_node(const SeedFxGraphDefinition *g, uint16_t id)
{
    for (unsigned n=0; n<g->node_count; ++n) if(g->nodes[n].id==id) return (int)n;
    return -1;
}

static inline unsigned seedfx_port_count(const SeedFxGraphDefinition *g, uint16_t id, bool output)
{
    if (!id) return 2;
    const int n=seedfx_find_node(g,id);
    if(n<0) return 0;
    return output ? seedfx_output_channels(g->nodes[n].effect_type)
                  : seedfx_input_channels(g->nodes[n].effect_type);
}

/* One wire per input jack; fan-out from any output is allowed. To sum signals
 * use the explicit Mixer node rather than hidden summation at an input. */
static inline bool seedfx_routes_valid(const SeedFxGraphDefinition *g)
{
    if(g->node_count>SEEDFX_MAX_NODES || g->edge_count>SEEDFX_MAX_EDGES) return false;
    unsigned indegree[SEEDFX_MAX_NODES]={0};
    for(unsigned n=0;n<g->node_count;++n) {
        if(!g->nodes[n].id || g->nodes[n].effect_type<1 || g->nodes[n].effect_type>SEEDFX_EFFECT_LAST
           ||g->nodes[n].parameter_count>SEEDFX_MAX_PARAMS) return false;
        for(unsigned p=0;p<g->nodes[n].parameter_count;++p)
            if(!isfinite(g->nodes[n].parameters[p])) return false;
        for(unsigned p=0;p<n;++p) if(g->nodes[p].id==g->nodes[n].id) return false;
    }
    for(unsigned e=0;e<g->edge_count;++e) {
        const SeedFxEdgeDefinition *r=&g->edges[e];
        if(r->source_port>=seedfx_port_count(g,r->source_id,true)
           ||r->destination_port>=seedfx_port_count(g,r->destination_id,false)
           ||!isfinite(r->gain) || r->gain < -4 || r->gain > 4
           || (r->source_id && r->source_id==r->destination_id)) return false;
        for(unsigned p=0;p<e;++p)
            if(g->edges[p].destination_id==r->destination_id
               &&g->edges[p].destination_port==r->destination_port) return false;
        if(r->source_id && r->destination_id) ++indegree[seedfx_find_node(g,r->destination_id)];
    }
    unsigned queue[SEEDFX_MAX_NODES],head=0,tail=0;
    for(unsigned n=0;n<g->node_count;++n) if(!indegree[n]) queue[tail++]=n;
    while(head<tail) {
        const uint16_t id=g->nodes[queue[head++]].id;
        for(unsigned e=0;e<g->edge_count;++e) if(g->edges[e].source_id==id && g->edges[e].destination_id) {
            const int n=seedfx_find_node(g,g->edges[e].destination_id);
            if(--indegree[n]==0) queue[tail++]=(unsigned)n;
        }
    }
    return tail==g->node_count;
}

static inline void seedfx_disconnect_input(SeedFxGraphDefinition *g, uint16_t id, uint8_t port)
{
    for(unsigned e=0;e<g->edge_count;) {
        if(g->edges[e].destination_id==id && g->edges[e].destination_port==port) {
            memmove(&g->edges[e],&g->edges[e+1],(--g->edge_count-e)*sizeof(g->edges[0]));
            memset(&g->edges[g->edge_count],0,sizeof(g->edges[0]));
        } else ++e;
    }
}

static inline bool seedfx_connect(SeedFxGraphDefinition *g, uint16_t from, uint8_t out,
                                  uint16_t to, uint8_t in)
{
    /* Validate a complete candidate before touching a live graph. */
    if(!seedfx_routes_valid(g)) return false;
    SeedFxGraphDefinition next=*g;
    seedfx_disconnect_input(&next,to,in);
    if(next.edge_count>=SEEDFX_MAX_EDGES) return false;
    SeedFxEdgeDefinition edge={0};
    edge.source_id=from; edge.destination_id=to; edge.gain=1;
    edge.source_port=out; edge.destination_port=in;
    next.edges[next.edge_count++]=edge;
    if(!seedfx_routes_valid(&next)) return false;
    *g=next;
    return true;
}

static inline void seedfx_prune_invalid_ports(SeedFxGraphDefinition *g)
{
    for(unsigned e=0;e<g->edge_count;) {
        const SeedFxEdgeDefinition *r=&g->edges[e];
        if(r->source_port>=seedfx_port_count(g,r->source_id,true)
           ||r->destination_port>=seedfx_port_count(g,r->destination_id,false)) {
            memmove(&g->edges[e],&g->edges[e+1],(--g->edge_count-e)*sizeof(g->edges[0]));
            memset(&g->edges[g->edge_count],0,sizeof(g->edges[0]));
        } else ++e;
    }
}

static inline uint16_t seedfx_next_node_id(const SeedFxGraphDefinition *g)
{
    for(uint16_t id=1;id<=SEEDFX_MAX_NODES+1;++id)
        if(seedfx_find_node(g,id)<0) return id;
    return 0;
}

/* Exact legacy layout; convert old stereo edges once, before validation. */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t magic; uint16_t version,bytes; uint32_t revision;
    uint8_t flags,node_count,edge_count,reserved; char name[24];
    SeedFxNodeDefinition nodes[12];
    struct __attribute__((packed)) {uint16_t source_id,destination_id; float gain;} edges[24];
    uint32_t crc32;
} SeedFxLegacyGraph;

static inline bool seedfx_graph_decode(const void *data, size_t bytes, SeedFxGraphDefinition *g)
{
    if(bytes==sizeof(*g)) {
        memcpy(g,data,bytes);
        return seedfx_graph_header_is_valid(g) && seedfx_graph_crc32(g)==g->crc32 && seedfx_routes_valid(g);
    }
    if(bytes!=sizeof(SeedFxLegacyGraph)) return false;
    SeedFxLegacyGraph old; memcpy(&old,data,sizeof(old));
    if(old.magic!=SEEDFX_GRAPH_MAGIC || old.version!=1 || old.bytes!=sizeof(old)
       ||old.node_count>12 ||old.edge_count>24
       ||seedfx_crc32(&old,offsetof(SeedFxLegacyGraph,crc32))!=old.crc32) return false;
    memset(g,0,sizeof(*g)); memcpy(g,&old,40+sizeof(old.nodes));
    g->version=SEEDFX_GRAPH_VERSION; g->bytes=sizeof(*g); g->edge_count=0;
    /* The old renderer treated a graph without edges as stereo thru. Only
     * migration retains that convention; an empty v2 route list is silence. */
    if(old.edge_count==0) {
        for(unsigned p=0;p<2;++p) {
            SeedFxEdgeDefinition r={0};
            r.gain=1; r.source_port=p; r.destination_port=p;
            g->edges[g->edge_count++]=r;
        }
    }
    for(unsigned e=0;e<old.edge_count;++e) {
        const unsigned src=seedfx_port_count(g,old.edges[e].source_id,true);
        const unsigned dst=seedfx_port_count(g,old.edges[e].destination_id,false);
        if(!src || !dst) return false;
        const int n=seedfx_find_node(g,old.edges[e].destination_id);
        for(unsigned p=0;p<dst;++p) {
            SeedFxEdgeDefinition r={0};
            r.source_id=old.edges[e].source_id; r.destination_id=old.edges[e].destination_id;
            r.gain=old.edges[e].gain; r.destination_port=p;
            r.source_port=seedfx_route_channel(src,dst,n<0?0:g->nodes[n].reserved,p);
            g->edges[g->edge_count++]=r;
        }
    }
    g->crc32=seedfx_graph_crc32(g);
    return seedfx_routes_valid(g);
}
