#pragma once
#include <cstddef>
#include <cstdint>

/* Audio ISR: bounded peak scan and four atomic maxima, no conversion/formatting.
 * Foreground: send at most 16 ready UART bytes per poll, never wait for TX. */
void seed_level_meter_capture(const float* const input[2],float* const output[2],
                              std::size_t frames,uint8_t input_mask,uint8_t output_mask);
void seed_level_meter_poll(uint32_t now_ms);
/* Drop an unfinished telemetry line before an existing diagnostic message.
 * Caller inserts a newline if true. Only the foreground owns the TX buffer. */
bool seed_level_meter_cancel_tx();
