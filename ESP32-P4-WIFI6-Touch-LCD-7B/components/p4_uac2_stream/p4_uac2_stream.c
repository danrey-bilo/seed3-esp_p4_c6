#include "p4_uac2_stream.h"
#include "audio_ring.h"
#include "audio_ring_test.h"
#include "audio_format.h"
#include "usb_profile.h"
#include "device/usbd_pvt.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

// All USB state has one task owner on core 0. IRQ shares only bounded metadata.
// The SPI core accesses only the two SPSC rings, atomic flags and counters.
enum { CAP = 0, PLAY = 1, FB = 2, SLOT_COUNT = 4, DMA_STRIDE = (UAC_MAX_BYTES + 63) & ~63,
       FREE = 0, READY, IN_FLIGHT, DONE, BUILDING };
_Static_assert(P4_UAC2_RING_FRAMES == AUDIO_RING_CAPACITY, "Public/private ring capacity mismatch");
typedef struct { uint16_t bytes, frames; uint8_t state; } slot_t;
typedef struct {
    uint8_t address;
    int8_t current;
    bool enabled, pending, armed, seen_completion;
    uint16_t due, last_completion, armed_at;
    uint32_t last_progress_tick;
    uint32_t produced, consumed;
    slot_t slots[SLOT_COUNT];
} endpoint_t;

static DMA_ATTR __attribute__((aligned(64))) int32_t capture_dma[SLOT_COUNT][DMA_STRIDE / 4];
static DMA_ATTR __attribute__((aligned(64))) int32_t playback_dma[SLOT_COUNT][DMA_STRIDE / 4];
static DMA_ATTR __attribute__((aligned(64))) uint32_t feedback_dma[2][16];
static DMA_ATTR __attribute__((aligned(64))) uint8_t silence_dma[DMA_STRIDE];
static DMA_ATTR __attribute__((aligned(64))) uint8_t sink_dma[DMA_STRIDE];
static audio_ring_t capture_ring, playback_ring;
static endpoint_t eps[3];
static p4_uac2_stats_t stats;
static uint32_t cap_active, play_active, play_epoch, initialised;
static uint32_t requested_rate = 48000, runtime_rate = 48000, confirmed_rate;
static uint32_t capture_selected_rate = 48000, playback_selected_rate = 48000;
static uint32_t rate_conflict;
static uint32_t source_epoch, applied_source_epoch;
static uint32_t buffer_ms = 1, prefill_frames = 64, applied_buffer_ms = 1;
static uint32_t cap_frame_bytes = UAC_FRAME_BYTES, play_frame_bytes = UAC_FRAME_BYTES, packet_phase;
static uint32_t queued_frames, source_frames, last_source, last_source_ms;
static uint32_t usb_ticks, clock_ticks, clock_frames;
static uint32_t host_alive, host_sof_age_ms = UINT32_MAX;
static uint32_t rate_q16 = 6u << 16, fill_q8 = P4_UAC2_PREFILL << 8;
static uint32_t last_feedback_ms;
static uint16_t last_sof;
static uint8_t alternates[3], muted[3];
static int16_t volume[3];
static int32_t gain_q24[3] = {1 << 24, 1 << 24, 1 << 24};
static uint8_t control_buffer[64];
static bool suspended, capture_started, capture_start_trimmed;
static int packet_mode;
static TaskHandle_t usb_task_handle;
static usb_phy_handle_t phy;
extern bool p4_dwc2_faulted(void);
extern uint32_t p4_dwc2_frame_number(void);

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define ADD(x, v) __atomic_fetch_add(&(x), (v), __ATOMIC_RELAXED)
static uint16_t frame_delta(uint16_t a, uint16_t b) { return (a - b) & 0x3fffu; }
static unsigned interval(void) { return tud_speed_get() == TUSB_SPEED_HIGH ? 4 : 1; }
static unsigned packet_hz(void) { return tud_speed_get() == TUSB_SPEED_HIGH ? 2000 : 1000; }
static unsigned nominal_frames(void) { return (LOAD(runtime_rate) + packet_hz() - 1) / packet_hz(); }
static bool source_ready(void) {
    return LOAD(confirmed_rate) == LOAD(runtime_rate) &&
           LOAD(requested_rate) == LOAD(runtime_rate) &&
           LOAD(source_epoch) == LOAD(applied_source_epoch);
}
static void limits(uint32_t fill, uint32_t *minimum, uint32_t *maximum) {
    if (fill < LOAD(*minimum)) STORE(*minimum, fill);
    if (fill > LOAD(*maximum)) STORE(*maximum, fill);
}
static void state_clear(unsigned index) {
    endpoint_t *e = &eps[index];
    memset(e, 0, sizeof(*e));
    e->address = index == CAP ? UAC_CAP_EP : index == PLAY ? UAC_PLAY_EP : UAC_FB_EP;
    e->current = -1;
}
// Caller holds TinyUSB's recursive, short spin critical section (not a mutex).
// At runtime this is invoked only in USB IRQ or in the USB task on core 0.
static void arm(unsigned index, uint16_t now) {
    endpoint_t *e = &eps[index];
    if (!e->enabled || !e->pending || e->armed || p4_dwc2_faulted()) return;
    uint8_t *buffer;
    uint16_t bytes;
    if (index == CAP) {
        unsigned slot = e->consumed % SLOT_COUNT;
        if (e->slots[slot].state == READY) {
            e->current = slot;
            e->slots[slot].state = IN_FLIGHT;
            ++e->consumed;
            buffer = (uint8_t *)capture_dma[slot];
            bytes = e->slots[slot].bytes;
        } else {
            if (!capture_started) return; // first packet waits for real-frame prefill
            e->current = -1;
            buffer = silence_dma;
            bytes = nominal_frames() * LOAD(cap_frame_bytes); // NEVER a ZLP
        }
    } else if (index == PLAY) {
        unsigned slot = e->produced % SLOT_COUNT;
        if (e->slots[slot].state == FREE) {
            e->current = slot;
            e->slots[slot].state = IN_FLIGHT;
            buffer = (uint8_t *)playback_dma[slot];
        } else {
            e->current = -1;
            buffer = sink_dma; // keep receiving; report, never overwrite a busy slot
        }
        bytes = UAC_MAX_FRAMES * LOAD(play_frame_bytes);
    } else {
        e->current = (e->current + 1) & 1;
        buffer = (uint8_t *)feedback_dma[e->current];
        bytes = 4;
    }
    e->pending = false;
    e->armed = true;
    e->armed_at = now;
    if (!usbd_edpt_xfer(UAC_PORT, e->address, buffer, bytes)) {
        e->armed = false;
        // Do not reuse a buffer following an uncertain hardware submission.
        // Task watchdog reactivates the endpoint, then clears metadata.
        e->armed_at = (now - 64) & 0x3fff;
    }
}

