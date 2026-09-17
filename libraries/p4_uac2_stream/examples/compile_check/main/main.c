#include "p4_uac2_stream.h"

_Static_assert(P4_UAC2_CHANNELS == 2, "This example expects stereo");
_Static_assert(P4_UAC2_RATE == 48000, "This example expects 48 kHz");

// Integration sketch, NOT a replacement for the tested SPI firmware.
// Your existing SPI task calls this once per real 32-frame Seed3 block.
// Do not call it from a timer or ISR and do not retry a rejected capture block.
void example_seed_block_in_task(const int32_t capture[64], int32_t playback[64])
{
    (void)p4_uac2_capture_write(capture, 32);
    (void)p4_uac2_playback_read(playback, 32);
}

// A separate low-priority task can read/log these counters.
void example_read_status(p4_uac2_stats_t *stats, bool *capture, bool *playback)
{
    p4_uac2_get_stats(stats);
    *capture = p4_uac2_capture_active();
    *playback = p4_uac2_playback_active();
}

void app_main(void)
{
    ESP_ERROR_CHECK(p4_uac2_init());
    // Compile/link check only: no SPI driver or synthetic clock is provided.
    // Without a real source capture remains in prefill; do not flash this over
    // the working bridge expecting a complete Seed3/P4 audio firmware.
}
