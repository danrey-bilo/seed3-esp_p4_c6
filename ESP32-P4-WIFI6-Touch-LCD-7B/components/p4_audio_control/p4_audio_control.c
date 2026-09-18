#include "p4_audio_control.h"

#include <inttypes.h>
#include <string.h>

#include "audio_dashboard.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "p4_uac2_stream.h"
#include "spi_audio_protocol.h"

enum {
    CONTROL_PERIOD_MS = 50,
    USB_DISCONNECT_GRACE_MS = 750,
};

static const char *TAG = "p4_audio_control";
static uint32_t s_local_sample_rate = SPI_AUDIO_SAMPLE_RATE;
static uint32_t s_pedalboard_requested = 1U;
static uint32_t s_pedalboard_confirmed = 1U;
static uint32_t s_windows_rate_owner;
static uint32_t s_capture_channel_mask = SPI_AUDIO_CONTROL_CHANNEL_MASK;
static uint32_t s_playback_channel_mask = SPI_AUDIO_CONTROL_CHANNEL_MASK;
static uint32_t s_spi_errors;
static uint32_t s_crc_errors;
static nvs_handle_t s_settings_handle;
static bool s_settings_ready;
static bool s_prepared;
static TaskHandle_t s_control_task;

static uint32_t local_sample_rate(void)
{
    return __atomic_load_n(&s_local_sample_rate, __ATOMIC_ACQUIRE);
}

static void save_local_rate(void)
{
    if (!s_settings_ready) return;
    esp_err_t result = nvs_set_u32(
        s_settings_handle, "local_rate", local_sample_rate());
    if (result == ESP_OK) result = nvs_commit(s_settings_handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "cannot persist local rate: %s", esp_err_to_name(result));
    }
}

static void save_channel_masks(void)
{
    if (!s_settings_ready) return;
    esp_err_t result = nvs_set_u8(
        s_settings_handle, "capture_mask",
        (uint8_t)__atomic_load_n(&s_capture_channel_mask, __ATOMIC_ACQUIRE));
    if (result == ESP_OK) {
        result = nvs_set_u8(
            s_settings_handle, "playback_mask",
            (uint8_t)__atomic_load_n(&s_playback_channel_mask,
                                     __ATOMIC_ACQUIRE));
    }
    if (result == ESP_OK) result = nvs_commit(s_settings_handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "cannot persist channel masks: %s",
                 esp_err_to_name(result));
    }
}

static void publish_dashboard_state(const p4_uac2_stats_t *usb)
{
    const uint32_t requested_rate = p4_uac2_requested_rate();
    const uint32_t confirmed_rate = usb->source_sample_rate;
    const uint32_t pedalboard_requested = __atomic_load_n(
        &s_pedalboard_requested, __ATOMIC_ACQUIRE);
    const uint32_t pedalboard_confirmed = __atomic_load_n(
        &s_pedalboard_confirmed, __ATOMIC_ACQUIRE);
    const bool windows_owner = __atomic_load_n(
        &s_windows_rate_owner, __ATOMIC_ACQUIRE) != 0U;

    const audio_dashboard_status_t dashboard = {
        /* Never publish an endpoint preference as the device clock. Seed3's
         * acknowledgement is the single source used by every screen. */
        .sample_rate = confirmed_rate,
        .sample_rate_valid = confirmed_rate != 0U,
        .buffer_ms = usb->buffer_ms,
        .spi_errors = __atomic_load_n(&s_spi_errors, __ATOMIC_ACQUIRE),
        .crc_errors = __atomic_load_n(&s_crc_errors, __ATOMIC_ACQUIRE),
        .usb_connected = usb->host_alive,
        .usb_mounted = usb->host_alive && usb->mounted,
        .usb_suspended = usb->host_alive && usb->suspended,
        .capture_active = usb->capture_active,
        .playback_active = usb->playback_active,
        .pc_controls_rate = windows_owner,
        .pedalboard_enabled = pedalboard_requested != 0U,
        .pedalboard_forced = !windows_owner,
        .pedalboard_syncing = pedalboard_requested != pedalboard_confirmed,
        .rate_syncing = confirmed_rate != requested_rate
                        || usb->sample_rate != requested_rate,
        .rate_conflict = usb->rate_conflict,
        .capture_channel_mask = (uint8_t)__atomic_load_n(
            &s_capture_channel_mask, __ATOMIC_ACQUIRE),
        .playback_channel_mask = (uint8_t)__atomic_load_n(
            &s_playback_channel_mask, __ATOMIC_ACQUIRE),
    };
    audio_dashboard_publish_status(&dashboard);
}