static bool transfer_isr(uint8_t port, uint8_t address, xfer_result_t result, uint32_t bytes) {
    (void)port;
    unsigned index = address == UAC_CAP_EP ? CAP : address == UAC_PLAY_EP ? PLAY : FB;
    endpoint_t *e = &eps[index];
    const uint16_t now = p4_dwc2_frame_number();
    // IRQ owns these fields while the task cannot preempt it on core 0.
    e->armed = false;
    if (!e->enabled) return true;
    if (result == XFER_RESULT_SUCCESS) {
        if (e->seen_completion) {
            uint32_t gap = frame_delta(now, e->last_completion);
            if (index == CAP && gap > LOAD(stats.capture_max_gap_uframes)) STORE(stats.capture_max_gap_uframes, gap);
            if (index == PLAY && gap > LOAD(stats.playback_max_gap_uframes)) STORE(stats.playback_max_gap_uframes, gap);
        }
        e->seen_completion = true;
        e->last_completion = now;
        e->last_progress_tick = xTaskGetTickCountFromISR();
        if (index == CAP) {
            ADD(stats.capture_packets, 1);
            unsigned frames = bytes / LOAD(cap_frame_bytes);
            ADD(stats.capture_completed_frames, frames);
            if (frames < nominal_frames()) ADD(stats.capture_short_packets, 1);
            if (frames > nominal_frames()) ADD(stats.capture_long_packets, 1);
            if (e->current >= 0) {
                slot_t *s = &e->slots[e->current];
                queued_frames -= s->frames;
                s->state = FREE;
            } else ADD(stats.capture_silence_frames, frames);
        } else if (index == PLAY) {
            ADD(stats.playback_packets, 1);
            unsigned frame_bytes = LOAD(play_frame_bytes);
            if (bytes < nominal_frames() * frame_bytes) ADD(stats.playback_short_packets, 1);
            if (bytes > nominal_frames() * frame_bytes) ADD(stats.playback_long_packets, 1);
            if (e->current >= 0) {
                slot_t *s = &e->slots[e->current];
                s->bytes = bytes;
                s->state = DONE;
                ++e->produced;
            } else ADD(stats.playback_overrun_frames, bytes / frame_bytes);
            ADD(stats.playback_received_frames, bytes / frame_bytes);
        } else ADD(stats.feedback_packets, 1);
        // Completion learns host phase. Arm one uframe before the next poll,
        // not in every intervening uframe (bInterval=3 => four HS uframes).
        e->due = (now + interval() - 1) & 0x3fff;
    } else {
        if (index == CAP) {
            ADD(stats.capture_incomplete, 1);
            // Retry the SAME packet; do not consume another ring block.
            if (e->current >= 0) {
                --e->consumed;
                e->slots[e->current].state = READY;
            }
        } else if (index == PLAY) {
            ADD(stats.playback_incomplete, 1);
            if (e->current >= 0) e->slots[e->current].state = FREE;
        } else ADD(stats.feedback_incomplete, 1);
        e->due = now;
    }
    e->pending = true;
    if (interval() == 1 || result != XFER_RESULT_SUCCESS) arm(index, now);
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(usb_task_handle, &wake);
    if (wake) portYIELD_FROM_ISR();
    return true; // no queued class callbacks, no PCM handling in ISR
}
static void sof_isr(uint8_t port, uint32_t frame) {
    (void)port;
    uint16_t now = frame & 0x3fff;
    usb_ticks += frame_delta(now, last_sof);
    last_sof = now;
    for (unsigned i = 0; i < 3; ++i) {
        endpoint_t *e = &eps[i];
        if (e->pending && frame_delta(now, e->due) < 8192) arm(i, now);
    }
}

