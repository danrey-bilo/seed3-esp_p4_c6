#include "daisy_seed.h"
#include "per/gpio.h"
#include "per/spi.h"
#include "per/uart.h"
#include "spi_audio_protocol.h"
#include "seedfx_graph.h"
#include "seed_level_meter.h"
#include "seed_audio_clock.h"
#include "stm32h7xx_hal.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

using daisy::AudioHandle;
using daisy::DaisySeed;
using daisy::GPIO;
using daisy::SpiHandle;
using daisy::System;
using daisy::UartHandler;

namespace
{
uint32_t g_sample_rate = SPI_AUDIO_SAMPLE_RATE;
constexpr std::size_t kBlockFrames      = SPI_AUDIO_FRAMES;
constexpr std::size_t kRingBlocks       = 8U;
constexpr std::size_t kDmaSlots         = 4U;
constexpr uint32_t    kControlPeriodMs  = 20U;
constexpr float       kLeftFrequency    = 997.0f;
constexpr float       kRightFrequency   = 1501.0f;
constexpr float       kTestAmplitude    = 0.2511886432f; // -12 dBFS peak
constexpr float       kPlaybackMixGain  = 0.5f;
constexpr float       kTwoPi            = 6.2831853071795864769f;
constexpr bool        kUseTestTones     = false; // production capture: Seed ADC L/R

struct AudioBlock
{
    uint32_t sample_counter;
    int32_t  samples[SPI_AUDIO_FRAMES * SPI_AUDIO_CHANNELS];
};

struct DmaSlot
{
    SpiAudioFrame tx;
    SpiAudioFrame rx;
};

DaisySeed g_seed;
SpiHandle g_spi;
GPIO      g_ready;
UartHandler g_uart;

AudioBlock g_capture_ring[kRingBlocks];
AudioBlock g_playback_ring[kRingBlocks];
DmaSlot DMA_BUFFER_MEM_SECTION g_dma_slots[kDmaSlots];
uint8_t DMA_BUFFER_MEM_SECTION g_uart_rx[64];
volatile uint32_t g_uart_command = 0U;
volatile bool g_uart_restart = false;
SeedFxControlPacket g_seedfx_control_packet;
volatile bool g_seedfx_control_pending = false;
IWDG_HandleTypeDef g_watchdog{};
// NOLOAD SRAM retains a tiny breadcrumb across watchdog resets (not power loss).
struct RecoveryTrace { uint32_t magic, stage, sequence, audio_frames, fault, pc, lr, address; };
volatile RecoveryTrace DMA_BUFFER_MEM_SECTION g_trace;

void Trace(uint32_t stage)
{
    g_trace.magic = 0x52554332U;
    g_trace.stage = stage;
}
uint32_t g_dma_started_ms = 0U;
bool g_session_start = true;
uint32_t g_idle_ticks, g_window_tick, g_window_ms;
volatile uint32_t g_audio_ticks;
uint32_t g_cpu_permille, g_audio_permille;

volatile uint32_t g_capture_write = 0U;
volatile uint32_t g_capture_read = 0U;
volatile uint32_t g_playback_write = 0U;
volatile uint32_t g_playback_read = 0U;
volatile uint32_t g_audio_sample_counter = 0U;
volatile uint32_t g_capture_overruns = 0U;
volatile uint32_t g_playback_underruns = 0U;
volatile uint32_t g_audio_size_errors = 0U;

volatile bool              g_dma_active = false;
volatile bool              g_dma_done = false;
volatile SpiHandle::Result g_dma_result = SpiHandle::Result::ERR;
volatile uint32_t          g_active_slot = 0U;

uint32_t g_tx_sequence = 0U;
uint32_t g_expected_rx_sequence = 0U;
bool     g_have_rx_sequence = false;
uint32_t g_good_transactions = 0U;
uint32_t g_spi_start_errors = 0U;
uint32_t g_rx_header_errors = 0U;
uint32_t g_rx_crc_errors = 0U;
uint32_t g_rx_sequence_errors = 0U;
uint16_t g_sticky_status = 0U;
bool     g_pedalboard_enabled = true;
volatile bool g_control_only = false;
bool     g_low_latency = false;
uint16_t g_spi_frames = SPI_AUDIO_FRAMES;
uint32_t g_last_control_transfer_ms = 0U;
volatile uint32_t g_capture_channel_mask = SPI_AUDIO_CONTROL_CHANNEL_MASK;
volatile uint32_t g_playback_channel_mask = SPI_AUDIO_CONTROL_CHANNEL_MASK;

float g_left_phase = 0.0f;
float g_right_phase = 0.0f;
float g_graph_input[SPI_AUDIO_CHANNELS][SPI_AUDIO_FRAMES];
float g_graph_output[SPI_AUDIO_CHANNELS][SPI_AUDIO_FRAMES];

void SendUart(const char* text)
{
    if(seed_level_meter_cancel_tx()) {
        uint8_t delimiter='\n';
        g_uart.BlockingTransmit(&delimiter,1,20U);
    }
    g_uart.BlockingTransmit(reinterpret_cast<uint8_t*>(const_cast<char*>(text)),
                            std::strlen(text),
                            20U);
}

[[noreturn]] void Fatal(const char* reason)
{
    g_ready.Write(false);
    SendUart(reason);
    g_seed.SetLed(true);
    while(true) {}
}

[[noreturn]] void Recover(const char* reason)
{
    g_ready.Write(false);
    SendUart(reason);
    // libDaisy SPI has no public abort API. A bounded, automatic MCU restart
    // also clears a partial slave DMA transaction left by a restarting P4.
    NVIC_SystemReset();
    // The independent watchdog also handles faults in a HAL/library wait.
    while(true) {}
}

void UartReceive(uint8_t* data, size_t size, void*, UartHandler::Result result)
{
    static char command[16];
    static size_t used = 0;
    static size_t packet_used = 0;
    if(result != UartHandler::Result::OK)
    {
        used = 0;
        __atomic_store_n(&g_uart_restart, true, __ATOMIC_RELEASE);
        return;
    }
    for(size_t i = 0; i < size; ++i)
    {
        const uint8_t byte = data[i];
        if(packet_used != 0U || (used == 0U && byte == 'S'))
        {
            if(__atomic_load_n(&g_seedfx_control_pending, __ATOMIC_ACQUIRE))
            {
                packet_used = 0U;
                continue;
            }
            reinterpret_cast<uint8_t*>(&g_seedfx_control_packet)[packet_used++]
                = byte;
            if(packet_used <= sizeof(uint32_t))
            {
                const uint8_t magic[] = {'S', 'F', 'X', 'C'};
                if(byte != magic[packet_used - 1U]) packet_used = 0U;
                continue;
            }
            if(packet_used == sizeof(SeedFxControlHeader))
            {
                const SeedFxControlHeader& header
                    = g_seedfx_control_packet.header;
                if(header.magic != SEEDFX_CONTROL_MAGIC
                   || header.version != SEEDFX_CONTROL_VERSION
                   || header.payload_bytes > SEEDFX_CONTROL_MAX_PAYLOAD)
                {
                    packet_used = 0U;
                    continue;
                }
            }
            if(packet_used >= sizeof(SeedFxControlHeader))
            {
                const size_t expected = sizeof(SeedFxControlHeader)
                    + g_seedfx_control_packet.header.payload_bytes;
                if(packet_used == expected)
                {
                    __atomic_store_n(&g_seedfx_control_pending, true,
                                     __ATOMIC_RELEASE);
                    packet_used = 0U;
                }
            }
            continue;
        }
        if(byte == '\r') continue;
        if(byte == '\n')
        {
            command[used] = 0;
            if(!std::strcmp(command, "@BOOT")) __atomic_store_n(&g_uart_command, 1U, __ATOMIC_RELEASE);
            if(!std::strcmp(command, "@RESET")) __atomic_store_n(&g_uart_command, 2U, __ATOMIC_RELEASE);
            if(!std::strcmp(command, "@STATS")) __atomic_store_n(&g_uart_command, 3U, __ATOMIC_RELEASE);
            used = 0;
        }
        else if(used + 1 < sizeof(command)) command[used++] = static_cast<char>(byte);
        else used = 0;
    }
}

void ProcessSeedFxControl()
{
    if(!__atomic_load_n(&g_seedfx_control_pending, __ATOMIC_ACQUIRE)) return;
    const SeedFxControlHeader header = g_seedfx_control_packet.header;
    SeedFxGraphResult result = SeedFxGraphResult::InvalidHeader;
    if(seedfx_crc32(g_seedfx_control_packet.payload, header.payload_bytes)
       != header.payload_crc32)
    {
        result = SeedFxGraphResult::InvalidCrc;
    }
    else if(header.command == SEEDFX_COMMAND_GRAPH
            && header.payload_bytes == sizeof(SeedFxGraphDefinition))
    {
        const auto* graph = reinterpret_cast<const SeedFxGraphDefinition*>(
            g_seedfx_control_packet.payload);
        result = seedfx_graph_stage(*graph);
    }
    else if(header.command == SEEDFX_COMMAND_CACHE_BEGIN
            && header.payload_bytes == sizeof(SeedFxCacheBegin))
    {
        const auto* begin = reinterpret_cast<const SeedFxCacheBegin*>(
            g_seedfx_control_packet.payload);
        result = seedfx_cache_begin(begin->package_id, begin->total_bytes,
                                    begin->content_crc32);
    }
    else if(header.command == SEEDFX_COMMAND_CACHE_CHUNK
            && header.payload_bytes >= offsetof(SeedFxCacheChunk, data))
    {
        const auto* chunk = reinterpret_cast<const SeedFxCacheChunk*>(
            g_seedfx_control_packet.payload);
        const size_t prefix = offsetof(SeedFxCacheChunk, data);
        result = chunk->data_bytes <= SEEDFX_CACHE_CHUNK_BYTES
                     && header.payload_bytes == prefix + chunk->data_bytes
            ? seedfx_cache_write(chunk->package_id, chunk->offset,
                                 chunk->data, chunk->data_bytes)
            : SeedFxGraphResult::CacheBounds;
    }
    else if(header.command == SEEDFX_COMMAND_CACHE_END
            && header.payload_bytes == sizeof(SeedFxCacheEnd))
    {
        const auto* end = reinterpret_cast<const SeedFxCacheEnd*>(
            g_seedfx_control_packet.payload);
        result = seedfx_cache_end(end->package_id);
    }
    else if(header.command == SEEDFX_COMMAND_CACHE_CLEAR
            && header.payload_bytes == 0U)
    {
        seedfx_cache_clear();
        result = SeedFxGraphResult::Ok;
    }
    __atomic_store_n(&g_seedfx_control_pending, false, __ATOMIC_RELEASE);

    /* A blocking diagnostic acknowledgement is safe in LOCAL. During live
     * USB/SPI audio P4 observes the revision through its own graph model and
     * no UART formatting is allowed to delay completion-driven SPI rearm. */
    if(__atomic_load_n(&g_control_only, __ATOMIC_ACQUIRE))
    {
        char response[72];
        std::snprintf(response, sizeof(response), "SEED:SFX seq=%lu %s\r\n",
                      (unsigned long)header.sequence,
                      seedfx_graph_result_name(result));
        SendUart(response);
    }
}

float ClampAudio(float sample)
{
    if(sample > 0.99999988f)
        return 0.99999988f;
    if(sample < -1.0f)
        return -1.0f;
    return sample;
}

int32_t FloatToLeftAlignedPcm24(float sample)
{
    const float clamped = ClampAudio(sample);
    int32_t     pcm24
        = static_cast<int32_t>(std::lrintf(clamped * 8388607.0f));
    if(pcm24 < -8388608)
        pcm24 = -8388608;
    return pcm24 * 256;
}

float LeftAlignedPcm24ToFloat(int32_t sample)
{
    return static_cast<float>(sample) * (1.0f / 2147483648.0f);
}

float NextSine(float& phase, float frequency)
{
    const float sample = std::sin(phase) * kTestAmplitude;
    phase += kTwoPi * frequency / static_cast<float>(g_sample_rate);
    if(phase >= kTwoPi)
        phase -= kTwoPi;
    return sample;
}

void AudioCallback(AudioHandle::InputBuffer  input,
                   AudioHandle::OutputBuffer output,
                   std::size_t               size)
{
    const uint32_t interrupted_stage = g_trace.stage;
    const uint32_t audio_start_tick = TIM2->CNT;
    Trace(0x100U | interrupted_stage);
    if(size != kBlockFrames)
        __atomic_fetch_add(&g_audio_size_errors, 1U, __ATOMIC_RELAXED);

    const uint32_t capture_mask = __atomic_load_n(
        &g_capture_channel_mask, __ATOMIC_ACQUIRE);
    const uint32_t playback_mask = __atomic_load_n(
        &g_playback_channel_mask, __ATOMIC_ACQUIRE);
    const uint32_t playback_read
        = __atomic_load_n(&g_playback_read, __ATOMIC_RELAXED);
    const uint32_t playback_write
        = __atomic_load_n(&g_playback_write, __ATOMIC_ACQUIRE);
    const bool control_only = __atomic_load_n(
        &g_control_only, __ATOMIC_ACQUIRE);
    const bool have_playback = !control_only
                               && playback_write != playback_read;
    const AudioBlock* playback
        = have_playback ? &g_playback_ring[playback_read % kRingBlocks]
                        : nullptr;
    if(!control_only && !have_playback && playback_mask != 0U)
        __atomic_fetch_add(&g_playback_underruns, 1U, __ATOMIC_RELAXED);

    const uint32_t capture_write
        = __atomic_load_n(&g_capture_write, __ATOMIC_RELAXED);
    const uint32_t capture_read
        = __atomic_load_n(&g_capture_read, __ATOMIC_ACQUIRE);
    const bool capture_has_space = !control_only
        && capture_write - capture_read < kRingBlocks;
    AudioBlock* capture
        = capture_has_space ? &g_capture_ring[capture_write % kRingBlocks]
                            : nullptr;
    const uint32_t first_sample
        = __atomic_load_n(&g_audio_sample_counter, __ATOMIC_RELAXED);
    if(capture != nullptr)
        capture->sample_counter = first_sample;
    else if(!control_only)
        __atomic_fetch_add(&g_capture_overruns, 1U, __ATOMIC_RELAXED);

    if((capture_mask | playback_mask) == 0U)
    {
        // Keep the audio clock and fixed SPI control cadence alive, but avoid
        // all floating-point conversion/mixing while every route is muted.
        std::memset(output[0], 0, size * sizeof(float));
        std::memset(output[1], 0, size * sizeof(float));
        if(capture != nullptr)
            std::memset(capture->samples, 0, sizeof(capture->samples));
    }
    else
    {
        for(std::size_t channel = 0U; channel < SPI_AUDIO_CHANNELS; ++channel)
            for(std::size_t frame = 0U; frame < size; ++frame)
                g_graph_input[channel][frame]
                    = (capture_mask & (1U << channel)) != 0U
                          ? input[channel][frame] : 0.0f;
        const float* graph_input[SPI_AUDIO_CHANNELS]
            = {g_graph_input[0], g_graph_input[1]};
        float* graph_output[SPI_AUDIO_CHANNELS]
            = {g_graph_output[0], g_graph_output[1]};
        seedfx_graph_process(graph_input, graph_output, size, g_sample_rate,
                             __atomic_load_n(&g_pedalboard_enabled,
                                             __ATOMIC_ACQUIRE),playback_mask);
        for(std::size_t frame = 0U; frame < size; ++frame)
        {
            for(std::size_t channel = 0U; channel < SPI_AUDIO_CHANNELS;
                ++channel)
            {
                const uint32_t bit = 1U << channel;
                const bool capture_enabled = (capture_mask & bit) != 0U;
                const bool playback_enabled = (playback_mask & bit) != 0U;
                const std::size_t sample =
                    frame * SPI_AUDIO_CHANNELS + channel;

                if(capture != nullptr && frame < kBlockFrames)
                {
                    if(capture_enabled)
                    {
                        const float source = kUseTestTones
                            ? (channel == 0U
                                ? NextSine(g_left_phase, kLeftFrequency)
                                : NextSine(g_right_phase, kRightFrequency))
                            : input[channel][frame];
                        capture->samples[sample]
                            = FloatToLeftAlignedPcm24(source);
                    }
                    else
                    {
                        capture->samples[sample] = 0;
                    }
                }

                if(playback_enabled)
                {
                    const float usb_playback =
                        have_playback && frame < kBlockFrames
                            ? LeftAlignedPcm24ToFloat(
                                  playback->samples[sample])
                            : 0.0f;
                    const float local_input = g_graph_output[channel][frame];
                    output[channel][frame] = ClampAudio(
                        local_input + usb_playback * kPlaybackMixGain);
                }
                else
                {
                    output[channel][frame] = 0.0f;
                }
            }
        }
    }

    seed_level_meter_capture(input,output,size,capture_mask,playback_mask);
    __atomic_store_n(&g_audio_sample_counter,
                     first_sample + static_cast<uint32_t>(size),
                     __ATOMIC_RELAXED);
    if(have_playback)
        __atomic_store_n(
            &g_playback_read, playback_read + 1U, __ATOMIC_RELEASE);
    if(capture != nullptr)
        __atomic_store_n(
            &g_capture_write, capture_write + 1U, __ATOMIC_RELEASE);
    g_audio_ticks += TIM2->CNT - audio_start_tick;
    Trace(interrupted_stage);
}

bool InitSpiSlave()
{
    SpiHandle::Config config{};
    config.periph = SpiHandle::Config::Peripheral::SPI_1;
    config.mode = SpiHandle::Config::Mode::SLAVE;
    config.direction = SpiHandle::Config::Direction::TWO_LINES;
    config.datasize = 8U;
    config.clock_polarity = SpiHandle::Config::ClockPolarity::LOW;
    config.clock_phase = SpiHandle::Config::ClockPhase::ONE_EDGE;
    config.nss = SpiHandle::Config::NSS::HARD_INPUT;
    config.pin_config.sclk = daisy::seed::D8;
    config.pin_config.mosi = daisy::seed::D10;
    config.pin_config.miso = daisy::seed::D9;
    config.pin_config.nss = daisy::seed::D7;
    return g_spi.Init(config) == SpiHandle::Result::OK;
}

bool InitDiagnosticUart()
{
    UartHandler::Config config{};
    config.periph = UartHandler::Config::Peripheral::USART_1;
    config.mode = UartHandler::Config::Mode::TX_RX;
    config.pin_config.tx = daisy::seed::D13;
    config.pin_config.rx = daisy::seed::D14;
    config.baudrate = 115200U;
    return g_uart.Init(config) == UartHandler::Result::OK;
}

void DmaFinished(void*, SpiHandle::Result result)
{
    g_ready.Write(false);
    g_dma_result = result;
    __atomic_store_n(&g_dma_done, true, __ATOMIC_RELEASE);
}

void FillHeader(SpiAudioFrame& frame,
                uint32_t       sequence,
                uint32_t       sample_counter,
                uint16_t       frames,
                uint8_t        flags,
                uint16_t       status)
{
    frame.header.magic = SPI_AUDIO_MAGIC;
    frame.header.version = SPI_AUDIO_VERSION;
    frame.header.flags = flags;
    frame.header.header_bytes = SPI_AUDIO_HEADER_BYTES;
    frame.header.sequence = sequence;
    frame.header.sample_rate = g_sample_rate;
    frame.header.frames = frames;
    frame.header.channels = SPI_AUDIO_CHANNELS;
    frame.header.valid_bits = SPI_AUDIO_VALID_BITS;
    frame.header.sample_counter = sample_counter;
    frame.header.payload_bytes = spi_audio_payload_bytes(frames);
    // Publish the already measured foreground/IRQ load in-band. This does not
    // add UART traffic or work to the audio callback and preserves the fixed
    // maximum-size SPI DMA frame.
    frame.header.status = spi_audio_status_with_cpu(status, g_cpu_permille);
    frame.header.crc32 = 0U;
}

void StartNextTransfer()
{
    if(__atomic_load_n(&g_dma_active, __ATOMIC_ACQUIRE))
        return;

    const bool control_only = __atomic_load_n(
        &g_control_only, __ATOMIC_ACQUIRE);
    const uint32_t now_ms = System::GetNow();
    const uint32_t read
        = __atomic_load_n(&g_capture_read, __ATOMIC_RELAXED);
    const uint32_t write
        = __atomic_load_n(&g_capture_write, __ATOMIC_ACQUIRE);
    const uint16_t frames = control_only ? SPI_AUDIO_FRAMES : g_spi_frames;
    const uint32_t blocks = frames / SPI_AUDIO_FRAMES;
    if(control_only)
    {
        if(now_ms - g_last_control_transfer_ms < kControlPeriodMs)
            return;
    }
    else if(write - read < blocks)
    {
        return;
    }

    const uint32_t slot_index = g_tx_sequence % kDmaSlots;
    DmaSlot&       slot = g_dma_slots[slot_index];
    uint32_t sample_counter = __atomic_load_n(
        &g_audio_sample_counter, __ATOMIC_RELAXED);
    if(control_only)
    {
        // The DMA transfer stays fixed at 544 bytes. Zero the entire sample
        // area so LOCAL sends no stale PCM in the unused physical tail.
        std::memset(slot.tx.samples, 0, sizeof(slot.tx.samples));
    }
    else
    {
        sample_counter = g_capture_ring[read % kRingBlocks].sample_counter;
        for(uint32_t block = 0; block < blocks; ++block)
        {
            const AudioBlock& source
                = g_capture_ring[(read + block) % kRingBlocks];
            std::memcpy(slot.tx.samples
                            + block * SPI_AUDIO_FRAMES * SPI_AUDIO_CHANNELS,
                        source.samples, sizeof(source.samples));
        }
    }

    uint8_t flags = SPI_AUDIO_FLAG_VALID;
    if(g_session_start) flags |= SPI_AUDIO_FLAG_SESSION_START;
    if(kUseTestTones)
        flags |= SPI_AUDIO_FLAG_TEST_TONES;
    if(control_only)
        flags |= SPI_AUDIO_FLAG_CONTROL_ONLY;
    if(g_low_latency)
        flags |= SPI_AUDIO_FLAG_LOW_LATENCY;
    uint16_t status = g_sticky_status;
    if(__atomic_load_n(&g_pedalboard_enabled, __ATOMIC_ACQUIRE))
        status |= SPI_AUDIO_STATUS_PEDALBOARD_ENABLED;
    if(__atomic_load_n(&g_capture_overruns, __ATOMIC_RELAXED) != 0U)
    {
        flags |= SPI_AUDIO_FLAG_XRUN;
        status |= SPI_AUDIO_STATUS_CAPTURE_OVERRUN;
    }
    if(__atomic_load_n(&g_playback_underruns, __ATOMIC_RELAXED) != 0U)
    {
        flags |= SPI_AUDIO_FLAG_XRUN;
        status |= SPI_AUDIO_STATUS_PLAYBACK_UNDERRUN;
    }

    FillHeader(slot.tx,
               g_tx_sequence,
               sample_counter,
               frames,
               flags,
               status);
    slot.tx.header.crc32 = spi_audio_frame_crc32(&slot.tx);

    if(control_only)
        g_last_control_transfer_ms = now_ms;
    else
        __atomic_store_n(&g_capture_read, read + blocks, __ATOMIC_RELEASE);
    g_active_slot = slot_index;
    __atomic_store_n(&g_dma_done, false, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dma_active, true, __ATOMIC_RELEASE);
    g_dma_started_ms = System::GetNow();

    Trace(6);
    g_trace.sequence = g_tx_sequence;
    g_trace.audio_frames = g_audio_sample_counter;
    const SpiHandle::Result result = g_spi.DmaTransmitAndReceive(
        reinterpret_cast<uint8_t*>(&slot.tx),
        reinterpret_cast<uint8_t*>(&slot.rx),
        sizeof(SpiAudioFrame),
        nullptr,
        DmaFinished,
        nullptr);
    if(result != SpiHandle::Result::OK)
    {
        g_ready.Write(false);
        ++g_spi_start_errors;
        __atomic_store_n(&g_dma_done, false, __ATOMIC_RELAXED);
        __atomic_store_n(&g_dma_active, false, __ATOMIC_RELEASE);
        Recover("SEED:SPI_START_RECOVER\r\n");
    }

    // DMA is armed before READY rises, so P4 can never clock an unprepared
    // slave transaction.
    g_ready.Write(true);
    Trace(7);
    g_session_start = false;
}

void ChangeAudioRate(uint32_t rate)
{
    // Only called by the foreground after the current SPI DMA completed.
    g_ready.Write(false);
    Trace(3);
    g_seed.StopAudio();
    Trace(4);
    if(!SeedConfigureAudioClock(g_seed, rate)) Recover("SEED:CLOCK_RECOVER\r\n");
    g_sample_rate = rate;
    g_spi_frames = g_low_latency
        ? SPI_AUDIO_FRAMES : spi_audio_balanced_frames(rate);
    g_capture_write = g_capture_read = 0;
    g_playback_write = g_playback_read = 0;
    g_audio_sample_counter = 0;
    g_left_phase = g_right_phase = 0;
    g_session_start = true;
    Trace(5);
    g_seed.StartAudio(AudioCallback);
}

void QueuePlayback(const SpiAudioFrame& frame)
{
    uint32_t write
        = __atomic_load_n(&g_playback_write, __ATOMIC_RELAXED);
    const uint32_t read
        = __atomic_load_n(&g_playback_read, __ATOMIC_ACQUIRE);
    const uint32_t blocks = frame.header.frames / SPI_AUDIO_FRAMES;
    if(write - read + blocks > kRingBlocks)
        return;

    for(uint32_t block = 0; block < blocks; ++block)
    {
        AudioBlock& destination = g_playback_ring[write % kRingBlocks];
        destination.sample_counter
            = frame.header.sample_counter + block * SPI_AUDIO_FRAMES;
        std::memcpy(destination.samples,
                    frame.samples
                        + block * SPI_AUDIO_FRAMES * SPI_AUDIO_CHANNELS,
                    sizeof(destination.samples));
        ++write;
    }
    __atomic_store_n(&g_playback_write, write, __ATOMIC_RELEASE);
}

void FinishTransfer()
{
    if(!__atomic_load_n(&g_dma_done, __ATOMIC_ACQUIRE))
        return;

    const uint32_t slot_index = g_active_slot;
    const SpiHandle::Result result = g_dma_result;
    __atomic_store_n(&g_dma_done, false, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dma_active, false, __ATOMIC_RELEASE);
    ++g_tx_sequence;

    if(result != SpiHandle::Result::OK)
    {
        ++g_spi_start_errors;
        Recover("SEED:SPI_DMA_RECOVER\r\n");
    }

    const SpiAudioFrame& rx = g_dma_slots[slot_index].rx;
    if(!spi_audio_header_is_valid(&rx.header))
    {
        ++g_rx_header_errors;
        g_sticky_status |= SPI_AUDIO_STATUS_RX_HEADER_ERROR;
        return;
    }
    if(spi_audio_frame_crc32(&rx) != rx.header.crc32)
    {
        ++g_rx_crc_errors;
        g_sticky_status |= SPI_AUDIO_STATUS_RX_CRC_ERROR;
        return;
    }
    if(rx.header.flags & SPI_AUDIO_FLAG_SESSION_START) g_have_rx_sequence = false;
    if(g_have_rx_sequence && rx.header.sequence != g_expected_rx_sequence)
    {
        ++g_rx_sequence_errors;
        g_sticky_status |= SPI_AUDIO_STATUS_RX_SEQUENCE_ERROR;
    }
    g_expected_rx_sequence = rx.header.sequence + 1U;
    g_have_rx_sequence = true;
    __atomic_store_n(
        &g_pedalboard_enabled,
        (rx.header.flags & SPI_AUDIO_FLAG_PEDALBOARD_ENABLED) != 0U,
        __ATOMIC_RELEASE);
    const bool requested_control_only
        = (rx.header.flags & SPI_AUDIO_FLAG_CONTROL_ONLY) != 0U;
    const bool requested_low_latency
        = (rx.header.flags & SPI_AUDIO_FLAG_LOW_LATENCY) != 0U;
    const bool mode_changed = requested_control_only
        != __atomic_load_n(&g_control_only, __ATOMIC_ACQUIRE)
        || requested_low_latency != g_low_latency;
    if(mode_changed)
    {
        g_ready.Write(false);
        const uint32_t primask = __get_PRIMASK();
        __disable_irq();
        g_capture_write = g_capture_read = 0;
        g_playback_write = g_playback_read = 0;
        __atomic_store_n(&g_control_only, requested_control_only,
                         __ATOMIC_RELAXED);
        __set_PRIMASK(primask);
        g_session_start = true;
    }
    g_low_latency = requested_low_latency;
    g_spi_frames = g_low_latency
        ? SPI_AUDIO_FRAMES : spi_audio_balanced_frames(rx.header.sample_rate);
    if(!mode_changed)
        __atomic_store_n(&g_control_only, requested_control_only,
                         __ATOMIC_RELEASE);
    if((rx.header.status & SPI_AUDIO_CONTROL_CHANNEL_MASKS_VALID) != 0U)
    {
        __atomic_store_n(
            &g_capture_channel_mask,
            spi_audio_control_capture_mask(rx.header.status),
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &g_playback_channel_mask,
            spi_audio_control_playback_mask(rx.header.status),
            __ATOMIC_RELEASE);
    }
    if(rx.header.sample_rate != g_sample_rate)
    {
        ChangeAudioRate(rx.header.sample_rate);
        ++g_good_transactions;
        return; // This TX payload belongs to the previous rate/epoch.
    }
    if(!requested_control_only)
        QueuePlayback(rx);
    ++g_good_transactions;
}
} // namespace