static void control_task(void *argument)
{
    (void)argument;
    TickType_t disconnect_started = 0;
    bool disconnect_pending = false;

    while (true) {
        p4_uac2_stats_t usb;
        p4_uac2_get_stats(&usb);
        const bool host_detected = usb.host_alive;
        bool windows_owner = __atomic_load_n(
            &s_windows_rate_owner, __ATOMIC_ACQUIRE) != 0U;

        if (host_detected) {
            disconnect_pending = false;
            if (!windows_owner) {
                windows_owner = true;
                __atomic_store_n(&s_windows_rate_owner, 1U,
                                 __ATOMIC_RELEASE);
                /* Each real PC session starts processed. The user may select
                 * bypass after Windows has taken clock ownership. */
                __atomic_store_n(&s_pedalboard_requested, 1U,
                                 __ATOMIC_RELEASE);
                ESP_LOGI(TAG, "owner WINDOWS; initial rate=%" PRIu32,
                         p4_uac2_requested_rate());
            }
        } else if (windows_owner) {
            const TickType_t now = xTaskGetTickCount();
            if (!disconnect_pending) {
                disconnect_pending = true;
                disconnect_started = now;
            } else if (now - disconnect_started
                       >= pdMS_TO_TICKS(USB_DISCONNECT_GRACE_MS)) {
                windows_owner = false;
                disconnect_pending = false;
                __atomic_store_n(&s_windows_rate_owner, 0U,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&s_pedalboard_requested, 1U,
                                 __ATOMIC_RELEASE);
                ESP_LOGI(TAG, "owner LOCAL; preferred rate=%" PRIu32,
                         local_sample_rate());
            }
        }

        audio_dashboard_command_t command = {0};
        if (audio_dashboard_take_command(&command)) {
            if (command.set_pedalboard_enabled) {
                __atomic_store_n(
                    &s_pedalboard_requested,
                    windows_owner && !command.pedalboard_enabled ? 0U : 1U,
                    __ATOMIC_RELEASE);
            }
            if (command.set_channel_masks) {
                (void)p4_audio_control_set_channel_masks(
                    command.capture_channel_mask,
                    command.playback_channel_mask);
            }
            if (command.set_sample_rate) {
                if (!windows_owner
                    && spi_audio_rate_is_supported(command.sample_rate)) {
                    const esp_err_t result = p4_uac2_set_local_rate(
                        command.sample_rate);
                    if (result == ESP_OK
                        && local_sample_rate() != command.sample_rate) {
                        __atomic_store_n(&s_local_sample_rate,
                                         command.sample_rate,
                                         __ATOMIC_RELEASE);
                        save_local_rate();
                    } else if (result != ESP_OK) {
                        ESP_LOGW(TAG, "local rate rejected: %s",
                                 esp_err_to_name(result));
                    }
                } else if (windows_owner) {
                    ESP_LOGW(TAG, "local rate ignored while Windows owns USB");
                }
            }
        }

        if (!windows_owner) {
            /* Retry harmlessly until TinyUSB has fully released a disconnected
             * bus and both streaming alternates are zero. */
            (void)p4_uac2_set_local_rate(local_sample_rate());
            __atomic_store_n(&s_pedalboard_requested, 1U, __ATOMIC_RELEASE);
        }

        p4_uac2_get_stats(&usb);
        publish_dashboard_state(&usb);
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

esp_err_t p4_audio_control_prepare(void)
{
    if (s_prepared) return ESP_ERR_INVALID_STATE;

    esp_err_t result = nvs_flash_init();
    if (result == ESP_OK) {
        result = nvs_open("audio_mode", NVS_READWRITE, &s_settings_handle);
    }
    if (result == ESP_OK) {
        s_settings_ready = true;
        uint32_t stored_rate = 0U;
        result = nvs_get_u32(s_settings_handle, "local_rate", &stored_rate);
        if (result == ESP_OK && spi_audio_rate_is_supported(stored_rate)) {
            s_local_sample_rate = stored_rate;
        } else if (result != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "local rate preference ignored: %s",
                     esp_err_to_name(result));
        }
        uint8_t stored_capture_mask = SPI_AUDIO_CONTROL_CHANNEL_MASK;
        uint8_t stored_playback_mask = SPI_AUDIO_CONTROL_CHANNEL_MASK;
        if (nvs_get_u8(s_settings_handle, "capture_mask",
                       &stored_capture_mask) == ESP_OK) {
            s_capture_channel_mask = stored_capture_mask &
                                     SPI_AUDIO_CONTROL_CHANNEL_MASK;
        }
        if (nvs_get_u8(s_settings_handle, "playback_mask",
                       &stored_playback_mask) == ESP_OK) {
            s_playback_channel_mask = stored_playback_mask &
                                      SPI_AUDIO_CONTROL_CHANNEL_MASK;
        }
    } else {
        ESP_LOGW(TAG, "NVS unavailable; local rate will not persist: %s",
                 esp_err_to_name(result));
    }

    result = p4_uac2_set_local_rate(s_local_sample_rate);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "initial rate %" PRIu32 " rejected: %s",
                 s_local_sample_rate, esp_err_to_name(result));
        s_local_sample_rate = SPI_AUDIO_SAMPLE_RATE;
        ESP_RETURN_ON_ERROR(p4_uac2_set_local_rate(s_local_sample_rate), TAG,
                            "cannot apply fallback sample rate");
    }
    s_prepared = true;
    return ESP_OK;
}

