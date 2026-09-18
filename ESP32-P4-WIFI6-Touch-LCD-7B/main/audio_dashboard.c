#include "audio_dashboard.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

enum {
    DASHBOARD_WIDTH = 1024,
    DASHBOARD_HEIGHT = 600,
    DASHBOARD_REFRESH_MS = 33,
    DASHBOARD_CHANNELS = 4,
};

typedef struct {
    lv_obj_t *bar;
    lv_obj_t *value;
    float displayed_percent;
    int displayed_tenths_db;
    char value_text[16];
} meter_view_t;

static const char *TAG = "audio_dashboard";

static meter_view_t s_meters[DASHBOARD_CHANNELS];
static lv_obj_t *s_cpu_bar;
static lv_obj_t *s_cpu_value;
static lv_obj_t *s_capture_rate;
static lv_obj_t *s_playback_rate;
static lv_obj_t *s_capture_status;
static lv_obj_t *s_playback_status;
static lv_obj_t *s_capture_status_dot;
static lv_obj_t *s_playback_status_dot;
static lv_obj_t *s_footer;

static volatile uint32_t s_pending_peak[DASHBOARD_CHANNELS];
static volatile uint32_t s_seed_cpu_percent;
static volatile uint32_t s_capture_rate_hz = 48000U;
static volatile uint32_t s_playback_rate_hz = 48000U;
static volatile uint32_t s_buffer_ms = 1U;
static volatile uint32_t s_status_flags;
static volatile uint32_t s_spi_errors;
static volatile uint32_t s_crc_errors;

static char s_cpu_text[8] = "0%";
static char s_capture_rate_text[12] = "48";
static char s_playback_rate_text[12] = "48";
static char s_capture_status_text[24] = "MIC: WAITING";
static char s_playback_status_text[24] = "OUTPUT: WAITING";
static char s_footer_text[128] =
    "SEED3 CLOCK LOCKED  |  USB AUDIO 2.0  |  2 IN / 2 OUT  |  BUFFER 1 ms";

enum {
    STATUS_USB_MOUNTED = 1U << 0,
    STATUS_CAPTURE_ACTIVE = 1U << 1,
    STATUS_PLAYBACK_ACTIVE = 1U << 2,
};

