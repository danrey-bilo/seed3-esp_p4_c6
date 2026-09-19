#pragma once
#include "seedfx_catalog.h"
#include <string.h>

static inline void seedfx_add_routing_catalog(SeedFxCatalogEntry *entries, size_t *count)
{
    for(unsigned type=SEEDFX_EFFECT_SPLITTER;type<=SEEDFX_EFFECT_MIXER;++type) {
        bool found=false;
        for(size_t n=0;n<*count;++n) if(entries[n].effect_type==type) found=true;
        if(found || *count>=SEEDFX_MAX_EFFECTS) continue;
        SeedFxCatalogEntry *fx=&entries[(*count)++];
        memset(fx,0,sizeof(*fx)); fx->effect_type=type; fx->package_id=0x10000+type;
        fx->color_rgb=type==SEEDFX_EFFECT_SPLITTER?0x63c9ff:0xffce68;
        fx->shape=type==SEEDFX_EFFECT_SPLITTER?SEEDFX_SHAPE_RECTANGLE:SEEDFX_SHAPE_ROUNDED;
        strcpy(fx->name,type==SEEDFX_EFFECT_SPLITTER?"Splitter":"Mixer");
        strcpy(fx->category,"routing"); strcpy(fx->cpu_label,"Light CPU");
        if(type==SEEDFX_EFFECT_MIXER) {
            fx->parameter_count=3;
            for(unsigned p=0;p<3;++p) {
                strcpy(fx->parameters[p].name,p==0?"Branch A":p==1?"Branch B":"Master");
                strcpy(fx->parameters[p].unit,"%");
                fx->parameters[p].maximum=1; fx->parameters[p].step=.01f;
                fx->parameters[p].default_value=p==2?1:.5f;
            }
        }
    }
}
