#include "pedal_patch.h"

static bool same_edge(const SeedFxEdgeDefinition *a,const SeedFxEdgeDefinition *b)
{
    return a->source_id==b->source_id && a->destination_id==b->destination_id
        && a->source_port==b->source_port && a->destination_port==b->destination_port;
}

static unsigned connections(const SeedFxGraphDefinition *g,pedal_jack_t j,SeedFxEdgeDefinition *found)
{
    unsigned count=0;
    for(unsigned n=0;n<g->edge_count;++n) {
        const SeedFxEdgeDefinition *e=&g->edges[n];
        if(j.output ? e->source_id==j.id && e->source_port==j.port
                    : e->destination_id==j.id && e->destination_port==j.port) {
            if(found) *found=*e;
            ++count;
        }
    }
    return count;
}

bool pedal_patch_is_selected(const pedal_patch_t *s,pedal_jack_t j)
{
    return s->active && s->jack.id==j.id && s->jack.port==j.port && s->jack.output==j.output;
}

pedal_patch_result_t pedal_patch_tap(pedal_patch_t *s,const SeedFxGraphDefinition *g,
                                    pedal_jack_t j,SeedFxEdgeDefinition *edit)
{
    if(j.port>=seedfx_port_count(g,j.id,j.output)) {s->active=false;return PEDAL_PATCH_CANCEL;}
    if(!s->active) {s->active=true;s->jack=j;return PEDAL_PATCH_SELECTED;}
    const pedal_jack_t first=s->jack;
    s->active=false;
    /* No implicit replacement/fan-out: use Splitter. Existing fan-out presets
     * remain valid but must be unplugged from individual destination jacks. */
    if(first.output==j.output || connections(g,first,NULL) || connections(g,j,NULL))
        return PEDAL_PATCH_CANCEL;
    const pedal_jack_t from=first.output?first:j, to=first.output?j:first;
    SeedFxGraphDefinition candidate=*g;
    if(!seedfx_connect(&candidate,from.id,from.port,to.id,to.port)) return PEDAL_PATCH_CANCEL;
    *edit=(SeedFxEdgeDefinition){from.id,to.id,1,from.port,to.port,0};
    return PEDAL_PATCH_CONNECT;
}

pedal_patch_result_t pedal_patch_blank(pedal_patch_t *s,const SeedFxGraphDefinition *g,
                                      SeedFxEdgeDefinition *edit)
{
    if(!s->active) return PEDAL_PATCH_CANCEL;
    const unsigned count=connections(g,s->jack,edit);
    s->active=false;
    return count==1?PEDAL_PATCH_DISCONNECT:PEDAL_PATCH_CANCEL;
}

void pedal_colors_sync(pedal_colors_t *c,const SeedFxGraphDefinition *g)
{
    for(unsigned i=0;i<SEEDFX_MAX_EDGES;++i) if(c->slots[i].used) {
        bool keep=false;
        for(unsigned n=0;n<g->edge_count;++n) keep|=same_edge(&c->slots[i].edge,&g->edges[n]);
        c->slots[i].used=keep;
    }
    for(unsigned n=0;n<g->edge_count;++n) {
        bool found=false;
        for(unsigned i=0;i<SEEDFX_MAX_EDGES;++i)
            found |= c->slots[i].used && same_edge(&c->slots[i].edge,&g->edges[n]);
        if(found) continue;
        for(unsigned i=0;i<SEEDFX_MAX_EDGES;++i) if(!c->slots[i].used) {
            c->slots[i].used=true;c->slots[i].edge=g->edges[n];break;
        }
    }
}

uint32_t pedal_color_for(const pedal_colors_t *c,const SeedFxEdgeDefinition *e)
{
    if(!e->source_id) return pedal_physical_color(e->source_port);
    if(!e->destination_id) return pedal_physical_color(e->destination_port);
    for(unsigned i=0;i<SEEDFX_MAX_EDGES;++i) if(c->slots[i].used && same_edge(&c->slots[i].edge,e)) {
        /* 48 distinct RGB colors; coprime hue stepping separates neighbours. */
        const unsigned h=((i*19)%48)*32, sector=h/256, f=h%256;
        const unsigned low=70, high=246, up=low+(high-low)*f/256, down=high-(high-low)*f/256;
        const unsigned rgb[6][3]={{high,up,low},{down,high,low},{low,high,up},
                                  {low,down,high},{up,low,high},{high,low,down}};
        return (rgb[sector][0]<<16)|(rgb[sector][1]<<8)|rgb[sector][2];
    }
    return 0x708090;
}
