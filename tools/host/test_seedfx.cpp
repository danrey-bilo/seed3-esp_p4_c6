#include "../../Seed3/src/seedfx_graph.h"
#include "seedfx_extensions.h"
#include "seedfx_ports.h"
#include "seedfx_routing.h"
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "catalog_cases.h"

static SeedFxGraphDefinition graph(uint16_t type, const float *parameters, unsigned count)
{
    SeedFxGraphDefinition g{};
    g.magic=SEEDFX_GRAPH_MAGIC; g.version=SEEDFX_GRAPH_VERSION; g.bytes=sizeof(g);
    g.flags=SEEDFX_GRAPH_ENABLED; g.revision=1; g.node_count=1;
    g.nodes[0].id=1; g.nodes[0].effect_type=type; g.nodes[0].flags=SEEDFX_NODE_ENABLED;
    g.nodes[0].parameter_count=count; std::memcpy(g.nodes[0].parameters,parameters,count*sizeof(float));
    for(unsigned ch=0;ch<seedfx_input_channels(type);++ch)
        g.edges[g.edge_count++]={0,1,1,(uint8_t)ch,(uint8_t)ch,0};
    for(unsigned ch=0;ch<2;++ch)
        g.edges[g.edge_count++]={1,0,1,(uint8_t)(seedfx_output_channels(type)==1?0:ch),(uint8_t)ch,0};
    g.crc32=seedfx_graph_crc32(&g);
    return g;
}

static double render(const SeedFxGraphDefinition &g, float *record)
{
    seedfx_graph_init(); assert(seedfx_graph_stage(g)==SeedFxGraphResult::Ok);
    float in[2][32]{},out[2][32]{};
    const float *input[2]={in[0],in[1]}; float *output[2]={out[0],out[1]};
    double energy=0;
    for(unsigned block=0;block<48000/32;++block) {
        for(unsigned i=0;i<32;++i) {
            unsigned t=block*32+i;
            in[0][i]=.2f*std::sin(t*.13050773f);
            in[1][i]=.17f*std::sin(t*.1964801f);
        }
        seedfx_graph_process(input,output,32,48000,true);
        for(unsigned i=0;i<32;++i) for(unsigned ch=0;ch<2;++ch) {
            assert(std::isfinite(out[ch][i]) && std::fabs(out[ch][i])<100);
            record[(block*32+i)*2+ch]=out[ch][i]; energy+=out[ch][i]*out[ch][i];
        }
    }
    return energy;
}

