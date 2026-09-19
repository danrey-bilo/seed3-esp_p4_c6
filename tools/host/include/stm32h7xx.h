#pragma once
#include <cstdint>
#include <string>
/* Test-only USART register model for the bounded foreground TX pump. */
extern std::string meter_uart_bytes;
struct MeterTxRegister {
    void operator=(uint8_t value) { meter_uart_bytes.push_back((char)value); }
};
struct MeterUart { uint32_t ISR; MeterTxRegister TDR; };
extern MeterUart meter_uart;
#define USART1 (&meter_uart)
#define USART_ISR_TXE_TXFNF (1U << 7)
