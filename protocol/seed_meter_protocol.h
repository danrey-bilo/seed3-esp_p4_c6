#pragma once
#include "seedfx_graph_protocol.h"
#include <string.h>

/* UART-only ADC/DAC peak telemetry; no change to the SPI wire frame. */
enum { SEED_METER_LINE_BYTES=58 };
static inline void seed_meter_hex(char *out,uint32_t value)
{
    static const char digits[]="0123456789ABCDEF";
    for(unsigned i=0;i<8;++i) out[i]=digits[(value>>(28-4*i))&15];
}
static inline size_t seed_meter_format(char out[SEED_METER_LINE_BYTES],const uint32_t peaks[4])
{
    memcpy(out,"SEED:LEVEL ",11);
    for(unsigned i=0;i<4;++i) {seed_meter_hex(out+11+9*i,peaks[i]);out[19+9*i]=' ';}
    seed_meter_hex(out+47,seedfx_crc32(peaks,4*sizeof(uint32_t)));
    out[55]='\r';out[56]='\n';out[57]=0;return 57;
}
static inline bool seed_meter_parse(const char *line,uint32_t peaks[4])
{
    if(strlen(line)!=55 || strncmp(line,"SEED:LEVEL ",11)) return false;
    uint32_t values[5]={0};
    for(unsigned n=0;n<5;++n) {
        for(unsigned i=0;i<8;++i) {
            const char c=line[11+n*9+i];
            const unsigned v=c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:99;
            if(v>15) return false;
            values[n]=(values[n]<<4)|v;
        }
        if(n<4 && (line[19+n*9]!=' ' || values[n]>0x7fffffffU)) return false;
    }
    if(values[4]!=seedfx_crc32(values,4*sizeof(uint32_t))) return false;
    memcpy(peaks,values,4*sizeof(uint32_t));return true;
}
