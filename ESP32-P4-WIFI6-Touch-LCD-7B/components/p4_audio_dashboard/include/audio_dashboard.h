#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AUDIO_DASHBOARD_TRANSPORT_BALANCED = 0,
    AUDIO_DASHBOARD_TRANSPORT_LOW_LATENCY = 1,
    AUDIO_DASHBOARD_TRANSPORT_LOCAL = 2,
};

typedef struct {
    /* The one confirmed physical Seed3 clock shown on every screen. */
    uint32_t sample_rate;
    bool sample_rate_valid;
    uint32_t buffer_ms;
    uint32_t spi_errors;
    uint32_t crc_errors;
    bool usb_connected;
    bool usb_mounted;
    bool usb_suspended;
    bool capture_active;
    bool playback_active;
    bool pc_controls_rate;
    bool pedalboard_enabled;
    bool pedalboard_forced;
    bool pedalboard_syncing;
    bool auto_switch_views;
    uint8_t transport_profile;
    uint8_t transport_mode;
    bool rate_syncing;
    bool rate_conflict;
    uint8_t capture_channel_mask;
    uint8_t playback_channel_mask;
} audio_dashboard_status_t;

typedef struct {
    bool set_sample_rate;
    uint32_t sample_rate;
    bool set_pedalboard_enabled;
    bool pedalboard_enabled;
    bool set_channel_masks;
    uint8_t capture_channel_mask;
    uint8_t playback_channel_mask;
    bool set_auto_switch_views;
    bool auto_switch_views;
    bool set_transport_profile;
    uint8_t transport_profile;
} audio_dashboard_command_t;

/* Starts the Waveshare 1024x600 MIPI-DSI display and the low-priority LVGL
 * presentation task. Audio/USB/SPI work never runs in that task. */
esp_err_t audio_dashboard_init(void);

/* Constant-time, lock-free publication from the SPI task. Values are absolute
 * PCM24-left-aligned Q31 peaks for IN1, IN2, OUT1 and OUT2. */
void audio_dashboard_submit_peaks(uint32_t input_left,
                                  uint32_t input_right,
                                  uint32_t output_left,
                                  uint32_t output_right);

/* True only while the live monitor page is visible. The SPI task uses this
 * hint to skip peak calculations when another window is open. */
bool audio_dashboard_needs_live_audio(void);

void audio_dashboard_set_seed_cpu(uint8_t percent);
void audio_dashboard_publish_status(const audio_dashboard_status_t *status);

/* Collapses repeated UI actions to their newest value. The application control
 * task is the sole consumer and validates ownership before applying them. */
bool audio_dashboard_take_command(audio_dashboard_command_t *command);

#ifdef __cplusplus
}
#endif
