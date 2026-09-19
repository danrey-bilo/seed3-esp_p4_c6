#include "seed_level_meter.h"
#include "seed_meter_protocol.h"
#include "stm32h7xx.h"
#include <cmath>
#include <cstring>

namespace {
volatile uint32_t peak_bits[4];
char line[SEED_METER_LINE_BYTES];
unsigned sent=0,length=0;
uint32_t last_ms=0;
void publish(unsigned index,float value)
{
    uint32_t bits; std::memcpy(&bits,&value,sizeof(bits));
    uint32_t old=__atomic_load_n(&peak_bits[index],__ATOMIC_RELAXED);
    while(bits>old && !__atomic_compare_exchange_n(&peak_bits[index],&old,bits,false,
                                                  __ATOMIC_RELEASE,__ATOMIC_RELAXED)) {}
}
}

void seed_level_meter_capture(const float* const input[2],float* const output[2],
                              std::size_t frames,uint8_t input_mask,uint8_t output_mask)
{
    float peak[4]={};
    for(unsigned ch=0;ch<2;++ch) for(std::size_t i=0;i<frames;++i) {
        const float in=(input_mask&(1U<<ch))?std::fabs(input[ch][i]):0;
        const float out=(output_mask&(1U<<ch))?std::fabs(output[ch][i]):0;
        if(in>peak[ch]) peak[ch]=in;
        if(out>peak[2+ch]) peak[2+ch]=out;
    }
    for(unsigned i=0;i<4;++i) publish(i,peak[i]);
}

bool seed_level_meter_cancel_tx()
{
    const bool pending=sent<length;
    sent=length=0;
    return pending;
}

void seed_level_meter_poll(uint32_t now_ms)
{
    if(sent==length && now_ms-last_ms>=40) {
        uint32_t peaks[4];
        for(unsigned i=0;i<4;++i) {
            const uint32_t bits=__atomic_exchange_n(&peak_bits[i],0U,__ATOMIC_ACQ_REL);
            float value; std::memcpy(&value,&bits,sizeof(value));
            if(!std::isfinite(value) || value>1) value=1;
            peaks[i]=(uint32_t)(value*2147483392.0f);
        }
        length=seed_meter_format(line,peaks);sent=0;last_ms=now_ms;
    }
    /* USART1 is already configured by libDaisy, RX remains circular DMA.
     * There is no TX DMA or TX interrupt owner in this firmware. */
    for(unsigned budget=0;budget<16 && sent<length;++budget) {
        if(!(USART1->ISR & USART_ISR_TXE_TXFNF)) break;
        USART1->TDR=(uint8_t)line[sent++];
    }
}