// libDaisy's default hard-fault handler breaks forever. Keep a bounded reboot
// plus the fault PC in NOLOAD SRAM instead, using a project-owned RAM vector.
extern "C" __attribute__((noreturn)) void SeedRecordHardFault(uint32_t* stack, uint32_t exc_return)
{
    (void)exc_return;
    g_trace.magic = 0x52554332U;
    g_trace.fault = SCB->CFSR;
    // Cortex-M7 puts R0..xPSR first even in an extended FP frame (PM0253
    // figure 11). Do not dereference an invalid or failed-stacking frame.
    const uintptr_t address = reinterpret_cast<uintptr_t>(stack);
    const bool valid_stack = ((address >= 0x20000000U && address <= 0x2001ffe0U)
        || (address >= 0x24000000U && address <= 0x2407ffe0U))
        && !(g_trace.fault & (SCB_CFSR_STKERR_Msk | SCB_CFSR_MSTKERR_Msk));
    g_trace.pc = valid_stack ? stack[6] : 0;
    g_trace.lr = valid_stack ? stack[5] : 0;
    g_trace.address = SCB->BFAR;
    NVIC_SystemReset();
    while(true) {}
}
extern "C" __attribute__((naked)) void __wrap_HardFault_Handler()
{
    __asm volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n mov r1, lr\n b SeedRecordHardFault");
}

