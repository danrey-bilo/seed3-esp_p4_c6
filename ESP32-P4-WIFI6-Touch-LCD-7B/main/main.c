#include "audio_dashboard.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p4_audio_control.h"
#include "p4_uac2_stream.h"
#include "seed3_spi_transport.h"

void app_main(void)
{
    /* The local clock must be loaded before TinyUSB begins enumeration. */
    ESP_ERROR_CHECK(p4_audio_control_prepare());
    ESP_ERROR_CHECK(seed3_spi_transport_start());
    ESP_ERROR_CHECK(p4_uac2_init());
    ESP_ERROR_CHECK(audio_dashboard_init());
    ESP_ERROR_CHECK(p4_audio_control_start());
    ESP_ERROR_CHECK(seed3_spi_transport_start_console());
    seed3_spi_transport_log_ready();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        seed3_spi_transport_log();
    }
}
