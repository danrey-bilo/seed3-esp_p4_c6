#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spi_audio_protocol.h"
#include "p4_uac2_stream.h"

enum {
    PIN_SPI_SCLK = 2,
    PIN_SPI_MOSI = 3,
    PIN_SPI_MISO = 4,
    PIN_SPI_CS = 5,
    PIN_SEED_READY = 28,
    PIN_SEED_UART_RX = 30,
    PIN_SEED_UART_TX = 31,
    SPI_CLOCK_HZ = 20000000,
};

typedef struct {
    uint64_t spi_transactions;
    uint64_t spi_errors;
    uint64_t ready_interrupts;
    uint64_t header_errors;
    uint64_t crc_errors;
    uint64_t sequence_errors;
    uint64_t format_errors;
    uint16_t seed_status;
    uint32_t seed_sessions;
    uint32_t echoed_frames;
    SpiAudioHeader last_bad_header;
} audio_stats_t;

typedef enum {
    PLAYBACK_IDLE_SILENCE,
    PLAYBACK_AUDIO,
    PLAYBACK_XRUN_SILENCE,
} playback_result_t;

static const char *TAG = "seed3_p4_spi";

static SpiAudioFrame s_spi_tx DMA_ATTR;
static SpiAudioFrame s_spi_rx DMA_ATTR;
static spi_device_handle_t s_spi_device;
static TaskHandle_t s_spi_task_handle;
static volatile bool s_spi_init_done;
static volatile esp_err_t s_spi_init_result = ESP_FAIL;

static audio_stats_t s_stats;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_tx_sequence;
static uint32_t s_tx_sample_counter;
static uint32_t s_expected_rx_sequence;
static bool s_have_rx_sequence;
static uint32_t s_spi_pause_until;
static uint32_t s_ready_generation;

static playback_result_t playback_ring_read_block(int32_t *destination)
{
    const size_t frames = p4_uac2_playback_read(destination, SPI_AUDIO_FRAMES);
    if (frames == SPI_AUDIO_FRAMES) return PLAYBACK_AUDIO;
    return p4_uac2_playback_active() ? PLAYBACK_XRUN_SILENCE : PLAYBACK_IDLE_SILENCE;
}

static int32_t decode_pcm24_left_aligned(const uint8_t sample[4])
{
    uint32_t value;
    memcpy(&value, sample, sizeof(value));
    return (int32_t)(value & 0xffffff00U);
}

static void fill_tx_header(uint8_t flags, uint16_t status)
{
    s_spi_tx.header.magic = SPI_AUDIO_MAGIC;
    s_spi_tx.header.version = SPI_AUDIO_VERSION;
    s_spi_tx.header.flags = flags | (s_tx_sequence == 0 ? SPI_AUDIO_FLAG_SESSION_START : 0);
    s_spi_tx.header.header_bytes = SPI_AUDIO_HEADER_BYTES;
    s_spi_tx.header.sequence = s_tx_sequence;
    s_spi_tx.header.sample_rate = p4_uac2_requested_rate();
    s_spi_tx.header.frames = SPI_AUDIO_FRAMES;
    s_spi_tx.header.channels = SPI_AUDIO_CHANNELS;
    s_spi_tx.header.valid_bits = SPI_AUDIO_VALID_BITS;
    s_spi_tx.header.sample_counter = s_tx_sample_counter;
    s_spi_tx.header.payload_bytes = SPI_AUDIO_PAYLOAD_BYTES;
    s_spi_tx.header.status = status;
    s_spi_tx.header.crc32 = 0U;
    s_spi_tx.header.crc32 = spi_audio_frame_crc32(&s_spi_tx);
}

