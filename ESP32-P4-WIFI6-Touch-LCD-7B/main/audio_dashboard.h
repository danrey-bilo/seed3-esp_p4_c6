#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t capture_sample_rate;
    uint32_t playback_sample_rate;
    uint32_t buffer_ms;
    uint64_t spi_errors;
    uint64_t crc_errors;
    bool usb_mounted;
    bool capture_active;
    bool playback_active;
} audio_dashboard_status_t;

/* Starts the Waveshare 1024x600 MIPI-DSI display and the low-priority LVGL
 * presentation task. Audio/USB/SPI work never runs in that task. */
esp_err_t audio_dashboard_init(void);

/* Constant-time, lock-free publication from the SPI task. Values are absolute
 * PCM24-left-aligned Q31 peaks for IN1, IN2, OUT1 and OUT2. */
void audio_dashboard_submit_peaks(uint32_t input_left,
                                  uint32_t input_right,
                                  uint32_t output_left,
                                  uint32_t output_right);

void audio_dashboard_set_seed_cpu(uint8_t percent);
void audio_dashboard_publish_status(const audio_dashboard_status_t *status);

#ifdef __cplusplus
}
#endif