static bool endpoint_restart(unsigned index, bool enable) {
    // Stop DMA before changing ownership. Failure requests a controller reset.
    usbd_spin_lock(false);
    eps[index].enabled = false;
    eps[index].pending = false;
    unsigned itf = index == CAP ? UAC_CAP_ITF : UAC_PLAY_ITF;
    uint8_t alt = alternates[itf] ? alternates[itf] : UAC_STREAM_ALT;
    bool ok = usbd_edpt_iso_activate(UAC_PORT, uac_endpoint(eps[index].address, alt));
    if (ok) {
        state_clear(index);
        eps[index].enabled = enable;
        eps[index].armed_at = p4_dwc2_frame_number();
        eps[index].last_completion = eps[index].armed_at;
        eps[index].last_progress_tick = xTaskGetTickCount();
        if (index != CAP && enable) {
            eps[index].pending = true;
            eps[index].due = eps[index].armed_at;
            arm(index, eps[index].due);
        }
    }
    usbd_spin_unlock(false);
    return ok;
}
static bool stream_set(unsigned itf, uint8_t alt) {
    if (itf != UAC_CAP_ITF && itf != UAC_PLAY_ITF) return false;
    if (alt > UAC_STREAM_ALT) return false;
    bool enable = alt != 0 && !suspended;
    // Stop/reconfigure under the recursive USB spin lock so an old completion
    // cannot be counted with the new subslot width.
    usbd_spin_lock(false);
    alternates[itf] = alt;
    if (itf == UAC_CAP_ITF) {
        STORE(cap_active, 0);
        if (alt) {
            STORE(cap_frame_bytes, UAC_FRAME_BYTES);
            STORE(capture_selected_rate, LOAD(requested_rate));
        }
        if (!endpoint_restart(CAP, enable)) { usbd_spin_unlock(false); return false; }
        queued_frames = 0;
        capture_started = false;
        capture_start_trimmed = false;
        packet_mode = 0;
        packet_phase = 0;
        ADD(stats.capture_discard_frames, audio_ring_discard(&capture_ring));
        STORE(cap_active, enable);
    } else {
        STORE(play_active, 0);
        if (alt) {
            STORE(play_frame_bytes, UAC_FRAME_BYTES);
            STORE(playback_selected_rate, LOAD(requested_rate));
        }
        if (!endpoint_restart(PLAY, enable) || !endpoint_restart(FB, enable)) {
            usbd_spin_unlock(false); return false;
        }
        ADD(play_epoch, 1); // playback consumer owns and flushes its tail
        fill_q8 = LOAD(prefill_frames) << 8;
        STORE(play_active, enable);
    }
    usbd_spin_unlock(false);
    if (!LOAD(cap_active) && !LOAD(play_active)) STORE(rate_conflict, 0);
    return true;
}
static void driver_reset(uint8_t port) {
    (void)port;
    usbd_spin_lock(false);
    STORE(cap_active, 0);
    STORE(play_active, 0);
    ADD(play_epoch, 1);
    for (unsigned i = 0; i < 3; ++i) state_clear(i);
    memset(alternates, 0, sizeof(alternates));
    queued_frames = 0;
    capture_started = false;
    capture_start_trimmed = false;
    STORE(rate_conflict, 0);
    suspended = false;
    last_sof = p4_dwc2_frame_number();
    usbd_spin_unlock(false);
    ADD(stats.capture_discard_frames, audio_ring_discard(&capture_ring));
    ADD(stats.usb_resets, 1);
}
static void driver_init(void) { for (unsigned i = 0; i < 3; ++i) state_clear(i); }
static bool driver_deinit(void) { driver_reset(UAC_PORT); return true; }
static uint16_t driver_open(uint8_t port, const tusb_desc_interface_t *itf, uint16_t length) {
    if (itf->bInterfaceNumber != 0 || itf->bInterfaceClass != TUSB_CLASS_AUDIO ||
        itf->bInterfaceSubClass != AUDIO_SUBCLASS_CONTROL || itf->bInterfaceProtocol != 0x20) return 0;
    for (unsigned i = 0; i < 3; ++i) {
        // DWC2 dedicated TX FIFOs need at least 16 words, even for 4-byte feedback.
        // Reserve rounded FIFO capacity independently of descriptor wMaxPacketSize.
        if (!usbd_edpt_iso_alloc(port, eps[i].address, i == FB ? 64 : DMA_STRIDE)) return 0;
    }
    if (tud_speed_get() != TUSB_SPEED_HIGH && LOAD(requested_rate) > 48000)
        STORE(requested_rate, 48000);
    rate_q16 = ((uint64_t)LOAD(runtime_rate) << 16) /
               (tud_speed_get() == TUSB_SPEED_HIGH ? 8000 : 1000);
    feedback_dma[0][0] = feedback_dma[1][0] = rate_q16 * interval();
    // Only the class ISR gets SOF; do not queue 8000 user SOF callbacks/second.
    usbd_sof_enable(port, SOF_CONSUMER_AUDIO, true);
    return length; // owns the complete three-interface IAD function
}
static bool control_impl(uint8_t port, uint8_t stage, const tusb_control_request_t *r) {
    uint8_t itf = (uint8_t)r->wIndex;
    uint8_t entity = r->wIndex >> 8;
    uint8_t selector = r->wValue >> 8;
    uint8_t channel = r->wValue;
    bool input = r->bmRequestType_bit.direction == TUSB_DIR_IN;
    if (stage != CONTROL_STAGE_SETUP) {
        if (stage == CONTROL_STAGE_DATA && !input && entity == 4 && itf == 0 &&
            selector == AUDIO_CS_CTRL_SAM_FREQ && channel == 0 && r->wLength == 4) {
            uint32_t rate;
            memcpy(&rate, control_buffer, 4);
            if (!audio_rate_supported(rate) ||
                (tud_speed_get() != TUSB_SPEED_HIGH && rate > 48000)) return false;
            // One physical clock. Never silently retune another live stream.
            if (rate != LOAD(requested_rate) && (LOAD(cap_active) || LOAD(play_active))) {
                STORE(rate_conflict, 1);
                ADD(stats.rate_conflicts, 1);
                return false;
            }
            if (rate != LOAD(requested_rate)) {
                STORE(requested_rate, rate);
                ADD(stats.rate_changes, 1);
            }
            STORE(rate_conflict, 0);
            // A shared UAC2 clock control has no direction identifier. While
            // one endpoint is live its owner is unambiguous; otherwise the
            // value is latched by that endpoint's following SET_INTERFACE.
            if (LOAD(cap_active) && !LOAD(play_active)) {
                STORE(capture_selected_rate, rate);
            } else if (LOAD(play_active) && !LOAD(cap_active)) {
                STORE(playback_selected_rate, rate);
            }
        }
        if (stage == CONTROL_STAGE_DATA && !input && entity == 2 &&
            selector == AUDIO_FU_CTRL_MUTE && channel <= 2 && r->wLength == 1)
            STORE(muted[channel], control_buffer[0] != 0);
        if (stage == CONTROL_STAGE_DATA && !input && entity == 2 &&
            selector == AUDIO_FU_CTRL_VOLUME && channel <= 2 && r->wLength == 2) {
            int16_t db;
            memcpy(&db, control_buffer, 2);
            if (db < -50 * 256 || db > 0 || db % 256) return false;
            STORE(volume[channel], db);
            STORE(gain_q24[channel], (int32_t)lroundf(powf(10.f, db / (256.f * 20.f)) * (1 << 24)));
        }
        return true;
    }
    if (r->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        if (r->bRequest == TUSB_REQ_GET_INTERFACE && input && itf < 3 && r->wLength == 1)
            return tud_control_xfer(port, r, &alternates[itf], 1);
        if (r->bRequest == TUSB_REQ_SET_INTERFACE && !input && r->wValue <= UAC_STREAM_ALT && !r->wLength) {
            if (!stream_set(itf, r->wValue)) return false;
            if (r->wValue && itf == UAC_CAP_ITF) ADD(stats.capture_opens, 1);
            if (r->wValue && itf == UAC_PLAY_ITF) ADD(stats.playback_opens, 1);
            return tud_control_status(port, r);
        }
        return false;
    }
    if (r->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS || itf != 0 ||
        r->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE) return false;
    memset(control_buffer, 0, sizeof(control_buffer));
    if (entity == 4 && channel == 0) {
        if (selector == AUDIO_CS_CTRL_SAM_FREQ) {
            if (r->bRequest == AUDIO_CS_REQ_CUR && r->wLength == 4) {
                const uint32_t rate = LOAD(requested_rate);
                memcpy(control_buffer, &rate, 4);
                return tud_control_xfer(port, r, control_buffer, 4);
            }
            if (input && r->bRequest == AUDIO_CS_REQ_RANGE) {
                const uint32_t rates[] = {44100, 48000, 88200, 96000};
                unsigned count = tud_speed_get() == TUSB_SPEED_HIGH ? 4 : 2;
                control_buffer[0] = count;
                for (unsigned i = 0; i < count; ++i) {
                    memcpy(control_buffer + 2 + i * 12, &rates[i], 4);
                    memcpy(control_buffer + 6 + i * 12, &rates[i], 4);
                }
                return tud_control_xfer(port, r, control_buffer, 2 + count * 12);
            }
        }
        if (input && selector == AUDIO_CS_CTRL_CLK_VALID && r->bRequest == AUDIO_CS_REQ_CUR && r->wLength == 1) {
            control_buffer[0] = source_ready() && last_source &&
                (uint32_t)(esp_timer_get_time() / 1000 - last_source_ms) < 100;
            return tud_control_xfer(port, r, control_buffer, 1);
        }
    }
    if (entity == 2 && channel <= 2) {
        if (selector == AUDIO_FU_CTRL_MUTE && r->bRequest == AUDIO_CS_REQ_CUR && r->wLength == 1) {
            control_buffer[0] = LOAD(muted[channel]);
            return tud_control_xfer(port, r, control_buffer, 1);
        }
        if (selector == AUDIO_FU_CTRL_VOLUME) {
            if (r->bRequest == AUDIO_CS_REQ_CUR && r->wLength == 2) {
                int16_t db = LOAD(volume[channel]);
                memcpy(control_buffer, &db, 2);
                return tud_control_xfer(port, r, control_buffer, 2);
            }
            if (input && r->bRequest == AUDIO_CS_REQ_RANGE) {
                const uint8_t range[] = {1, 0, 0, 0xce, 0, 0, 0, 1}; // -50..0 dB, 1 dB
                memcpy(control_buffer, range, sizeof(range));
                return tud_control_xfer(port, r, control_buffer, 8);
            }
        }
    }
    return false; // never ACK controls that are not implemented
}
static bool control(uint8_t port, uint8_t stage, const tusb_control_request_t *r) {
    bool ok = control_impl(port, stage, r);
    // A console write may block for tens of ms behind another task's log.
    // Control polling must never delay PCM preparation: counters only here.
    if (stage == CONTROL_STAGE_SETUP) {
        ADD(stats.control_requests, 1);
        if (!ok) ADD(stats.control_stalls, 1);
    }
    return ok;
}
static const usbd_class_driver_t driver = {
    .name = "p4_uac2_stream", .init = driver_init, .deinit = driver_deinit,
    .reset = driver_reset, .open = driver_open, .control_xfer_cb = control,
    .xfer_cb = transfer_isr, .xfer_isr = transfer_isr, .sof = sof_isr
};
const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *count) { *count = 1; return &driver; }
void tud_suspend_cb(bool remote_wakeup) {
    (void)remote_wakeup;
    suspended = true;
    STORE(cap_active, 0); STORE(play_active, 0);
    ADD(stats.usb_suspends, 1);
}
void tud_resume_cb(void) {
    suspended = false;
    stream_set(UAC_CAP_ITF, alternates[UAC_CAP_ITF]);
    stream_set(UAC_PLAY_ITF, alternates[UAC_PLAY_ITF]);
}
void tud_umount_cb(void) { driver_reset(UAC_PORT); }