static lv_color_t color(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static void set_no_scroll(lv_obj_t *object)
{
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
}

static lv_obj_t *make_panel(lv_obj_t *parent,
                            int x,
                            int y,
                            int width,
                            int height,
                            uint32_t background,
                            uint32_t border,
                            int radius)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_bg_color(panel, color(background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, color(border), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, border == background ? 0 : 1,
                                  LV_PART_MAIN);
    lv_obj_set_style_radius(panel, radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    set_no_scroll(panel);
    return panel;
}

static lv_obj_t *make_label(lv_obj_t *parent,
                            int x,
                            int y,
                            int width,
                            int height,
                            const char *text,
                            const lv_font_t *font,
                            uint32_t text_color,
                            lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color(text_color), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    set_no_scroll(label);
    return label;
}

static void style_card(lv_obj_t *card)
{
    lv_obj_set_style_shadow_color(card, color(0x000000), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(card, 70, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_y(card, 7, LV_PART_MAIN);
}

static lv_obj_t *make_chip(lv_obj_t *screen,
                           int x,
                           int width,
                           const char *text)
{
    lv_obj_t *chip = make_panel(screen, x, 22, width, 42, 0x101f31,
                                0x20364d, 12);
    lv_obj_t *label = make_label(chip, 0, 0, width, 42, text,
                                 &lv_font_montserrat_14, 0xd7e2ef,
                                 LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_align(label, LV_ALIGN_CENTER, LV_PART_MAIN);
    return chip;
}

static void style_meter_bar(lv_obj_t *bar, bool input)
{
    const uint32_t main_color = 0x17283a;
    const uint32_t indicator_low = input ? 0x21b991 : 0x496bd7;
    const uint32_t indicator_high = input ? 0xd9ff7a : 0xb2c3ff;

    lv_obj_set_style_bg_color(bar, color(main_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, color(0x294158), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 21, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 5, LV_PART_MAIN);

    lv_obj_set_style_bg_color(bar, color(indicator_low), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(bar, color(indicator_high),
                                   LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 16, LV_PART_INDICATOR);
}

static void add_tick(lv_obj_t *card, int y)
{
    lv_obj_t *tick = make_panel(card, 45, y, 8, 1, 0x3c526a, 0x3c526a, 0);
    lv_obj_set_style_opa(tick, 150, LV_PART_MAIN);
}

static void create_meter(lv_obj_t *screen,
                         unsigned index,
                         int x,
                         const char *name,
                         bool input)
{
    lv_obj_t *card = make_panel(screen, x, 120, 150, 404, 0x0d1928,
                                0x1a2c42, 20);
    style_card(card);
    make_label(card, 0, 18, 150, 28, name, &lv_font_montserrat_20,
               0xeef4fb, LV_TEXT_ALIGN_CENTER);

    meter_view_t *meter = &s_meters[index];
    strcpy(meter->value_text, "-60.0 dB");
    meter->value = make_label(card, 0, 55, 150, 24, meter->value_text,
                              &lv_font_montserrat_14, 0x92a4b8,
                              LV_TEXT_ALIGN_CENTER);
    meter->displayed_tenths_db = -600;

    meter->bar = lv_bar_create(card);
    lv_obj_set_pos(meter->bar, 54, 92);
    lv_obj_set_size(meter->bar, 42, 236);
    lv_bar_set_range(meter->bar, 0, 1000);
    lv_bar_set_value(meter->bar, 0, LV_ANIM_OFF);
    style_meter_bar(meter->bar, input);
    set_no_scroll(meter->bar);

    add_tick(card, 115);
    add_tick(card, 171);
    add_tick(card, 234);
    add_tick(card, 299);
    make_label(card, 0, 350, 150, 22, "PEAK  -60.0",
               &lv_font_montserrat_12, 0x667d95, LV_TEXT_ALIGN_CENTER);
}

static void create_dashboard(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, DASHBOARD_WIDTH, DASHBOARD_HEIGHT);
    lv_obj_set_style_bg_color(screen, color(0x07111f), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    set_no_scroll(screen);

    make_panel(screen, 0, 0, DASHBOARD_WIDTH, 5, 0x42e8bd, 0x42e8bd, 0);
    make_label(screen, 32, 20, 360, 36, "SEED3 / P4 AUDIO",
               &lv_font_montserrat_28, 0xf4f8ff, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *subtitle = make_label(
        screen, 34, 57, 340, 18, "LOW-LATENCY USB AUDIO MONITOR",
        &lv_font_montserrat_12, 0x7589a3, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(subtitle, 2, LV_PART_MAIN);

    make_chip(screen, 420, 112, "44.1-96 kHz");
    make_chip(screen, 544, 96, "24 bit");
    make_chip(screen, 652, 110, "USB HS");

    lv_obj_t *cpu = make_panel(screen, 786, 14, 210, 60, 0x0e1c2b,
                               0x28405a, 15);
    style_card(cpu);
    lv_obj_t *cpu_caption = make_label(cpu, 16, 9, 126, 18, "SEED3 CPU",
                                       &lv_font_montserrat_12, 0x7f93aa,
                                       LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(cpu_caption, 1, LV_PART_MAIN);
    s_cpu_bar = lv_bar_create(cpu);
    lv_obj_set_pos(s_cpu_bar, 16, 35);
    lv_obj_set_size(s_cpu_bar, 125, 10);
    lv_bar_set_range(s_cpu_bar, 0, 100);
    lv_bar_set_value(s_cpu_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_cpu_bar, color(0x203145), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_cpu_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_cpu_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_cpu_bar, color(0x42e8bd), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_cpu_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_cpu_bar, 5, LV_PART_INDICATOR);
    s_cpu_value = make_label(cpu, 150, 13, 48, 32, s_cpu_text,
                             &lv_font_montserrat_20, 0x42e8bd,
                             LV_TEXT_ALIGN_RIGHT);

    lv_obj_t *side = make_panel(screen, 28, 104, 210, 420, 0x0c1827,
                                0x1b3047, 20);
    style_card(side);
    lv_obj_t *monitor = make_label(side, 20, 20, 170, 22, "LIVE MONITOR",
                                   &lv_font_montserrat_14, 0xa8bbcf,
                                   LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(monitor, 2, LV_PART_MAIN);
    make_label(side, 20, 54, 170, 18, "INPUT / MICROPHONE",
               &lv_font_montserrat_12, 0x42e8bd, LV_TEXT_ALIGN_LEFT);
    s_capture_rate = make_label(side, 20, 74, 94, 40,
                                s_capture_rate_text,
                                &lv_font_montserrat_28, 0xf5f8fc,
                                LV_TEXT_ALIGN_LEFT);
    make_label(side, 116, 86, 54, 22, "kHz", &lv_font_montserrat_14,
               0x42e8bd, LV_TEXT_ALIGN_LEFT);
    s_capture_status_dot = make_panel(side, 20, 121, 10, 10,
                                      0x627a94, 0x627a94, 5);
    s_capture_status = make_label(side, 40, 115, 150, 22,
                                  s_capture_status_text,
                                  &lv_font_montserrat_12, 0x627a94,
                                  LV_TEXT_ALIGN_LEFT);

    make_panel(side, 20, 151, 170, 1, 0x1a2e43, 0x1a2e43, 0);
    make_label(side, 20, 168, 170, 18, "OUTPUT / PLAYBACK",
               &lv_font_montserrat_12, 0x8ba9ff, LV_TEXT_ALIGN_LEFT);
    s_playback_rate = make_label(side, 20, 188, 94, 40,
                                 s_playback_rate_text,
                                 &lv_font_montserrat_28, 0xf5f8fc,
                                 LV_TEXT_ALIGN_LEFT);
    make_label(side, 116, 200, 54, 22, "kHz", &lv_font_montserrat_14,
               0x8ba9ff, LV_TEXT_ALIGN_LEFT);
    s_playback_status_dot = make_panel(side, 20, 235, 10, 10,
                                       0x627a94, 0x627a94, 5);
    s_playback_status = make_label(side, 40, 229, 150, 22,
                                   s_playback_status_text,
                                   &lv_font_montserrat_12, 0x627a94,
                                   LV_TEXT_ALIGN_LEFT);
    lv_obj_t *info = make_label(
        side, 20, 270, 170, 92,
        "FORMAT  PCM24\nUSB  HIGH SPEED / UAC2\nCLOCK  SEED3 MASTER",
        &lv_font_montserrat_12, 0xb5c4d5, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(info, 5, LV_PART_MAIN);
    make_panel(side, 20, 379, 170, 1, 0x1a2e43, 0x1a2e43, 0);
    make_label(side, 20, 390, 170, 18, "SPI 20 MHz  |  CRC 0",
               &lv_font_montserrat_12, 0x627a94, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *input_group = make_label(screen, 270, 93, 300, 22,
        "INPUT  |  SEED3 ADC", &lv_font_montserrat_12, 0x42e8bd,
        LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(input_group, 2, LV_PART_MAIN);
    lv_obj_t *output_group = make_label(screen, 626, 93, 330, 22,
        "OUTPUT  |  USB PLAYBACK", &lv_font_montserrat_12, 0x8ba9ff,
        LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(output_group, 2, LV_PART_MAIN);

    create_meter(screen, 0, 264, "IN 1", true);
    create_meter(screen, 1, 424, "IN 2", true);
    create_meter(screen, 2, 620, "OUT 1", false);
    create_meter(screen, 3, 780, "OUT 2", false);

    make_panel(screen, 32, 544, 960, 1, 0x14283d, 0x14283d, 0);
    s_footer = make_label(screen, 32, 558, 960, 22, s_footer_text,
                          &lv_font_montserrat_12, 0x647a92,
                          LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(s_footer, 1, LV_PART_MAIN);

    lv_screen_load(screen);
}

static void atomic_max_u32(volatile uint32_t *target, uint32_t value)
{
    uint32_t current = __atomic_load_n(target, __ATOMIC_RELAXED);
    while (value > current &&
           !__atomic_compare_exchange_n(target, &current, value, false,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
    }
}

static float peak_to_db(uint32_t peak)
{
    if (peak < 256U) {
        return -60.0f;
    }
    float normalized = (float)peak * (1.0f / 2147483392.0f);
    float db = 20.0f * log10f(normalized);
    if (db < -60.0f) db = -60.0f;
    if (db > 0.0f) db = 0.0f;
    return db;
}

static void update_meter(unsigned index, uint32_t peak)
{
    meter_view_t *meter = &s_meters[index];
    const float db = peak_to_db(peak);
    const float target = (db + 60.0f) * (100.0f / 60.0f);
    if (target >= meter->displayed_percent) {
        meter->displayed_percent = target;
    } else {
        meter->displayed_percent -= 2.4f;
        if (meter->displayed_percent < target) {
            meter->displayed_percent = target;
        }
        if (meter->displayed_percent < 0.0f) {
            meter->displayed_percent = 0.0f;
        }
    }
    lv_bar_set_value(meter->bar,
                     (int32_t)(meter->displayed_percent * 10.0f),
                     LV_ANIM_OFF);

    const int tenths = (int)lrintf(db * 10.0f);
    if (tenths != meter->displayed_tenths_db) {
        meter->displayed_tenths_db = tenths;
        snprintf(meter->value_text, sizeof(meter->value_text), "%d.%d dB",
                 tenths / 10, abs(tenths % 10));
        lv_label_set_text_static(meter->value, meter->value_text);
    }
}

static void update_status_text(void)
{
    static uint32_t shown_cpu = UINT32_MAX;
    static uint32_t shown_capture_rate = UINT32_MAX;
    static uint32_t shown_playback_rate = UINT32_MAX;
    static uint32_t shown_buffer = UINT32_MAX;
    static uint32_t shown_flags = UINT32_MAX;
    static uint32_t shown_spi_errors = UINT32_MAX;
    static uint32_t shown_crc_errors = UINT32_MAX;

    const uint32_t cpu = __atomic_load_n(&s_seed_cpu_percent, __ATOMIC_ACQUIRE);
    const uint32_t capture_rate =
        __atomic_load_n(&s_capture_rate_hz, __ATOMIC_ACQUIRE);
    const uint32_t playback_rate =
        __atomic_load_n(&s_playback_rate_hz, __ATOMIC_ACQUIRE);
    const uint32_t buffer = __atomic_load_n(&s_buffer_ms, __ATOMIC_ACQUIRE);
    const uint32_t flags = __atomic_load_n(&s_status_flags, __ATOMIC_ACQUIRE);
    const uint32_t spi_errors = __atomic_load_n(&s_spi_errors, __ATOMIC_ACQUIRE);
    const uint32_t crc_errors = __atomic_load_n(&s_crc_errors, __ATOMIC_ACQUIRE);

    if (cpu != shown_cpu) {
        shown_cpu = cpu;
        snprintf(s_cpu_text, sizeof(s_cpu_text), "%lu%%", (unsigned long)cpu);
        lv_label_set_text_static(s_cpu_value, s_cpu_text);
        lv_bar_set_value(s_cpu_bar, (int32_t)cpu, LV_ANIM_ON);
    }
    if (capture_rate != shown_capture_rate) {
        shown_capture_rate = capture_rate;
        if (capture_rate % 1000U == 0U) {
            snprintf(s_capture_rate_text, sizeof(s_capture_rate_text), "%lu",
                     (unsigned long)(capture_rate / 1000U));
        } else {
            snprintf(s_capture_rate_text, sizeof(s_capture_rate_text),
                     "%lu.%lu", (unsigned long)(capture_rate / 1000U),
                     (unsigned long)((capture_rate % 1000U) / 100U));
        }
        lv_label_set_text_static(s_capture_rate, s_capture_rate_text);
    }
    if (playback_rate != shown_playback_rate) {
        shown_playback_rate = playback_rate;
        if (playback_rate % 1000U == 0U) {
            snprintf(s_playback_rate_text, sizeof(s_playback_rate_text),
                     "%lu", (unsigned long)(playback_rate / 1000U));
        } else {
            snprintf(s_playback_rate_text, sizeof(s_playback_rate_text),
                     "%lu.%lu", (unsigned long)(playback_rate / 1000U),
                     (unsigned long)((playback_rate % 1000U) / 100U));
        }
        lv_label_set_text_static(s_playback_rate, s_playback_rate_text);
    }
    if (flags != shown_flags) {
        shown_flags = flags;
        const bool mounted = (flags & STATUS_USB_MOUNTED) != 0U;
        const bool capture_active = (flags & STATUS_CAPTURE_ACTIVE) != 0U;
        const bool playback_active = (flags & STATUS_PLAYBACK_ACTIVE) != 0U;
        snprintf(s_capture_status_text, sizeof(s_capture_status_text), "%s",
                 capture_active ? "MIC: ACTIVE" :
                 mounted ? "MIC: READY" : "MIC: WAITING");
        snprintf(s_playback_status_text, sizeof(s_playback_status_text), "%s",
                 playback_active ? "OUTPUT: ACTIVE" :
                 mounted ? "OUTPUT: READY" : "OUTPUT: WAITING");
        lv_label_set_text_static(s_capture_status, s_capture_status_text);
        lv_label_set_text_static(s_playback_status, s_playback_status_text);
        const uint32_t capture_color = capture_active ? 0x42e8bd :
                                           mounted ? 0xffca68 : 0x627a94;
        const uint32_t playback_color = playback_active ? 0x8ba9ff :
                                            mounted ? 0xffca68 : 0x627a94;
        lv_obj_set_style_text_color(s_capture_status, color(capture_color),
                                    LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_capture_status_dot, color(capture_color),
                                  LV_PART_MAIN);
        lv_obj_set_style_text_color(s_playback_status, color(playback_color),
                                    LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_playback_status_dot, color(playback_color),
                                  LV_PART_MAIN);
    }
    if (buffer != shown_buffer || flags != shown_flags ||
        spi_errors != shown_spi_errors || crc_errors != shown_crc_errors) {
        shown_buffer = buffer;
        shown_spi_errors = spi_errors;
        shown_crc_errors = crc_errors;
        snprintf(s_footer_text, sizeof(s_footer_text),
                 "SEED3 CLOCK LOCKED  |  USB AUDIO 2.0  |  2 IN / 2 OUT  |  BUFFER %lu ms  |  ERR %lu/%lu",
                 (unsigned long)buffer, (unsigned long)spi_errors,
                 (unsigned long)crc_errors);
        lv_label_set_text_static(s_footer, s_footer_text);
    }
}

static void dashboard_task(void *argument)
{
    (void)argument;
    while (true) {
        uint32_t peaks[DASHBOARD_CHANNELS];
        for (unsigned i = 0; i < DASHBOARD_CHANNELS; ++i) {
            peaks[i] = __atomic_exchange_n(&s_pending_peak[i], 0U,
                                           __ATOMIC_ACQ_REL);
        }
        if (bsp_display_lock(pdMS_TO_TICKS(20))) {
            for (unsigned i = 0; i < DASHBOARD_CHANNELS; ++i) {
                update_meter(i, peaks[i]);
            }
            update_status_text();
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(DASHBOARD_REFRESH_MS));
    }
}

esp_err_t audio_dashboard_init(void)
{
    bsp_display_cfg_t config = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };
    lv_display_t *display = bsp_display_start_with_config(&config);
    if (display == NULL) {
        return ESP_FAIL;
    }
    bsp_display_backlight_on();
    if (!bsp_display_lock(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    create_dashboard();
    bsp_display_unlock();

    const BaseType_t created = xTaskCreatePinnedToCore(
        dashboard_task, "audio_ui", 6144, NULL, 3, NULL, 0);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "1024x600 LVGL audio dashboard started");
    return ESP_OK;
}

void audio_dashboard_submit_peaks(uint32_t input_left,
                                  uint32_t input_right,
                                  uint32_t output_left,
                                  uint32_t output_right)
{
    atomic_max_u32(&s_pending_peak[0], input_left);
    atomic_max_u32(&s_pending_peak[1], input_right);
    atomic_max_u32(&s_pending_peak[2], output_left);
    atomic_max_u32(&s_pending_peak[3], output_right);
}

void audio_dashboard_set_seed_cpu(uint8_t percent)
{
    __atomic_store_n(&s_seed_cpu_percent,
                     percent <= 100U ? percent : 100U,
                     __ATOMIC_RELEASE);
}

void audio_dashboard_publish_status(const audio_dashboard_status_t *status)
{
    if (status == NULL) {
        return;
    }
    uint32_t flags = 0U;
    if (status->usb_mounted) flags |= STATUS_USB_MOUNTED;
    if (status->capture_active) flags |= STATUS_CAPTURE_ACTIVE;
    if (status->playback_active) flags |= STATUS_PLAYBACK_ACTIVE;
    __atomic_store_n(&s_capture_rate_hz, status->capture_sample_rate,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_playback_rate_hz, status->playback_sample_rate,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_buffer_ms, status->buffer_ms, __ATOMIC_RELEASE);
    __atomic_store_n(&s_status_flags, flags, __ATOMIC_RELEASE);
    __atomic_store_n(&s_spi_errors, (uint32_t)status->spi_errors,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_crc_errors, (uint32_t)status->crc_errors,
                     __ATOMIC_RELEASE);
}