static void validate_rx_frame(void)
{
    static bool logged_first_valid_frame;

    if (!spi_audio_header_is_valid(&s_spi_rx.header)) {
        portENTER_CRITICAL(&s_stats_mux);
        ++s_stats.header_errors;
        s_stats.last_bad_header = s_spi_rx.header;
        portEXIT_CRITICAL(&s_stats_mux);
        return;
    }
    if (spi_audio_frame_crc32(&s_spi_rx) != s_spi_rx.header.crc32) {
        portENTER_CRITICAL(&s_stats_mux);
        ++s_stats.crc_errors;
        portEXIT_CRITICAL(&s_stats_mux);
        return;
    }

    bool format_error = false;
    for (size_t sample = 0;
         sample < SPI_AUDIO_FRAMES * SPI_AUDIO_CHANNELS; ++sample) {
        if (((uint32_t)s_spi_rx.samples[sample] & 0xFFU) != 0U) {
            format_error = true;
            break;
        }
    }

    portENTER_CRITICAL(&s_stats_mux);
    if (s_spi_rx.header.flags & SPI_AUDIO_FLAG_SESSION_START) {
        s_have_rx_sequence = false;
        ++s_stats.seed_sessions;
    }
    if (s_have_rx_sequence &&
        s_spi_rx.header.sequence != s_expected_rx_sequence) {
        ++s_stats.sequence_errors;
    }
    if (format_error) {
        ++s_stats.format_errors;
    }
    s_stats.seed_status = s_spi_rx.header.status;
    if (!memcmp(&s_spi_rx, &s_spi_tx, sizeof(s_spi_rx))) ++s_stats.echoed_frames;
    portEXIT_CRITICAL(&s_stats_mux);

    s_expected_rx_sequence = s_spi_rx.header.sequence + 1U;
    s_have_rx_sequence = true;

    if (!logged_first_valid_frame) {
        int32_t peak[SPI_AUDIO_CHANNELS] = {0};
        for (size_t frame = 0; frame < SPI_AUDIO_FRAMES; ++frame) {
            for (size_t channel = 0; channel < SPI_AUDIO_CHANNELS;
                 ++channel) {
                const int32_t value =
                    s_spi_rx.samples[frame * SPI_AUDIO_CHANNELS + channel];
                const int32_t magnitude =
                    value < 0 ? (int32_t)(-(int64_t)value) : value;
                if (magnitude > peak[channel]) {
                    peak[channel] = magnitude;
                }
            }
        }
        ESP_LOGI(TAG,
                 "first valid Seed frame: seq=%" PRIu32 " flags=%u tx_seq=%" PRIu32
                 " sample0=%" PRId32 "/%" PRId32
                 " peak=%" PRId32 "/%" PRId32,
                 s_spi_rx.header.sequence, s_spi_rx.header.flags, s_spi_tx.header.sequence, s_spi_rx.samples[0],
                 s_spi_rx.samples[1], peak[0], peak[1]);
        logged_first_valid_frame = true;
    }
    p4_uac2_source_rate(s_spi_rx.header.sample_rate,
        (s_spi_rx.header.flags & SPI_AUDIO_FLAG_SESSION_START) != 0);
    p4_uac2_capture_write(s_spi_rx.samples, SPI_AUDIO_FRAMES);
}

static void IRAM_ATTR seed_ready_isr(void *argument)
{
    (void)argument;
    __atomic_fetch_add(&s_ready_generation, 1U, __ATOMIC_RELEASE);
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (s_spi_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_spi_task_handle, &higher_priority_task_woken);
    }
    portENTER_CRITICAL_ISR(&s_stats_mux);
    ++s_stats.ready_interrupts;
    portEXIT_CRITICAL_ISR(&s_stats_mux);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t init_spi_master(void)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = sizeof(SpiAudioFrame),
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_SCLK |
                 SPICOMMON_BUSFLAG_MOSI | SPICOMMON_BUSFLAG_MISO,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG,
        "cannot initialize SPI2 bus");

    const spi_device_interface_config_t device_config = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0,
        .clock_source = SPI_CLK_SRC_DEFAULT,
        .duty_cycle_pos = 128,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz = SPI_CLOCK_HZ,
        .input_delay_ns = 0,
        .sample_point = SPI_SAMPLING_POINT_PHASE_0,
        .spics_io_num = PIN_SPI_CS,
        .flags = 0,
        .queue_size = 1,
        .pre_cb = NULL,
        .post_cb = NULL,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_add_device(SPI2_HOST, &device_config, &s_spi_device), TAG,
        "cannot add Seed3 SPI device");

    const gpio_config_t ready_config = {
        .pin_bit_mask = 1ULL << PIN_SEED_READY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ready_config), TAG,
                        "cannot configure READY input");
    esp_err_t result = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    return gpio_isr_handler_add(PIN_SEED_READY, seed_ready_isr, NULL);
}