static void capture_prepare(void) {
    if (!LOAD(cap_active)) { ADD(stats.capture_discard_frames, audio_ring_discard(&capture_ring)); return; }
    if (!source_ready()) return;
    uint32_t fill = audio_ring_fill(&capture_ring);
    if (!capture_started) {
        if (fill < LOAD(prefill_frames)) return;
        capture_started = true;
        eps[CAP].last_progress_tick = xTaskGetTickCount();
    }
    endpoint_t *e = &eps[CAP];
    if (!capture_start_trimmed && e->seen_completion) {
        // SET_INTERFACE may precede the first host IN request by tens of ms.
        // Keep only the latency budget when actual polling begins. Draining
        // that startup backlog via a sustained +1 frame/packet burst can
        // overflow Windows' minimum exclusive buffers (notably at 44.1k).
        // This one startup discard is task-owned and counted, never a steady
        // state sample-rate correction. Already queued DMA slots stay intact.
        usbd_spin_lock(false);
        uint32_t target = LOAD(prefill_frames) + nominal_frames();
        uint32_t keep = queued_frames < target ? target - queued_frames : 0;
        usbd_spin_unlock(false);
        uint32_t available = audio_ring_fill(&capture_ring);
        uint32_t discard = available > keep ? available - keep : 0;
        int32_t scratch[UAC_MAX_FRAMES * 2];
        while (discard) {
            unsigned count = discard > UAC_MAX_FRAMES ? UAC_MAX_FRAMES : discard;
            count = audio_ring_read(&capture_ring, scratch, count);
            ADD(stats.capture_discard_frames, count);
            if (!count) break;
            discard -= count;
        }
        packet_mode = 0;
        capture_start_trimmed = true;
    }
    // At most two packets ahead, one DMA slot, one spare; power-of-two indices
    // preserve slot order even when 32-bit producer/consumer counters wrap.
    for (unsigned count = 0; count < 2; ++count) {
        usbd_spin_lock(false);
        unsigned slot = e->produced % SLOT_COUNT;
        uint32_t occupancy = audio_ring_fill(&capture_ring) + queued_frames;
        if (e->slots[slot].state != FREE || e->produced - e->consumed >= 2) {
            usbd_spin_unlock(false); break;
        }
        uint32_t next_phase = packet_phase;
        unsigned frames = audio_next_packet(LOAD(runtime_rate), packet_hz(), &next_phase,
            occupancy, LOAD(prefill_frames) + nominal_frames(), &packet_mode);
        if (audio_ring_fill(&capture_ring) < frames) { usbd_spin_unlock(false); break; }
        e->slots[slot].state = BUILDING;
        usbd_spin_unlock(false);
        int32_t pcm[UAC_MAX_FRAMES * 2];
        audio_ring_read(&capture_ring, pcm, frames);
        audio_pack_pcm(capture_dma[slot], pcm, frames * 2, LOAD(cap_frame_bytes) / 2);
        usbd_spin_lock(false);
        e->slots[slot].bytes = frames * LOAD(cap_frame_bytes);
        e->slots[slot].frames = frames;
        packet_phase = next_phase;
        e->slots[slot].state = READY;
        queued_frames += frames;
        ++e->produced;
        if (!e->armed && !e->pending) {
            e->pending = true;
            e->due = p4_dwc2_frame_number();
            arm(CAP, e->due);
        }
        usbd_spin_unlock(false);
        limits(occupancy, &stats.capture_min_fill, &stats.capture_max_fill);
    }
}
static void playback_drain(void) {
    endpoint_t *e = &eps[PLAY];
    for (unsigned count = 0; count < SLOT_COUNT; ++count) {
        unsigned slot = e->consumed % SLOT_COUNT;
        usbd_spin_lock(false);
        bool ready = e->slots[slot].state == DONE;
        unsigned bytes = e->slots[slot].bytes;
        usbd_spin_unlock(false);
        if (!ready) break;
        unsigned frame_bytes = LOAD(play_frame_bytes);
        if (bytes > UAC_MAX_FRAMES * frame_bytes || bytes % frame_bytes) ADD(stats.playback_bad_packets, 1);
        else if (LOAD(play_active) && source_ready()) {
            unsigned frames = bytes / frame_bytes;
            int32_t pcm[UAC_MAX_FRAMES * 2];
            int32_t gains[2];
            for (unsigned c = 0; c < 2; ++c)
                gains[c] = (int32_t)(((int64_t)LOAD(gain_q24[0]) * LOAD(gain_q24[c + 1])) >> 24);
            for (unsigned f = 0; f < frames; ++f) {
                for (unsigned c = 0; c < 2; ++c) {
                    pcm[f * 2 + c] = (LOAD(muted[0]) || LOAD(muted[c + 1])) ? 0 :
                        audio_unpack_sample((uint8_t *)playback_dma[slot] + (f * 2 + c) * (frame_bytes / 2), frame_bytes / 2);
                    if (gains[c] != (1 << 24))
                        pcm[f * 2 + c] = (int32_t)(((int64_t)pcm[f * 2 + c] * gains[c]) >> 24) & ~255;
                }
            }
            // Never dirty an OUT DMA cache line: CPU transformations use separate SRAM.
            unsigned written = audio_ring_write(&playback_ring, pcm, frames);
            ADD(stats.playback_overrun_frames, frames - written);
            limits(audio_ring_fill(&playback_ring), &stats.playback_min_fill, &stats.playback_max_fill);
        }
        usbd_spin_lock(false);
        e->slots[slot].state = FREE;
        ++e->consumed;
        usbd_spin_unlock(false);
    }
}
static void feedback_update(uint32_t now_ms) {
    uint32_t frames = LOAD(source_frames);
    if (frames != last_source) { last_source = frames; last_source_ms = now_ms; }
    uint32_t ticks = LOAD(usb_ticks);
    uint32_t scale = tud_speed_get() == TUSB_SPEED_HIGH ? 8000 : 1000;
    if (ticks - clock_ticks >= scale) {
        uint32_t elapsed = ticks - clock_ticks;
        uint64_t measured = ((uint64_t)(frames - clock_frames) << 16) / elapsed;
        uint32_t nominal = ((uint64_t)LOAD(runtime_rate) << 16) / scale;
        // Reject start/pause windows. Ring feedback handles short scheduling jitter.
        if (measured > nominal * 99 / 100 && measured < nominal * 101 / 100)
            rate_q16 = (3 * rate_q16 + (uint32_t)measured) / 4;
        clock_ticks = ticks; clock_frames = frames;
    }
    if (now_ms == last_feedback_ms) return;
    last_feedback_ms = now_ms;
    int32_t fill = audio_ring_fill(&playback_ring);
    fill_q8 = (uint32_t)((int32_t)fill_q8 + ((fill * 256 - (int32_t)fill_q8) / 16));
    int32_t error = (int32_t)LOAD(prefill_frames) - (int32_t)(fill_q8 >> 8);
    int32_t correction = error * (scale == 8000 ? 8 : 64);
    int32_t limit = (int32_t)(rate_q16 / 500); // +/-2000 ppm, no sample slips
    if (correction > limit) correction = limit;
    if (correction < -limit) correction = -limit;
    // Windows usbaudio2 expects Q16.16 FRAMES PER OUT PACKET when bInterval>1,
    // not frames per microframe. Keep clock math in uframes; scale only on wire.
    // Compatibility behaviour also documented by Linux gadget u_audio.c,
    // u_audio_set_fback_frequency(). At HS bInterval=3 wire nominal is rate/2000.
    uint32_t value = (uint32_t)((int32_t)rate_q16 + correction) * interval();
    STORE(stats.feedback_16_16, value);
    usbd_spin_lock(false);
    for (unsigned i = 0; i < 2; ++i)
        if (!eps[FB].armed || eps[FB].current != i) feedback_dma[i][0] = value;
    usbd_spin_unlock(false);
}
static void watchdog(void) {
    if (!tud_mounted() || suspended) return;
    for (unsigned i = 0; i < 3; ++i) {
        endpoint_t *e = &eps[i];
        usbd_spin_lock(false);
        // Read time AFTER masking the USB ISR. If read before the lock, a
        // completion can advance last_progress_tick into the next tick while
        // 'now' is stale. Unsigned subtraction then wraps and falsely times out
        // a healthy endpoint. ISR and watchdog run on the same pinned core.
        uint32_t now = xTaskGetTickCount();
        bool stalled = e->enabled && (i != CAP || capture_started) &&
            now - e->last_progress_tick >= pdMS_TO_TICKS(8);
        usbd_spin_unlock(false);
        if (stalled) {
            ADD(stats.endpoint_recoveries, 1);
            // Fresh capture prefill after a host pause; no old queued tail replay.
            if (i == CAP) stream_set(UAC_CAP_ITF, alternates[UAC_CAP_ITF]);
            else if (i == PLAY) stream_set(UAC_PLAY_ITF, alternates[UAC_PLAY_ITF]);
            else endpoint_restart(FB, true);
        }
    }
}
static void format_update(void) {
    uint32_t rate = LOAD(requested_rate), epoch = LOAD(source_epoch), budget = LOAD(buffer_ms);
    if (rate == LOAD(runtime_rate) && epoch == LOAD(applied_source_epoch) && budget == applied_buffer_ms) return;
    uint8_t cap_alt = alternates[UAC_CAP_ITF], play_alt = alternates[UAC_PLAY_ITF];
    if (tud_mounted()) {
        stream_set(UAC_CAP_ITF, 0);
        stream_set(UAC_PLAY_ITF, 0);
    }
    STORE(runtime_rate, rate);
    STORE(prefill_frames, audio_prefill(rate, budget));
    applied_buffer_ms = budget;
    rate_q16 = ((uint64_t)rate << 16) / (tud_speed_get() == TUSB_SPEED_HIGH ? 8000 : 1000);
    feedback_dma[0][0] = feedback_dma[1][0] = rate_q16 * interval();
    fill_q8 = LOAD(prefill_frames) << 8;
    clock_ticks = LOAD(usb_ticks);
    clock_frames = LOAD(source_frames);
    STORE(applied_source_epoch, epoch);
    if (tud_mounted()) {
        stream_set(UAC_CAP_ITF, cap_alt);
        stream_set(UAC_PLAY_ITF, play_alt);
    }
}
static void usb_task(void *arg) {
    (void)arg;
    if (!audio_ring_self_test(&capture_ring) || !audio_format_self_test()) {
        STORE(initialised, 3); ESP_LOGE("p4_uac2", "SPSC self-test FAILED"); vTaskDelete(NULL); return;
    }
    ESP_LOGI("p4_uac2", "SPSC and multirate self-tests PASS; USB profile PCM24 only");
    usb_phy_config_t conf = { .controller = USB_PHY_CTRL_OTG, .otg_mode = USB_OTG_MODE_DEVICE,
                             .target = USB_PHY_TARGET_INT, .otg_speed = USB_PHY_SPEED_HIGH };
    if (usb_new_phy(&conf, &phy) != ESP_OK) { STORE(initialised, 3); vTaskDelete(NULL); return; }
    tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_HIGH };
    if (!tusb_init(UAC_PORT, &init)) { STORE(initialised, 3); vTaskDelete(NULL); return; }
    STORE(initialised, 2);
    uint32_t observed_sof_ticks = LOAD(usb_ticks);
    uint32_t last_sof_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t controller_started_ms = last_sof_ms;
    bool saw_sof = false;
    for (;;) {
        // Audio notifications wake this task immediately; USB control requests
        // have a bounded 1ms polling fallback and are handled in this same task.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        tud_task_ext(0, false);
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const uint32_t current_sof_ticks = LOAD(usb_ticks);
        if (current_sof_ticks != observed_sof_ticks) {
            observed_sof_ticks = current_sof_ticks;
            last_sof_ms = now_ms;
            saw_sof = true;
            STORE(host_alive, 1);
            STORE(host_sof_age_ms, 0);
        } else if (saw_sof && now_ms - last_sof_ms >= 250U) {
            // A self-powered P4 can keep stale TinyUSB connected/mounted bits
            // and even the suspended bit after D+/D- are physically removed.
            // SOF progress is the only usable host heartbeat because VBUS is
            // intentionally cut by the data-only adapter. A genuinely
            // suspended PC therefore hands ownership to LOCAL as well; fresh
            // SOF restores WINDOWS ownership immediately after resume.
            STORE(host_alive, 0);
            STORE(host_sof_age_ms, now_ms - last_sof_ms);
        } else if (saw_sof) {
            STORE(host_sof_age_ms, now_ms - last_sof_ms);
        }

        const bool controller_fault = p4_dwc2_faulted();
        const bool stale_attached_bus =
            ((saw_sof && now_ms - last_sof_ms >= 500U) ||
             (!saw_sof && now_ms - controller_started_ms >= 1500U)) &&
            (tud_connected() || tud_mounted());
        if (controller_fault || stale_attached_bus) {
            if (controller_fault) ADD(stats.controller_faults, 1);
            STORE(cap_active, 0); STORE(play_active, 0);
            STORE(host_alive, 0);
            STORE(host_sof_age_ms, UINT32_MAX);
            tud_disconnect();
            // Stop controller/interrupts before any DMA buffer can be recycled.
            tusb_deinit(UAC_PORT);
            vTaskDelay(pdMS_TO_TICKS(25));
            if (!tusb_init(UAC_PORT, &init)) { STORE(initialised, 3); vTaskDelete(NULL); return; }
            ADD(stats.controller_restarts, 1);
            observed_sof_ticks = LOAD(usb_ticks);
            last_sof_ms = controller_started_ms =
                (uint32_t)(esp_timer_get_time() / 1000);
            saw_sof = false;
            continue;
        }
        format_update();
        playback_drain();
        capture_prepare();
        feedback_update(now_ms);
        watchdog();
    }
}