static void test_routing()
{
    float params[4]={0,0,0,0};
    auto g=graph(SEEDFX_EFFECT_GAIN,params,1);
    g.node_count=2; g.nodes[1]=g.nodes[0]; g.nodes[1].id=9;
    g.nodes[1].effect_type=SEEDFX_EFFECT_POLARITY; g.nodes[1].parameter_count=0;
    g.edge_count=0;
    assert(seedfx_connect(&g,0,0,1,0)); assert(seedfx_connect(&g,1,0,0,0));
    assert(seedfx_connect(&g,0,1,9,0)); assert(seedfx_connect(&g,9,0,0,1));
    auto before=g;
    assert(!seedfx_connect(&g,1,0,1,0)); assert(std::memcmp(&g,&before,sizeof(g))==0);
    assert(!seedfx_connect(&g,44,0,1,0));
    float in[2][32],out[2][32]; const float* input[]={in[0],in[1]}; float* output[]={out[0],out[1]};
    for(unsigned i=0;i<32;++i) {in[0][i]=.25f; in[1][i]=-.75f;}
    g.crc32=seedfx_graph_crc32(&g); seedfx_graph_init();
    assert(seedfx_graph_stage(g)==SeedFxGraphResult::Ok);
    seedfx_graph_process(input,output,32,96000,true);
    for(unsigned i=0;i<32;++i) { assert(out[0][i]==.25f); assert(out[1][i]==.75f); }
    /* Split -> two opposite-polarity branches -> Mixer cancellation, while
     * physical input/output 2 remain independent. Deliberately unsorted IDs. */
    g.node_count=4; g.edge_count=0;
    g.nodes[0].effect_type=SEEDFX_EFFECT_SPLITTER; g.nodes[0].parameter_count=0;
    g.nodes[2]=g.nodes[1]; g.nodes[2].id=3; g.nodes[2].effect_type=SEEDFX_EFFECT_BYPASS;
    g.nodes[3]=g.nodes[1]; g.nodes[3].id=7; g.nodes[3].effect_type=SEEDFX_EFFECT_MIXER;
    g.nodes[3].parameter_count=3; g.nodes[3].parameters[0]=g.nodes[3].parameters[1]=.5f;
    g.nodes[3].parameters[2]=1;
    assert(seedfx_connect(&g,0,0,1,0)); assert(seedfx_connect(&g,1,0,3,0));
    assert(seedfx_connect(&g,1,1,9,0)); assert(seedfx_connect(&g,3,0,7,0));
    assert(seedfx_connect(&g,9,0,7,1)); assert(seedfx_connect(&g,7,0,0,0));
    assert(seedfx_connect(&g,0,1,0,1));
    before=g; assert(!seedfx_connect(&g,7,0,1,0)); assert(std::memcmp(&g,&before,sizeof(g))==0);
    assert(!seedfx_connect(&g,0,0,1,1)); // Splitter has only one input
    for(uint32_t rate : {44100U,48000U,88200U,96000U}) {
        seedfx_graph_init(); g.crc32=seedfx_graph_crc32(&g);
        assert(seedfx_graph_stage(g)==SeedFxGraphResult::Ok);
        seedfx_graph_process(input,output,32,rate,true);
        for(unsigned i=0;i<32;++i) {assert(out[0][i]==0);assert(out[1][i]==-.75f);}
    }
    seedfx_disconnect_input(&g,7,1); g.crc32=seedfx_graph_crc32(&g);
    assert(seedfx_graph_stage(g)==SeedFxGraphResult::Ok);
    seedfx_graph_process(input,output,32,48000,true);
    assert(out[0][0]==.125f && out[1][0]==-.75f);
    seedfx_graph_process(input,output,32,48000,true,0); assert(out[0][0]==0 && out[1][0]==0);
    g.edge_count=0; g.crc32=seedfx_graph_crc32(&g);
    assert(seedfx_graph_stage(g)==SeedFxGraphResult::Ok); seedfx_graph_process(input,output,32,48000,true);
    assert(out[0][0]==0 && out[1][0]==0); // no implicit reconnection
    SeedFxLegacyGraph legacy{}; legacy.magic=SEEDFX_GRAPH_MAGIC; legacy.version=1;
    legacy.bytes=sizeof(legacy); legacy.edge_count=1; legacy.edges[0]={0,0,1};
    legacy.crc32=seedfx_crc32(&legacy,offsetof(SeedFxLegacyGraph,crc32));
    assert(sizeof(legacy)==572 && seedfx_graph_decode(&legacy,sizeof(legacy),&g));
    assert(g.edge_count==2 && g.edges[1].source_port==1 && g.edges[1].destination_port==1);
    legacy.edge_count=0;
    legacy.crc32=seedfx_crc32(&legacy,offsetof(SeedFxLegacyGraph,crc32));
    assert(seedfx_graph_decode(&legacy,sizeof(legacy),&g) && g.edge_count==2);
    legacy.crc32^=1; assert(!seedfx_graph_decode(&legacy,sizeof(legacy),&g));
    puts("PASS: independent channels, split/mix cancellation, 4 rates, cycles, silent disconnect, v1 migration");
}

static void test_maximum_graphs()
{
    uint32_t random=0x34f89721U;
    auto next=[&]() { random=random*1664525U+1013904223U; return random; };
    const uint16_t types[]={SEEDFX_EFFECT_SPLITTER,SEEDFX_EFFECT_MIXER,
                            SEEDFX_EFFECT_POLARITY,SEEDFX_EFFECT_GAIN};
    for(unsigned trial=0;trial<64;++trial) {
        SeedFxGraphDefinition g{};
        g.magic=SEEDFX_GRAPH_MAGIC; g.version=SEEDFX_GRAPH_VERSION; g.bytes=sizeof(g);
        g.flags=SEEDFX_GRAPH_ENABLED; g.node_count=SEEDFX_MAX_NODES;
        float reference[SEEDFX_MAX_NODES+1][2]={{.25f,-.75f}};
        for(unsigned n=0;n<SEEDFX_MAX_NODES;++n) {
            auto &node=g.nodes[n]; node.id=100+17*n; node.effect_type=types[(n+trial)%4];
            node.flags=SEEDFX_NODE_ENABLED;
            if(node.effect_type==SEEDFX_EFFECT_MIXER) {
                node.parameter_count=3; node.parameters[0]=.2f; node.parameters[1]=.8f; node.parameters[2]=.9f;
            } else if(node.effect_type==SEEDFX_EFFECT_GAIN) node.parameter_count=1;
            float inputs[2]={};
            for(unsigned p=0;p<seedfx_input_channels(node.effect_type);++p) {
                const unsigned source=next()%(n+1);
                const unsigned port=next()%(source?seedfx_output_channels(g.nodes[source-1].effect_type):2);
                g.edges[g.edge_count++]={uint16_t(source?g.nodes[source-1].id:0),node.id,1,(uint8_t)port,(uint8_t)p,0};
                inputs[p]=reference[source][port];
            }
            float *r=reference[n+1];
            if(node.effect_type==SEEDFX_EFFECT_SPLITTER) r[0]=r[1]=inputs[0];
            if(node.effect_type==SEEDFX_EFFECT_MIXER) r[0]=(inputs[0]*.2f+inputs[1]*.8f)*.9f;
            if(node.effect_type==SEEDFX_EFFECT_POLARITY) {r[0]=-inputs[0];r[1]=-inputs[1];}
            if(node.effect_type==SEEDFX_EFFECT_GAIN) {r[0]=inputs[0];r[1]=inputs[1];}
        }
        g.edges[g.edge_count++]={g.nodes[11].id,0,1,0,0,0};
        g.edges[g.edge_count++]={g.nodes[10].id,0,1,0,1,0};
        for(unsigned n=0;n<6;++n) {auto tmp=g.nodes[n];g.nodes[n]=g.nodes[11-n];g.nodes[11-n]=tmp;}
        g.crc32=seedfx_graph_crc32(&g); assert(seedfx_routes_valid(&g));
        seedfx_graph_init(); assert(seedfx_graph_stage(g)==SeedFxGraphResult::Ok);
        float a[32],b[32],left[32],right[32];
        for(unsigned n=0;n<32;++n) {a[n]=.25f;b[n]=-.75f;}
        const float *input[]={a,b}; float *output[]={left,right};
        for(uint8_t mask=0;mask<4;++mask) {
            seedfx_graph_process(input,output,32,96000,true,mask);
            for(unsigned n=0;n<32;++n) {
                assert(std::fabs(left[n]-((mask&1)?reference[12][0]:0))<1e-6f);
                assert(std::fabs(right[n]-((mask&2)?reference[11][0]:0))<1e-6f);
            }
        }
    }
    puts("PASS: 64 maximum-size DAGs, reversed node order and all output masks match scalar reference");
}