static void spi_transport_task(void *argument)
{
    (void)argument;
    s_spi_task_handle = xTaskGetCurrentTaskHandle();
    const esp_err_t init_result = init_spi_master();
    __atomic_store_n(&s_spi_init_result, init_result, __ATOMIC_RELAXED);
    __atomic_store_n(&s_spi_init_done, true, __ATOMIC_RELEASE);
    if (init_result != ESP_OK) {
        ESP_LOGE(TAG, "SPI initialization failed: %s",
                 esp_err_to_name(init_result));
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG,
             "SPI2 master ready: mode 0, %d Hz, SCLK=%d MOSI=%d MISO=%d "
             "CS=%d READY=%d, frame=%u bytes",
             SPI_CLOCK_HZ, PIN_SPI_SCLK, PIN_SPI_MOSI, PIN_SPI_MISO,
             PIN_SPI_CS, PIN_SEED_READY, (unsigned)sizeof(SpiAudioFrame));

    uint32_t consumed_ready = UINT32_MAX; // accept an already-high READY at startup
    while (true) {
        uint32_t pause_until = __atomic_load_n(&s_spi_pause_until, __ATOMIC_ACQUIRE);
        if (pause_until && (int32_t)(pause_until - xTaskGetTickCount()) > 0) {
            vTaskDelay(1);
            continue;
        }
        uint32_t generation = __atomic_load_n(&s_ready_generation, __ATOMIC_ACQUIRE);
        if (gpio_get_level(PIN_SEED_READY) == 0 || generation == consumed_ready) {
            // A level alone is insufficient: at 96k the slave may still be
            // clearing the previous completion. Clock exactly once per READY
            // assertion, including when low->high happens before we get here.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }
        consumed_ready = generation;

        const playback_result_t playback =
            playback_ring_read_block(s_spi_tx.samples);
        uint8_t flags = SPI_AUDIO_FLAG_VALID;
        uint16_t status = 0U;
        if (playback != PLAYBACK_AUDIO) {
            flags |= SPI_AUDIO_FLAG_SILENCE;
        }
        if (playback == PLAYBACK_XRUN_SILENCE) {
            flags |= SPI_AUDIO_FLAG_XRUN;
            status |= SPI_AUDIO_STATUS_PLAYBACK_UNDERRUN;
        }
        fill_tx_header(flags, status);
        memset(&s_spi_rx, 0, sizeof(s_spi_rx));

        spi_transaction_t transaction = {
            // 288-byte protocol frames are not a multiple of the P4's
            // 64-byte cache line. Let ESP-IDF use its aligned DMA bounce
            // buffer; declaring manual alignment here is invalid on rev 1.x.
            .flags = 0,
            .cmd = 0,
            .addr = 0,
            .length = sizeof(SpiAudioFrame) * 8U,
            .rxlength = sizeof(SpiAudioFrame) * 8U,
            .override_freq_hz = 0,
            .user = NULL,
            .tx_buffer = &s_spi_tx,
            .rx_buffer = &s_spi_rx,
        };
        esp_err_t result = spi_device_queue_trans(
            s_spi_device, &transaction, pdMS_TO_TICKS(2));
        spi_transaction_t *completed = NULL;
        if (result == ESP_OK) {
            // Never reuse a transaction or its DMA buffers before completion.
            // This wait is the existing hardware SPI transfer, not a USB wait.
            do {
                result = spi_device_get_trans_result(
                    s_spi_device, &completed, pdMS_TO_TICKS(20));
            } while (result == ESP_ERR_TIMEOUT);
        }
        if (result != ESP_OK || completed != &transaction) {
            portENTER_CRITICAL(&s_stats_mux);
            ++s_stats.spi_errors;
            portEXIT_CRITICAL(&s_stats_mux);
            ESP_LOGE(TAG, "SPI transaction failed: %s",
                     esp_err_to_name(result));
            continue;
        }

        ++s_tx_sequence;
        s_tx_sample_counter += SPI_AUDIO_FRAMES;
        portENTER_CRITICAL(&s_stats_mux);
        ++s_stats.spi_transactions;
        portEXIT_CRITICAL(&s_stats_mux);
        validate_rx_frame();
    }
}

static void run_format_self_test(void)
{
    static const uint8_t vectors[][4] = {
        {0x00, 0x00, 0x00, 0x00},
        {0x00, 0xFF, 0xFF, 0x7F},
        {0x00, 0x00, 0x00, 0x80},
        {0x00, 0x55, 0x55, 0x55},
        {0x00, 0xAA, 0xAA, 0xAA},
    };
    static const int32_t expected[] = {
        0, 2147483392, INT32_MIN, 1431655680, -1431655936,
    };
    for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]);
         ++index) {
        if (decode_pcm24_left_aligned(vectors[index]) != expected[index]) {
            ESP_LOGE(TAG, "PCM24 self-test failed at vector %u",
                     (unsigned)index);
            abort();
        }
    }
    ESP_LOGI(TAG, "PCM24-in-32 and transport layout self-test: PASS");
}

