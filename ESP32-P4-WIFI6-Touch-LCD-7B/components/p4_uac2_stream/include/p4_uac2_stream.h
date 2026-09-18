#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

// Legacy default constants; query get_stats for the live rate and prefill.
enum { P4_UAC2_RATE = 48000, P4_UAC2_CHANNELS = 2,
       P4_UAC2_PREFILL = 96, P4_UAC2_RING_FRAMES = 512 };

typedef struct {
    uint32_t initialization_state; // 0 off, 1 starting, 2 running, 3 failed
    uint32_t sample_rate, source_sample_rate, rate_changes, source_restarts;
    // Last rate actually selected when each Windows endpoint was opened.
    // The physical Seed clock remains common to both directions.
    uint32_t capture_sample_rate, playback_sample_rate;
    uint32_t capture_bits, playback_bits, prefill_frames, buffer_ms;
    uint32_t capture_source_frames, capture_completed_frames;
    uint32_t capture_packets, capture_short_packets, capture_long_packets;
    uint32_t capture_overrun_frames, capture_silence_frames, capture_discard_frames;
    uint32_t playback_received_frames, playback_consumed_frames;
    uint32_t playback_packets, playback_short_packets, playback_long_packets;
    uint32_t playback_overrun_frames, playback_underruns, playback_bad_packets;
    uint32_t capture_incomplete, playback_incomplete, feedback_incomplete;
    uint32_t endpoint_recoveries, controller_restarts, controller_faults;
    uint32_t capture_opens, playback_opens, usb_resets, usb_suspends;
    uint32_t feedback_packets, feedback_16_16; // wire Q16.16 frames / OUT packet (HS: 0.5 ms)
    uint32_t control_requests, control_stalls;
    uint32_t capture_fill, playback_fill, capture_queued_frames;
    uint32_t capture_min_fill, capture_max_fill, playback_min_fill, playback_max_fill;
    uint32_t capture_max_gap_uframes, playback_max_gap_uframes;
    bool mounted, high_speed, capture_active, playback_active;
} p4_uac2_stats_t;

// Singleton, task-context API. Initialize once; starts USB asynchronously.
// Exactly ONE producer calls capture_write and ONE consumer calls playback_read.
// Interleaved signed PCM24, left aligned in int32_t (low 8 bits zero).
// Streaming calls: no blocking, allocation or caller callbacks. Counts are FRAMES.
// Initialization creates the USB task/platform resources in task context.
esp_err_t p4_uac2_init(void);
// The SPI master puts requested_rate in its TX header; Seed confirms its actual
// rate in the RX header. Call source_rate before capture_write for each valid
// block. new_session is the peer's explicit restart/format epoch indication.
uint32_t p4_uac2_requested_rate(void);
void p4_uac2_source_rate(uint32_t actual_rate, bool new_session);
// Device-side prefill budget, not a Windows/ASIO buffer size. 1, 2 or 4 ms;
// at least two 32-frame SPI blocks. Change only with both host streams closed.
esp_err_t p4_uac2_set_buffer_ms(uint32_t milliseconds);
// Call at the Seed clock rate, including while capture is closed (clock tracking).
// Returns frames accepted; partial/full rejection is possible. Closed input is discarded.
size_t p4_uac2_capture_write(const int32_t *pcm, size_t frames);
// Returns real frames copied. Always zero-fills the remainder. Dynamic prefill.
// Call at the Seed consumption rate. No sample duplication, deletion or resampling.
size_t p4_uac2_playback_read(int32_t *pcm, size_t frames);
bool p4_uac2_capture_active(void);
bool p4_uac2_playback_active(void);
// Counters are uint32_t modulo 2^32. Snapshot fields may differ by one transaction.
void p4_uac2_get_stats(p4_uac2_stats_t *out);

#ifdef __cplusplus
}
#endif