#define SEED_TRACE_IRQ(name, marker) \
    extern "C" void name(); \
    extern "C" void __wrap_##name() { \
        uint32_t prior = g_trace.stage; Trace(prior | marker); \
        name(); Trace(prior); \
    }
SEED_TRACE_IRQ(SPI1_IRQHandler, 0x200U)
SEED_TRACE_IRQ(DMA2_Stream2_IRQHandler, 0x400U)
SEED_TRACE_IRQ(DMA2_Stream3_IRQHandler, 0x800U)
SEED_TRACE_IRQ(DMA1_Stream0_IRQHandler, 0x1000U)
SEED_TRACE_IRQ(DMA1_Stream1_IRQHandler, 0x2000U)
SEED_TRACE_IRQ(USART1_IRQHandler, 0x4000U)
#undef SEED_TRACE_IRQ

alignas(1024) static uint32_t recovery_vectors[256];
static void InstallRecoveryVectors()
{
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    std::memcpy(recovery_vectors, reinterpret_cast<const void*>(SCB->VTOR), sizeof(recovery_vectors));
    recovery_vectors[3] = reinterpret_cast<uint32_t>(__wrap_HardFault_Handler);
    recovery_vectors[16 + SPI1_IRQn] = reinterpret_cast<uint32_t>(__wrap_SPI1_IRQHandler);
    recovery_vectors[16 + DMA2_Stream2_IRQn] = reinterpret_cast<uint32_t>(__wrap_DMA2_Stream2_IRQHandler);
    recovery_vectors[16 + DMA2_Stream3_IRQn] = reinterpret_cast<uint32_t>(__wrap_DMA2_Stream3_IRQHandler);
    recovery_vectors[16 + DMA1_Stream0_IRQn] = reinterpret_cast<uint32_t>(__wrap_DMA1_Stream0_IRQHandler);
    recovery_vectors[16 + DMA1_Stream1_IRQn] = reinterpret_cast<uint32_t>(__wrap_DMA1_Stream1_IRQHandler);
    recovery_vectors[16 + USART1_IRQn] = reinterpret_cast<uint32_t>(__wrap_USART1_IRQHandler);
    SCB_CleanDCache_by_Addr(recovery_vectors, sizeof(recovery_vectors));
    __DSB();
    SCB->VTOR = reinterpret_cast<uint32_t>(recovery_vectors);
    __DSB(); __ISB();
    __set_PRIMASK(primask);
}