static void seed_uart_task(void *argument)
{
    (void)argument;
    uint8_t input[64];
    char line[160];
    size_t used = 0U;
    while (true) {
        const int received = uart_read_bytes(
            UART_NUM_1, input, sizeof(input), pdMS_TO_TICKS(100));
        for (int index = 0; index < received; ++index) {
            const uint8_t byte = input[index];
            if (byte == '\r') {
                continue;
            }
            if (byte == '\n') {
                line[used] = '\0';
                if (used != 0U) {
                    ESP_LOGI(TAG, "UART %s", line);
                }
                used = 0U;
            } else if (used + 1U < sizeof(line)) {
                line[used++] = (char)byte;
            } else {
                used = 0U;
            }
        }
    }
}

static esp_err_t init_seed_uart(void)
{
    const uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {
            .backup_before_sleep = 0,
        },
    };
    ESP_RETURN_ON_ERROR(uart_param_config(UART_NUM_1, &config), TAG,
                        "cannot configure Seed diagnostic UART");
    ESP_RETURN_ON_ERROR(
        uart_set_pin(UART_NUM_1, PIN_SEED_UART_TX, PIN_SEED_UART_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
        TAG, "cannot assign Seed diagnostic UART pins");
    ESP_RETURN_ON_ERROR(
        uart_driver_install(UART_NUM_1, 512, 0, 0, NULL, 0), TAG,
        "cannot install Seed diagnostic UART");
    const BaseType_t result = xTaskCreatePinnedToCore(
        seed_uart_task, "seed_uart", 3072, NULL, 8, NULL, 0);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void log_stats(void)
{
    audio_stats_t snapshot;
    portENTER_CRITICAL(&s_stats_mux);
    snapshot = s_stats;
    portEXIT_CRITICAL(&s_stats_mux);
    p4_uac2_stats_t u;
    p4_uac2_get_stats(&u);
    ESP_LOGI(TAG, "spi=%" PRIu64 " err=%" PRIu64
             " frame_err[h/c/s/f]=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
             " seed_status=0x%04x sessions=%" PRIu32 " echoed=%" PRIu32,
             snapshot.spi_transactions, snapshot.spi_errors, snapshot.header_errors,
             snapshot.crc_errors, snapshot.sequence_errors, snapshot.format_errors, snapshot.seed_status, snapshot.seed_sessions, snapshot.echoed_frames);
    ESP_LOGI(TAG, "format rate=%" PRIu32 " seed=%" PRIu32 " bits=%" PRIu32 "/%" PRIu32
             " prefill=%" PRIu32 " budget=%" PRIu32 "ms changes=%" PRIu32 " epochs=%" PRIu32,
             u.sample_rate, u.source_sample_rate, u.capture_bits, u.playback_bits,
             u.prefill_frames, u.buffer_ms, u.rate_changes, u.source_restarts);
    if (snapshot.header_errors) {
        const SpiAudioHeader *h = &snapshot.last_bad_header;
        ESP_LOGI(TAG, "last_bad magic=%08" PRIx32 " v=%u flags=%u hdr=%u rate=%" PRIu32
                 " frames=%u channels=%u bits=%u payload=%u seq=%" PRIu32,
                 h->magic, h->version, h->flags, h->header_bytes, h->sample_rate,
                 h->frames, h->channels, h->valid_bits, h->payload_bytes, h->sequence);
    }
    ESP_LOGI(TAG, "uac mounted=%d hs=%d active=%d/%d src=%" PRIu32
             " cap_done=%" PRIu32 " pkts=%" PRIu32 " fill=%" PRIu32 "+%" PRIu32
             " sil=%" PRIu32 " over=%" PRIu32 " inc=%" PRIu32
             " play_rx=%" PRIu32 " play_read=%" PRIu32 " fill=%" PRIu32
             " under=%" PRIu32 " over=%" PRIu32 " bad=%" PRIu32
             " recover=%" PRIu32 " reset=%" PRIu32 " gap=%" PRIu32 "/%" PRIu32
             " fb=%" PRIu32 " fb_done=%" PRIu32 " fb_inc=%" PRIu32
             " play_pkt=%" PRIu32 " play_len=%" PRIu32 "/%" PRIu32
             " ctrl=%" PRIu32 "/%" PRIu32,
             u.mounted, u.high_speed, u.capture_active, u.playback_active, u.capture_source_frames,
             u.capture_completed_frames, u.capture_packets, u.capture_fill, u.capture_queued_frames,
             u.capture_silence_frames, u.capture_overrun_frames, u.capture_incomplete,
             u.playback_received_frames, u.playback_consumed_frames, u.playback_fill,
             u.playback_underruns, u.playback_overrun_frames, u.playback_bad_packets,
             u.endpoint_recoveries, u.controller_restarts,
             u.capture_max_gap_uframes, u.playback_max_gap_uframes, u.feedback_16_16,
             u.feedback_packets, u.feedback_incomplete, u.playback_packets,
             u.playback_short_packets, u.playback_long_packets,
             u.control_requests, u.control_stalls);
}

static void log_cpu_load(void)
{
    static TaskHandle_t handles[4];
    static uint32_t previous[4];
    static int64_t previous_time;
    uint32_t current[4] = {0};
    const char *names[4] = {"IDLE0", "IDLE1", "seed3_spi", "p4_uac2"};
    // uxTaskGetSystemState also scans ALL task stacks while holding the
    // scheduler lock. That diagnostic can itself delay a 0.5ms USB packet.
    // Our four tasks live forever: cache their handles, sample counters only,
    // and explicitly skip stack scanning and task-state traversal.
    for (unsigned i = 0; i < 4; ++i) {
        if (!handles[i]) handles[i] = xTaskGetHandle(names[i]);
        if (!handles[i]) return;
        TaskStatus_t task;
        vTaskGetInfo(handles[i], &task, pdFALSE, eRunning);
        current[i] = task.ulRunTimeCounter;
    }
    const int64_t now = esp_timer_get_time();
    if (previous_time) {
        uint32_t load[4];
        for (unsigned i = 0; i < 4; ++i) {
            load[i] = (uint64_t)(current[i] - previous[i]) * 1000 / (now - previous_time);
            if (load[i] > 1000) load[i] = 1000;
        }
        ESP_LOGI(TAG, "cpu permille busy_core0=%lu busy_core1=%lu spi_task=%lu usb_task=%lu (scheduler estimate)",
                 (unsigned long)(1000 - load[0]), (unsigned long)(1000 - load[1]),
                 (unsigned long)load[2], (unsigned long)load[3]);
    }
    memcpy(previous, current, sizeof(current));
    previous_time = now;
}

static void console_task(void *argument)
{
    (void)argument;
    char line[64];
    size_t used = 0;
    if (!uart_is_driver_installed(UART_NUM_0)) {
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    }
    while (true) {
        uint8_t byte;
        if (uart_read_bytes(UART_NUM_0, &byte, 1, pdMS_TO_TICKS(20)) != 1) continue;
        if (byte == '\r') continue;
        if (byte != '\n') {
            if (used + 1 < sizeof(line)) line[used++] = (char)byte;
            else used = 0;
            continue;
        }
        line[used] = '\0';
        used = 0;
        ESP_LOGI(TAG, "console received: [%s]", line);
        // A leading newline discards an incomplete line/noise from P4 reset.
        if (!strcmp(line, "seed reset")) uart_write_bytes(UART_NUM_1, "\n@RESET\n", 8);
        else if (!strcmp(line, "seed boot")) uart_write_bytes(UART_NUM_1, "\n@BOOT\n", 7);
        else if (!strcmp(line, "seed stats")) {
            if (!p4_uac2_capture_active() && !p4_uac2_playback_active())
                uart_write_bytes(UART_NUM_1, "\n@STATS\n", 8);
            else ESP_LOGW(TAG, "Close audio streams before requesting Seed stats");
        }
        else if (!strncmp(line, "buffer ", 7)) {
            esp_err_t result = p4_uac2_set_buffer_ms((uint32_t)strtoul(line + 7, NULL, 10));
            ESP_LOGI(TAG, "buffer command: %s", esp_err_to_name(result));
        } else if (!strncmp(line, "spi pause ", 10)) {
            unsigned ms = (unsigned)strtoul(line + 10, NULL, 10);
            if (ms && ms <= 10000 && !p4_uac2_capture_active() && !p4_uac2_playback_active()) {
                __atomic_store_n(&s_spi_pause_until, xTaskGetTickCount() + pdMS_TO_TICKS(ms), __ATOMIC_RELEASE);
                ESP_LOGI(TAG, "SPI pause test: %u ms", ms);
            }
        } else if (line[0]) ESP_LOGI(TAG, "commands: buffer 1|2|4; seed reset; seed boot; seed stats; spi pause 1..10000 (idle only)");
    }
}

void app_main(void)
{
    run_format_self_test();
    ESP_ERROR_CHECK(init_seed_uart());
    const BaseType_t task_result = xTaskCreatePinnedToCore(
        spi_transport_task, "seed3_spi", 6144, NULL, 15, NULL, 1);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    while (!__atomic_load_n(&s_spi_init_done, __ATOMIC_ACQUIRE)) {
        vTaskDelay(1);
    }
    ESP_ERROR_CHECK(__atomic_load_n(&s_spi_init_result, __ATOMIC_RELAXED));

    ESP_ERROR_CHECK(p4_uac2_init());
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(console_task, "audio_console", 3072, NULL, 3, NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG,
             "ready: UAC2 2 IN + 2 OUT, 44.1/48/88.2/96 kHz, packed PCM24 only, USB HS; "
             "Seed3 SPI transport at %d MHz",
             SPI_CLOCK_HZ / 1000000);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        log_stats();
        log_cpu_load();
    }
}