int main()
{
    test_routing();
    test_maximum_graphs();
    assert(seedfx_input_channels(SEEDFX_EFFECT_OVERDRIVE)==1);
    assert(seedfx_output_channels(SEEDFX_EFFECT_CHORUS)==2);
    assert(seedfx_input_channels(SEEDFX_EFFECT_CHORUS)==1);
    assert(seedfx_route_channel(2,1,1,0)==1);
    assert(seedfx_route_channel(1,2,0,1)==0);
    /* An impulse already in delay RAM must survive a knob update. */
    seedfx_graph_init(); float params[4]={1,0,1,1};
    auto g=graph(SEEDFX_EFFECT_DELAY,params,4);
    assert(seedfx_graph_stage(g)==SeedFxGraphResult::Ok);
    float in[2][32]{},out[2][32]{};
    const float *input[2]={in[0],in[1]}; float *output[2]={out[0],out[1]};
    /* A bypassed mono-input pedal must take ONLY its selected input and fan
     * out the mono output, not accidentally preserve the other source. */
    auto mono=graph(SEEDFX_EFFECT_OVERDRIVE,params,3);
    mono.nodes[0].flags=0;
    for(unsigned selected=0;selected<2;++selected) {
        seedfx_graph_init(); mono.edges[0].source_port=selected;
        mono.crc32=seedfx_graph_crc32(&mono);
        assert(seedfx_graph_stage(mono)==SeedFxGraphResult::Ok);
        for(unsigned i=0;i<32;++i) { in[0][i]=.25f; in[1][i]=-.75f; }
        seedfx_graph_process(input,output,32,48000,true);
        for(unsigned i=0;i<32;++i) {
            assert(out[0][i]==in[selected][i]); assert(out[1][i]==in[selected][i]);
        }
    }
    seedfx_graph_init(); assert(seedfx_graph_stage(g)==SeedFxGraphResult::Ok);
    std::memset(in,0,sizeof(in));
    in[0][0]=1; seedfx_graph_process(input,output,32,48000,true);
    std::memset(in,0,sizeof(in));
    g.nodes[0].parameters[3]=.7f; ++g.revision; g.crc32=seedfx_graph_crc32(&g);
    assert(seedfx_graph_stage(g)==SeedFxGraphResult::Ok);
    seedfx_graph_process(input,output,32,48000,true);
    assert(out[0][16]>.5f);
    static float baseline[96000], adjusted[96000];
    for(const auto &c : cases) {
        const auto base=graph(c.type,c.params,c.count);
        render(base,baseline);
        for(const auto &ext : seedfx_extensions) if(ext.type==c.type) {
            auto changed=base;
            changed.nodes[0].parameters[ext.index]=ext.parameter.default_value==ext.parameter.maximum
                ? ext.parameter.minimum : ext.parameter.maximum;
            changed.crc32=seedfx_graph_crc32(&changed);
            render(changed,adjusted);
            double difference=0;
            for(unsigned i=0;i<96000;++i) difference+=std::fabs(baseline[i]-adjusted[i]);
            if(difference<.001) std::printf("No effect: type=%u parameter=%s\n",c.type,ext.parameter.name);
            assert(difference>.001);
        }
    }
    puts("PASS: 42 DSP algorithms finite, all 14 appended knobs affect sound, delay tail preserved");
}
