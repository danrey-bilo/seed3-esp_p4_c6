#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P4_AUDIO_TRANSPORT_BALANCED = 0,
    P4_AUDIO_TRANSPORT_LOW_LATENCY = 1,
    P4_AUDIO_TRANSPORT_LOCAL = 2,
} p4_audio_transport_mode_t;

typedef struct {
    uint32_t local_sample_rate;
    bool windows_rate_owner;
    bool pedalboard_requested;
    bool pedalboard_confirmed;
    bool auto_switch_views;
    p4_audio_transport_mode_t transport_profile;
    p4_audio_transport_mode_t transport_mode;
    uint8_t capture_channel_mask;
    uint8_t playback_channel_mask;
} p4_audio_control_snapshot_t;

/* Load the standalone rate from NVS and make it the UAC2 clock value before
 * USB enumeration starts. Call once before p4_uac2_init(). */
esp_err_t p4_audio_control_prepare(void);

/* Start the owner/pedalboard state machine after UAC2 and the dashboard. */
esp_err_t p4_audio_control_start(void);

/* The SPI transport repeats this desired state in every outgoing frame. */
bool p4_audio_control_pedalboard_requested(void);
p4_audio_transport_mode_t p4_audio_control_transport_profile(void);
/* LOCAL is selected automatically while both USB streaming alternates are
 * inactive. Otherwise this returns the persisted BALANCED/LOW LATENCY profile. */
p4_audio_transport_mode_t p4_audio_control_transport_mode(void);
esp_err_t p4_audio_control_set_transport_profile(
    p4_audio_transport_mode_t profile);
uint8_t p4_audio_control_capture_channel_mask(void);
uint8_t p4_audio_control_playback_channel_mask(void);
/* Apply 2-bit channel masks and persist them in NVS. Used by the UI and the
 * diagnostic console; the SPI transport publishes them on its next frame. */
esp_err_t p4_audio_control_set_channel_masks(uint8_t capture_mask,
                                             uint8_t playback_mask);

/* Feed back the Seed3 acknowledgement from every valid incoming SPI frame. */
void p4_audio_control_confirm_pedalboard(bool enabled);

/* Low-rate diagnostic counters shown by the dashboard. */
void p4_audio_control_set_transport_errors(uint32_t spi_errors,
                                           uint32_t crc_errors);

void p4_audio_control_get_snapshot(p4_audio_control_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
