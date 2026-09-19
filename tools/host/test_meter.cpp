#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "stm32h7xx.h"
#include "seed_meter_protocol.h"
#include "seed_level_meter.h"

std::string meter_uart_bytes;
MeterUart meter_uart={0,{}};

int main()
{
    char line[SEED_METER_LINE_BYTES];
    const uint32_t expected[4]={0,1,0x40000000,0x7fffffff};
    uint32_t decoded[4]={};
    assert(seed_meter_format(line,expected)==57);
    assert(line[55]=='\r' && line[56]=='\n');
    line[55]=0;
    assert(seed_meter_parse(line,decoded) && !std::memcmp(expected,decoded,sizeof(decoded)));
    for(unsigned i=0;i<55;++i) {
        const char original=line[i]; line[i]=original=='0'?'1':'0';
        assert(!seed_meter_parse(line,decoded)); line[i]=original;
    }
    for(unsigned i=0;i<55;++i) {
        const char original=line[i];line[i]=0;
        assert(!seed_meter_parse(line,decoded));line[i]=original;
    }
    float in0[2]={-.25f,.125f},in1[2]={1.f,1.f};
    float out0[2]={-1.25f,.5f},out1[2]={.5f,-.75f};
    const float *in[2]={in0,in1};float *out[2]={out0,out1};
    seed_level_meter_capture(in,out,2,1,3);
    seed_level_meter_poll(40); // UART not ready: zero writes, no busy-wait.
    assert(meter_uart_bytes.empty());
    meter_uart.ISR=USART_ISR_TXE_TXFNF;
    for(unsigned i=0;i<4;++i) {
        const auto before=meter_uart_bytes.size();
        seed_level_meter_poll(41+i);
        assert(meter_uart_bytes.size()-before<=16);
    }
    assert(meter_uart_bytes.size()==57);
    meter_uart_bytes.resize(55);
    assert(seed_meter_parse(meter_uart_bytes.c_str(),decoded));
    assert(decoded[0]>0x1ffff000 && decoded[0]<=0x20000000);
    assert(decoded[1]==0 && decoded[2]>0x7ffff000 && decoded[3]>0x5ffff000);
    meter_uart_bytes.clear();
    seed_level_meter_poll(80);
    assert(meter_uart_bytes.size()==16 && seed_level_meter_cancel_tx());
    assert(!seed_level_meter_cancel_tx());
    seed_level_meter_poll(81);assert(meter_uart_bytes.size()==16);
    puts("PASS: physical telemetry CRC, malformed lines, channel masks, clipping, bounded nonblocking TX");
}