int main(void)
{
    g_seed.Init();
    seedfx_graph_init();
    InstallRecoveryVectors();
    // ~2 s from the independent LSI clock; refresh only in foreground.
    // A wedged DMA/HAL/IRQ cannot keep the board stuck indefinitely.
    // Software-mode IWDG is reset by the MCU reset; ROM DFU stays usable.
    g_watchdog.Instance = IWDG1;
    g_watchdog.Init.Prescaler = IWDG_PRESCALER_64;
    g_watchdog.Init.Reload = 999;
    g_watchdog.Init.Window = IWDG_WINDOW_DISABLE;
    if(HAL_IWDG_Init(&g_watchdog) != HAL_OK) NVIC_SystemReset();
    if(!InitDiagnosticUart())
    {
        g_seed.SetLed(true);
        while(true) {}
    }
    SendUart("SEED:BOOT\r\n");
    if(g_trace.magic == 0x52554332U)
    {
        char reason[160];
        std::snprintf(reason, sizeof(reason), "SEED:PREVIOUS stage=%lu seq=%lu audio=%lu reset=%08lx\r\n",
            (unsigned long)g_trace.stage, (unsigned long)g_trace.sequence,
            (unsigned long)g_trace.audio_frames, (unsigned long)RCC->RSR);
        SendUart(reason);
        std::snprintf(reason, sizeof(reason), "SEED:FAULT cfsr=%08lx pc=%08lx lr=%08lx addr=%08lx\r\n",
            (unsigned long)g_trace.fault, (unsigned long)g_trace.pc,
            (unsigned long)g_trace.lr, (unsigned long)g_trace.address);
        SendUart(reason);
    }
    g_trace.fault = g_trace.pc = g_trace.lr = g_trace.address = 0;
    __HAL_RCC_CLEAR_RESET_FLAGS();
    Trace(0);
    // DMA SRAM is NOLOAD and survives a software reset. libDaisy enables IDLE
    // before arming RX, so an already pending IRQ can expose the old buffer.
    // Never replay a retained @BOOT/@RESET command from the previous session.
    std::memset(g_uart_rx, 0, sizeof(g_uart_rx));
    if(g_uart.DmaListenStart(g_uart_rx, sizeof(g_uart_rx), UartReceive, nullptr)
       != UartHandler::Result::OK) Fatal("SEED:UART_RX_FAIL\r\n");

    // Do not prevent startup solely on revision detection. Some Seed3 carrier
    // layouts do not expose the PH6 revision strap exactly as a bare module.
    const bool detected_seed3
        = g_seed.CheckBoardVersion() == DaisySeed::BoardVersion::DAISY_SEED_3;
    SendUart(detected_seed3 ? "SEED:REV3\r\n" : "SEED:REV_UNKNOWN\r\n");

    g_ready.Init(daisy::seed::D0,
                 GPIO::Mode::OUTPUT,
                 GPIO::Pull::NOPULL,
                 GPIO::Speed::VERY_HIGH);
    g_ready.Write(false);
    if(!InitSpiSlave())
        Fatal("SEED:SPI_INIT_FAIL\r\n");
    // libDaisy clears its active-DMA owner in the SPI EOT callback. If RX/TX
    // DMA IRQs pend together, RX enables EOT and SPI1's lower IRQ number can
    // win before TX: the orphan TX IRQ is then never acknowledged. This is
    // readily hit at 96 kHz. Drain TX first, RX second, SPI completion last.
    HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 0, 0);
    HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, 1, 0);
    HAL_NVIC_SetPriority(SPI1_IRQn, 2, 0);
    SendUart("SEED:SPI_OK\r\n");

    g_seed.SetAudioBlockSize(kBlockFrames);
    if(!SeedConfigureAudioClock(g_seed, g_sample_rate)) Fatal("SEED:CLOCK_FAIL\r\n");
    SendUart(kUseTestTones ? "SEED:CAPTURE_TEST_TONES\r\n" : "SEED:CAPTURE_ADC\r\n");
    g_seed.StartAudio(AudioCallback);
    SendUart("SEED:AUDIO_START\r\n");

    uint32_t last_led_transaction = 0U;
    bool     led = false;
    g_window_tick = TIM2->CNT;
    g_window_ms = System::GetNow();
    while(true)
    {
        HAL_IWDG_Refresh(&g_watchdog);
        if(__atomic_exchange_n(&g_uart_restart, false, __ATOMIC_ACQ_REL))
        {
            // P4 power/reset can glitch the UART line. Re-arm after HAL aborts
            // circular RX on framing/overrun errors; no reset button required.
            g_uart.DmaListenStop();
            std::memset(g_uart_rx, 0, sizeof(g_uart_rx));
            if(g_uart.DmaListenStart(g_uart_rx, sizeof(g_uart_rx), UartReceive, nullptr)
               != UartHandler::Result::OK) Recover("SEED:UART_RECOVER\r\n");
        }
        const uint32_t command = __atomic_exchange_n(&g_uart_command, 0U, __ATOMIC_ACQ_REL);
        ProcessSeedFxControl();
        if(command == 1U)
        {
            g_ready.Write(false);
            SendUart("SEED:REMOTE_BOOT\r\n");
            g_seed.StopAudio();
            System::ResetToBootloader(System::BootloaderMode::STM);
        }
        if(command == 2U) Recover("SEED:REMOTE_RESET\r\n");
        if(command == 3U)
        {
            // P4 only requests this with both USB streams closed. Blocking
            // UART formatting/transmit must not disturb an active audio test.
            char report[288];
            const bool local = __atomic_load_n(
                &g_control_only, __ATOMIC_ACQUIRE);
            SeedFxGraphStats graph_stats{};
            seedfx_graph_get_stats(graph_stats);
            std::snprintf(report, sizeof(report),
                "SEED:CPU rate=%lu mode=%s frames=%u busy_permille=%lu audio_permille=%lu capture_overruns=%lu spi_errors=%lu graph=%lu/%lu cache=%lu/%lu commits=%lu rejects=%lu\r\n",
                (unsigned long)g_sample_rate,
                local ? "LOCAL" : (g_low_latency ? "LOW" : "BALANCED"),
                (unsigned)(local ? SPI_AUDIO_FRAMES : g_spi_frames),
                (unsigned long)g_cpu_permille,
                (unsigned long)g_audio_permille,
                (unsigned long)g_capture_overruns,
                (unsigned long)g_spi_start_errors,
                (unsigned long)graph_stats.active_revision,
                (unsigned long)graph_stats.active_nodes,
                (unsigned long)graph_stats.cache_bytes_used,
                (unsigned long)graph_stats.cache_capacity,
                (unsigned long)graph_stats.graph_commits,
                (unsigned long)graph_stats.graph_rejects);
            SendUart(report);
        }
        if(__atomic_load_n(&g_dma_active, __ATOMIC_ACQUIRE)
           && System::GetNow() - g_dma_started_ms > 500U)
            Recover("SEED:SPI_TIMEOUT_RECOVER\r\n");
        Trace(2);
        FinishTransfer();
        Trace(8);
        StartNextTransfer();
        if(g_good_transactions - last_led_transaction >= 750U)
        {
            last_led_transaction = g_good_transactions;
            led = !led;
            g_seed.SetLed(led);
        }
        Trace(9);
        const uint32_t now_ms = System::GetNow();
        seed_level_meter_poll(now_ms);
        if(now_ms - g_window_ms >= 1000U)
        {
            const uint32_t primask = __get_PRIMASK();
            __disable_irq();
            const uint32_t now_tick = TIM2->CNT;
            const uint32_t elapsed = now_tick - g_window_tick;
            const uint32_t idle = g_idle_ticks;
            const uint32_t audio = g_audio_ticks;
            g_idle_ticks = g_audio_ticks = 0;
            g_window_tick = now_tick;
            g_window_ms = now_ms;
            __set_PRIMASK(primask);
            if(elapsed) {
                g_cpu_permille = 1000U - uint64_t(idle) * 1000U / elapsed;
                g_audio_permille = uint64_t(audio) * 1000U / elapsed;
            }
        }
        // Sleep only when there is no work. Mask around check+WFI so an IRQ
        // cannot complete between them and lose a wakeup; a pending IRQ wakes
        // Cortex-M WFI even with PRIMASK set, then runs immediately on unmask.
        const uint32_t primask = __get_PRIMASK();
        __disable_irq();
        const bool control_only = __atomic_load_n(
            &g_control_only, __ATOMIC_RELAXED);
        const uint32_t ready_blocks = control_only
            ? 0U : g_spi_frames / SPI_AUDIO_FRAMES;
        const bool audio_ready = !control_only
            && g_capture_write - g_capture_read >= ready_blocks;
        const bool control_due = control_only
            && System::GetNow() - g_last_control_transfer_ms
                   >= kControlPeriodMs;
        if(!g_dma_done && (g_dma_active || (!audio_ready && !control_due))
           && !g_uart_command && !g_uart_restart)
        {
            const uint32_t start = TIM2->CNT;
            __DSB(); __WFI();
            g_idle_ticks += TIM2->CNT - start;
        }
        __set_PRIMASK(primask);
    }
}