esp_err_t p4_audio_control_start(void)
{
    if (!s_prepared || s_control_task != NULL) return ESP_ERR_INVALID_STATE;
    return xTaskCreatePinnedToCore(control_task, "audio_control", 4096, NULL,
                                   4, &s_control_task, 0) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

bool p4_audio_control_pedalboard_requested(void)
{
    return __atomic_load_n(&s_pedalboard_requested, __ATOMIC_ACQUIRE) != 0U;
}

uint8_t p4_audio_control_capture_channel_mask(void)
{
    return (uint8_t)__atomic_load_n(&s_capture_channel_mask,
                                    __ATOMIC_ACQUIRE);
}

uint8_t p4_audio_control_playback_channel_mask(void)
{
    return (uint8_t)__atomic_load_n(&s_playback_channel_mask,
                                    __ATOMIC_ACQUIRE);
}

esp_err_t p4_audio_control_set_channel_masks(uint8_t capture_mask,
                                             uint8_t playback_mask)
{
    if ((capture_mask & ~SPI_AUDIO_CONTROL_CHANNEL_MASK) != 0U ||
        (playback_mask & ~SPI_AUDIO_CONTROL_CHANNEL_MASK) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    __atomic_store_n(&s_capture_channel_mask, capture_mask,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_playback_channel_mask, playback_mask,
                     __ATOMIC_RELEASE);
    save_channel_masks();
    ESP_LOGI(TAG, "channel masks capture=0x%x playback=0x%x",
             capture_mask, playback_mask);
    return ESP_OK;
}

void p4_audio_control_confirm_pedalboard(bool enabled)
{
    __atomic_store_n(&s_pedalboard_confirmed, enabled ? 1U : 0U,
                     __ATOMIC_RELEASE);
}

void p4_audio_control_set_transport_errors(uint32_t spi_errors,
                                           uint32_t crc_errors)
{
    __atomic_store_n(&s_spi_errors, spi_errors, __ATOMIC_RELEASE);
    __atomic_store_n(&s_crc_errors, crc_errors, __ATOMIC_RELEASE);
}

void p4_audio_control_get_snapshot(p4_audio_control_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    snapshot->local_sample_rate = local_sample_rate();
    snapshot->windows_rate_owner = __atomic_load_n(
        &s_windows_rate_owner, __ATOMIC_ACQUIRE) != 0U;
    snapshot->pedalboard_requested = __atomic_load_n(
        &s_pedalboard_requested, __ATOMIC_ACQUIRE) != 0U;
    snapshot->pedalboard_confirmed = __atomic_load_n(
        &s_pedalboard_confirmed, __ATOMIC_ACQUIRE) != 0U;
    snapshot->capture_channel_mask =
        p4_audio_control_capture_channel_mask();
    snapshot->playback_channel_mask =
        p4_audio_control_playback_channel_mask();
}
