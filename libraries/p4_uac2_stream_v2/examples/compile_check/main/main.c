#include "p4_uac2_stream.h"

_Static_assert(P4_UAC2_CHANNELS == 2, "Stereo source required");

// Called by ONE existing transport task for each real 32-frame source block.
// Implement the rate command/ack in your transport, outside the USB component.
uint32_t example_rate_to_request_from_seed(void)
{
    return p4_uac2_requested_rate();
}

void example_seed_block_in_task(uint32_t confirmed_rate, bool new_session,
                               const int32_t capture[64], int32_t playback[64])
{
    p4_uac2_source_rate(confirmed_rate, new_session);
    (void)p4_uac2_capture_write(capture, 32);
    (void)p4_uac2_playback_read(playback, 32);
}

void app_main(void)
{
    ESP_ERROR_CHECK(p4_uac2_init());
    // Compile/link example only. There is no SPI driver here: capture waits
    // for source confirmation/prefill. Do NOT flash this over the real bridge.
}