esp_err_t p4_uac2_init(void) {
    if (LOAD(initialised)) return ESP_ERR_INVALID_STATE;
    if (!esp_ptr_internal(capture_dma) || !esp_ptr_dma_capable(capture_dma) ||
        !esp_ptr_internal(playback_dma) || ((uintptr_t)capture_dma & 63) || ((uintptr_t)playback_dma & 63))
        return ESP_ERR_INVALID_STATE;
    stats.capture_min_fill = stats.playback_min_fill = UINT32_MAX;
    feedback_dma[0][0] = feedback_dma[1][0] = 48u << 16;
    STORE(initialised, 1);
    if (xTaskCreatePinnedToCore(usb_task, "p4_uac2", 6144, NULL, 12, &usb_task_handle, 0) != pdPASS) {
        STORE(initialised, 0); return ESP_ERR_NO_MEM;
    }
    return ESP_OK; // get_stats().initialization_state reports asynchronous startup
}
bool p4_uac2_capture_active(void) { return LOAD(cap_active) != 0; }
bool p4_uac2_playback_active(void) { return LOAD(play_active) != 0; }
uint32_t p4_uac2_requested_rate(void) { return LOAD(requested_rate); }
esp_err_t p4_uac2_set_local_rate(uint32_t sample_rate) {
    if (!audio_rate_supported(sample_rate)) return ESP_ERR_INVALID_ARG;
    if (LOAD(cap_active) || LOAD(play_active)) return ESP_ERR_INVALID_STATE;
    // Once a host has started enumeration it owns the shared UAC2 clock. This
    // also closes the short window before tud_mounted() becomes true.
    if (LOAD(initialised) >= 2 && LOAD(host_alive)) return ESP_ERR_INVALID_STATE;
    if (sample_rate != LOAD(requested_rate)) {
        STORE(requested_rate, sample_rate);
        STORE(rate_conflict, 0);
        ADD(stats.rate_changes, 1);
        if (usb_task_handle) xTaskNotifyGive(usb_task_handle);
    }
    return ESP_OK;
}
void p4_uac2_source_rate(uint32_t actual_rate, bool new_session) {
    if (!audio_rate_supported(actual_rate)) return;
    bool changed = actual_rate != LOAD(confirmed_rate);
    STORE(confirmed_rate, actual_rate);
    if (new_session || changed) ADD(source_epoch, 1);
    if (new_session) ADD(stats.source_restarts, 1);
}
esp_err_t p4_uac2_set_buffer_ms(uint32_t milliseconds) {
    if (milliseconds != 1 && milliseconds != 2 && milliseconds != 4) return ESP_ERR_INVALID_ARG;
    if (LOAD(cap_active) || LOAD(play_active)) return ESP_ERR_INVALID_STATE;
    STORE(buffer_ms, milliseconds);
    if (usb_task_handle) xTaskNotifyGive(usb_task_handle);
    return ESP_OK;
}
size_t p4_uac2_capture_write(const int32_t *pcm, size_t frames) {
    if (!pcm || !frames || frames > P4_UAC2_RING_FRAMES) return 0;
    ADD(source_frames, frames);
    if (!LOAD(cap_active) || !source_ready()) return 0;
    size_t written = audio_ring_write(&capture_ring, pcm, frames);
    ADD(stats.capture_overrun_frames, frames - written);
    if (written && usb_task_handle) xTaskNotifyGive(usb_task_handle);
    return written;
}
size_t p4_uac2_playback_read(int32_t *pcm, size_t frames) {
    static uint32_t epoch;
    static bool started;
    if (!pcm || !frames || frames > P4_UAC2_RING_FRAMES) return 0;
    memset(pcm, 0, frames * 8);
    uint32_t current_epoch = LOAD(play_epoch);
    if (epoch != current_epoch || !LOAD(play_active) || !source_ready()) {
        audio_ring_discard(&playback_ring);
        started = false;
        epoch = current_epoch;
        return 0;
    }
    uint32_t fill = audio_ring_fill(&playback_ring);
    if (!started) {
        if (fill < LOAD(prefill_frames)) return 0;
        started = true;
    }
    if (fill < frames) {
        ADD(stats.playback_underruns, 1);
        started = false;
        return 0;
    }
    size_t read = audio_ring_read(&playback_ring, pcm, frames);
    if (LOAD(play_epoch) != epoch || !LOAD(play_active)) {
        memset(pcm, 0, frames * 8); started = false; return 0;
    }
    ADD(stats.playback_consumed_frames, read);
    return read;
}
void p4_uac2_get_stats(p4_uac2_stats_t *out) {
    if (!out) return;
    // Individual naturally aligned counters, never a 64-bit ISR atomic or lock.
    uint32_t *dst = (uint32_t *)out;
    const uint32_t *src = (const uint32_t *)&stats;
    for (unsigned i = 0; i < offsetof(p4_uac2_stats_t, connected) / 4; ++i) dst[i] = LOAD(src[i]);
    out->initialization_state = LOAD(initialised);
    out->sample_rate = LOAD(runtime_rate);
    out->source_sample_rate = LOAD(confirmed_rate);
    out->capture_sample_rate = LOAD(capture_selected_rate);
    out->playback_sample_rate = LOAD(playback_selected_rate);
    out->capture_bits = UAC_VALID_BITS;
    out->playback_bits = UAC_VALID_BITS;
    out->prefill_frames = LOAD(prefill_frames);
    out->buffer_ms = LOAD(buffer_ms);
    out->capture_source_frames = LOAD(source_frames);
    out->capture_fill = audio_ring_fill(&capture_ring);
    out->playback_fill = audio_ring_fill(&playback_ring);
    out->capture_queued_frames = LOAD(queued_frames);
    out->usb_sof_age_ms = LOAD(host_sof_age_ms);
    out->connected = tud_connected();
    out->mounted = tud_mounted();
    out->suspended = tud_suspended();
    out->high_speed = tud_speed_get() == TUSB_SPEED_HIGH;
    out->host_alive = LOAD(host_alive) != 0;
    out->capture_active = p4_uac2_capture_active();
    out->playback_active = p4_uac2_playback_active();
    out->rate_conflict = LOAD(rate_conflict) != 0;
    if (out->capture_min_fill == UINT32_MAX) out->capture_min_fill = 0;
    if (out->playback_min_fill == UINT32_MAX) out->playback_min_fill = 0;
}
