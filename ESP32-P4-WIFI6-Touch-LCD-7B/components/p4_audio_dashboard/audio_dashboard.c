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
    PEDALBOARD_REFRESH_MS = 200,
    DASHBOARD_CHANNELS = 4,
    TOP_GESTURE_START_Y = 80,
    WINDOW_SWIPE_DISTANCE = 70,
};

typedef enum {
    DASHBOARD_PAGE_MONITOR = 0,
    DASHBOARD_PAGE_PEDALBOARD = 1,
    DASHBOARD_PAGE_SETTINGS = 2,
    DASHBOARD_PAGE_COUNT,
} dashboard_page_t;

typedef struct {
    lv_obj_t *bar;
    lv_obj_t *value;
    float displayed_percent;
    int displayed_tenths_db;
    char value_text[16];
} meter_view_t;

typedef enum {
    PEDAL_IO_INPUT = 0,
    PEDAL_IO_OUTPUT = 1,
    PEDAL_IO_COUNT,
} pedal_io_kind_t;

typedef struct {
    const char *title;
    const char *settings_title;
    uint32_t accent;
    bool channel_enabled[2];
    lv_obj_t *card;
    lv_obj_t *status_bar;
    lv_obj_t *status_dot;
    lv_obj_t *rate;
    lv_obj_t *channel_dot[2];
    lv_obj_t *channel_text[2];
} pedal_io_view_t;

static const char *TAG = "audio_dashboard";

static meter_view_t s_meters[DASHBOARD_CHANNELS];
static pedal_io_view_t s_pedal_io[PEDAL_IO_COUNT] = {
    [PEDAL_IO_INPUT] = {
        .title = "IN",
        .settings_title = "INPUT SETTINGS",
        .accent = 0x42e8bd,
        .channel_enabled = {true, true},
    },
    [PEDAL_IO_OUTPUT] = {
        .title = "OUT",
        .settings_title = "OUTPUT SETTINGS",
        .accent = 0x8ba9ff,
        .channel_enabled = {true, true},
    },
};
static const char *const s_pedal_rate_names[] = {"44.1", "48", "88.2", "96"};
static const uint32_t s_pedal_rates[] = {44100U, 48000U, 88200U, 96000U};
static uint8_t s_pedal_rate_index = 1;
static lv_obj_t *s_monitor_page;
static lv_obj_t *s_pedalboard_page;
static lv_obj_t *s_settings_page;
static lv_obj_t *s_window_overlay;
static lv_obj_t *s_window_sheet;
static lv_obj_t *s_window_buttons[DASHBOARD_PAGE_COUNT];
static lv_obj_t *s_cpu_bar;
static lv_obj_t *s_cpu_value;
static lv_obj_t *s_capture_rate;
static lv_obj_t *s_playback_rate;
static lv_obj_t *s_capture_status;
static lv_obj_t *s_playback_status;
static lv_obj_t *s_capture_status_dot;
static lv_obj_t *s_playback_status_dot;
static lv_obj_t *s_footer;
static lv_obj_t *s_touch_indicator;
static lv_obj_t *s_pedal_cpu_bar;
static lv_obj_t *s_pedal_cpu_value;
static lv_obj_t *s_pedalboard_switch;
static lv_obj_t *s_pedalboard_mode;
static lv_obj_t *s_pedalboard_control_panel;
static lv_obj_t *s_auto_switch_views;
static lv_obj_t *s_transport_buttons[2];
static lv_obj_t *s_transport_active;
static lv_obj_t *s_io_settings_overlay;
static lv_obj_t *s_io_settings_title;
static lv_obj_t *s_io_settings_rate;
static lv_obj_t *s_io_settings_owner;
static lv_obj_t *s_io_settings_accent;
static lv_obj_t *s_io_rate_buttons[4];
static lv_obj_t *s_io_channel_switches[2];
static pedal_io_view_t *s_io_settings_target;

static volatile uint32_t s_pending_peak[DASHBOARD_CHANNELS];
static volatile uint32_t s_seed_cpu_percent;
static volatile uint32_t s_sample_rate_hz;
static volatile uint32_t s_buffer_ms = 1U;
static volatile uint32_t s_status_flags;
static volatile uint32_t s_spi_errors;
static volatile uint32_t s_crc_errors;
static volatile uint32_t s_active_page = DASHBOARD_PAGE_PEDALBOARD;
static volatile uint32_t s_command_flags;
static volatile uint32_t s_command_rate = 48000U;
static volatile uint32_t s_command_pedalboard = 1U;
static volatile uint32_t s_capture_channel_mask = 3U;
static volatile uint32_t s_playback_channel_mask = 3U;
static volatile uint32_t s_command_capture_mask = 3U;
static volatile uint32_t s_command_playback_mask = 3U;
static volatile uint32_t s_command_auto_switch_views = 1U;
static volatile uint32_t s_transport_profile =
    AUDIO_DASHBOARD_TRANSPORT_BALANCED;
static volatile uint32_t s_transport_mode =
    AUDIO_DASHBOARD_TRANSPORT_LOCAL;
static volatile uint32_t s_command_transport_profile =
    AUDIO_DASHBOARD_TRANSPORT_BALANCED;
static lv_point_t s_touch_start;
static bool s_touch_tracking;
static bool s_touch_was_pressed;
static bool s_touch_gesture_consumed;
static bool s_updating_pedalboard_switch;
static bool s_updating_auto_switch;
static bool s_auto_page_state_known;
static bool s_last_host_connected;
static bool s_last_auto_switch;

static char s_cpu_text[8] = "0%";
static char s_sample_rate_text[12] = "--";
static char s_capture_status_text[24] = "MIC: WAITING";
static char s_playback_status_text[24] = "OUTPUT: WAITING";
static char s_pedalboard_mode_text[32] = "LOCAL  |  FORCED ON";
static char s_io_owner_text[56] = "SHARED RATE  |  LOCAL CONTROL";
static char s_footer_text[128] =
    "SEED3 CLOCK LOCKED  |  USB AUDIO 2.0  |  2 IN / 2 OUT  |  BUFFER 1 ms";

enum {
    STATUS_USB_MOUNTED = 1U << 0,
    STATUS_CAPTURE_ACTIVE = 1U << 1,
    STATUS_PLAYBACK_ACTIVE = 1U << 2,
    STATUS_USB_CONNECTED = 1U << 3,
    STATUS_USB_SUSPENDED = 1U << 4,
    STATUS_PC_RATE_OWNER = 1U << 5,
    STATUS_PEDALBOARD_ENABLED = 1U << 6,
    STATUS_PEDALBOARD_FORCED = 1U << 7,
    STATUS_PEDALBOARD_SYNCING = 1U << 8,
    STATUS_RATE_SYNCING = 1U << 9,
    STATUS_RATE_CONFLICT = 1U << 10,
    STATUS_RATE_VALID = 1U << 11,
    STATUS_AUTO_SWITCH_VIEWS = 1U << 12,
};

enum {
    COMMAND_SET_RATE = 1U << 0,
    COMMAND_SET_PEDALBOARD = 1U << 1,
    COMMAND_SET_CHANNEL_MASKS = 1U << 2,
    COMMAND_SET_AUTO_SWITCH_VIEWS = 1U << 3,
    COMMAND_SET_TRANSPORT_PROFILE = 1U << 4,
};

static void close_io_settings(void);

static lv_color_t color(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static uint8_t rate_index_from_hz(uint32_t sample_rate)
{
    for (uint8_t index = 0; index < 4; ++index) {
        if (s_pedal_rates[index] == sample_rate) return index;
    }
    return 1;
}

static void request_sample_rate(uint32_t sample_rate)
{
    __atomic_store_n(&s_command_rate, sample_rate, __ATOMIC_RELEASE);
    __atomic_fetch_or(&s_command_flags, COMMAND_SET_RATE, __ATOMIC_RELEASE);
}

static void request_pedalboard_enabled(bool enabled)
{
    __atomic_store_n(&s_command_pedalboard, enabled ? 1U : 0U,
                     __ATOMIC_RELEASE);
    __atomic_fetch_or(&s_command_flags, COMMAND_SET_PEDALBOARD,
                      __ATOMIC_RELEASE);
}

static void request_channel_masks(void)
{
    uint32_t capture_mask = 0U;
    uint32_t playback_mask = 0U;
    for (unsigned channel = 0; channel < 2; ++channel) {
        if (s_pedal_io[PEDAL_IO_INPUT].channel_enabled[channel]) {
            capture_mask |= 1U << channel;
        }
        if (s_pedal_io[PEDAL_IO_OUTPUT].channel_enabled[channel]) {
            playback_mask |= 1U << channel;
        }
    }
    __atomic_store_n(&s_capture_channel_mask, capture_mask, __ATOMIC_RELEASE);
    __atomic_store_n(&s_playback_channel_mask, playback_mask, __ATOMIC_RELEASE);
    __atomic_store_n(&s_command_capture_mask, capture_mask, __ATOMIC_RELEASE);
    __atomic_store_n(&s_command_playback_mask, playback_mask, __ATOMIC_RELEASE);
    __atomic_fetch_or(&s_command_flags, COMMAND_SET_CHANNEL_MASKS,
                      __ATOMIC_RELEASE);
}

static void request_auto_switch_views(bool enabled)
{
    __atomic_store_n(&s_command_auto_switch_views, enabled ? 1U : 0U,
                     __ATOMIC_RELEASE);
    __atomic_fetch_or(&s_command_flags, COMMAND_SET_AUTO_SWITCH_VIEWS,
                      __ATOMIC_RELEASE);
}

static void request_transport_profile(uint8_t profile)
{
    if (profile > AUDIO_DASHBOARD_TRANSPORT_LOW_LATENCY) return;
    __atomic_store_n(&s_command_transport_profile, profile, __ATOMIC_RELEASE);
    __atomic_fetch_or(&s_command_flags, COMMAND_SET_TRANSPORT_PROFILE,
                      __ATOMIC_RELEASE);
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

static lv_obj_t *make_centered_label(lv_obj_t *parent,
                                     int x,
                                     int y,
                                     int width,
                                     int height,
                                     const char *text,
                                     const lv_font_t *font,
                                     uint32_t text_color)
{
    const int line_height = lv_font_get_line_height(font);
    const int text_y = y + (height - line_height) / 2;
    return make_label(parent, x, text_y, width, line_height, text, font,
                      text_color, LV_TEXT_ALIGN_CENTER);
}

static void style_card(lv_obj_t *card)
{
    lv_obj_set_style_shadow_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_y(card, 0, LV_PART_MAIN);
}

static lv_obj_t *make_chip(lv_obj_t *screen,
                           int x,
                           int width,
                           const char *text)
{
    lv_obj_t *chip = make_panel(screen, x, 22, width, 42, 0x101f31,
                                0x20364d, 12);
    make_centered_label(chip, 0, 0, width, 42, text,
                        &lv_font_montserrat_14, 0xd7e2ef);
    return chip;
}

static void update_window_button_styles(dashboard_page_t page)
{
    const uint32_t accents[DASHBOARD_PAGE_COUNT] = {
        0x42e8bd, 0xffa85c, 0xb596ff
    };
    for (unsigned i = 0; i < DASHBOARD_PAGE_COUNT; ++i) {
        lv_obj_t *button = s_window_buttons[i];
        if (button == NULL) {
            continue;
        }
        const bool active = i == (unsigned)page;
        lv_obj_set_style_bg_color(
            button, color(active ? 0x17283a : 0x0d1928), LV_PART_MAIN);
        lv_obj_set_style_border_color(
            button, color(active ? accents[i] : 0x263b51), LV_PART_MAIN);
        lv_obj_set_style_border_width(button, active ? 2 : 1, LV_PART_MAIN);
    }
}

static void close_window_menu(void)
{
    if (s_window_overlay != NULL) {
        lv_obj_add_flag(s_window_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void open_window_menu(void)
{
    if (s_window_overlay == NULL) {
        return;
    }
    const dashboard_page_t page = (dashboard_page_t)__atomic_load_n(
        &s_active_page, __ATOMIC_ACQUIRE);
    update_window_button_styles(page);
    lv_obj_clear_flag(s_window_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_window_overlay);
    lv_obj_set_y(s_window_sheet, 0);
}

static void set_active_page(dashboard_page_t page)
{
    static const char *const names[DASHBOARD_PAGE_COUNT] = {
        "monitor", "pedalboard", "settings"
    };
    if (page >= DASHBOARD_PAGE_COUNT) {
        return;
    }
    close_io_settings();
    lv_obj_add_flag(s_monitor_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pedalboard_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_settings_page, LV_OBJ_FLAG_HIDDEN);
    if (page == DASHBOARD_PAGE_MONITOR) {
        lv_obj_clear_flag(s_monitor_page, LV_OBJ_FLAG_HIDDEN);
    } else if (page == DASHBOARD_PAGE_PEDALBOARD) {
        lv_obj_clear_flag(s_pedalboard_page, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_settings_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (page != DASHBOARD_PAGE_MONITOR) {
        for (unsigned i = 0; i < DASHBOARD_CHANNELS; ++i) {
            __atomic_store_n(&s_pending_peak[i], 0U, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&s_active_page, (uint32_t)page, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "active view: %s", names[page]);
    update_window_button_styles(page);
    close_window_menu();
}

static void window_button_event(lv_event_t *event)
{
    const dashboard_page_t page = (dashboard_page_t)(uintptr_t)
        lv_event_get_user_data(event);
    set_active_page(page);
}

static void close_button_event(lv_event_t *event)
{
    (void)event;
    close_window_menu();
}

static void touch_poll_timer(lv_timer_t *timer)
{
    lv_indev_t *input = lv_timer_get_user_data(timer);
    if (input == NULL || s_touch_indicator == NULL) {
        return;
    }

    const bool pressed =
        lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED;
    if (!pressed) {
        if (s_touch_was_pressed) {
            lv_obj_add_flag(s_touch_indicator, LV_OBJ_FLAG_HIDDEN);
        }
        s_touch_was_pressed = false;
        s_touch_tracking = false;
        s_touch_gesture_consumed = false;
        return;
    }

    lv_point_t current;
    lv_indev_get_point(input, &current);
    lv_obj_set_pos(s_touch_indicator, current.x - 15, current.y - 15);
    lv_obj_clear_flag(s_touch_indicator, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_touch_indicator);

    const bool menu_open = s_window_overlay != NULL
        && !lv_obj_has_flag(s_window_overlay, LV_OBJ_FLAG_HIDDEN);
    if (!s_touch_was_pressed) {
        s_touch_start = current;
        s_touch_tracking = menu_open
                           || s_touch_start.y <= TOP_GESTURE_START_Y;
        s_touch_gesture_consumed = false;
        ESP_LOGD(TAG, "touch: press (%ld,%ld)",
                 (long)current.x, (long)current.y);
    }
    s_touch_was_pressed = true;

    if (!s_touch_tracking || s_touch_gesture_consumed) {
        return;
    }

    const int32_t dx = current.x - s_touch_start.x;
    const int32_t dy = current.y - s_touch_start.y;
    if (!menu_open && dy >= WINDOW_SWIPE_DISTANCE && abs(dx) <= 300) {
        ESP_LOGI(TAG, "touch: open windows (%ld,%ld -> %ld,%ld)",
                 (long)s_touch_start.x, (long)s_touch_start.y,
                 (long)current.x, (long)current.y);
        s_touch_gesture_consumed = true;
        open_window_menu();
    } else if (menu_open && dy <= -WINDOW_SWIPE_DISTANCE
               && abs(dx) <= 300) {
        ESP_LOGI(TAG, "touch: close windows (%ld,%ld -> %ld,%ld)",
                 (long)s_touch_start.x, (long)s_touch_start.y,
                 (long)current.x, (long)current.y);
        s_touch_gesture_consumed = true;
        close_window_menu();
    }
}

static void update_pedal_io_summary(pedal_io_view_t *view)
{
    if (view == NULL || view->rate == NULL) {
        return;
    }
    const uint32_t flags = __atomic_load_n(&s_status_flags, __ATOMIC_ACQUIRE);
    lv_label_set_text(
        view->rate,
        (flags & STATUS_RATE_VALID) != 0U
            ? s_pedal_rate_names[s_pedal_rate_index] : "--");
    const bool enabled = view->channel_enabled[0] || view->channel_enabled[1];
    const uint32_t status_color = enabled ? 0x48e0a8 : 0x4c5d6e;
    lv_obj_set_style_bg_color(view->status_bar, color(status_color),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(view->status_dot, color(status_color),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(view->card,
                                  color(enabled ? 0x285e50 : 0x294159),
                                  LV_PART_MAIN);
    for (unsigned channel = 0; channel < 2; ++channel) {
        const bool channel_enabled = view->channel_enabled[channel];
        const uint32_t active_color = channel_enabled ? 0x48e0a8 : 0x465666;
        lv_obj_set_style_bg_color(view->channel_dot[channel],
                                  color(active_color), LV_PART_MAIN);
        lv_obj_set_style_text_color(view->channel_text[channel],
                                    color(channel_enabled ? 0xe3ecef : 0x596d82),
                                    LV_PART_MAIN);
    }
}

static void refresh_io_settings(void)
{
    pedal_io_view_t *view = s_io_settings_target;
    if (view == NULL) {
        return;
    }
    lv_label_set_text(s_io_settings_title, view->settings_title);
    const uint32_t flags = __atomic_load_n(&s_status_flags, __ATOMIC_ACQUIRE);
    const bool rate_valid = (flags & STATUS_RATE_VALID) != 0U;
    lv_label_set_text(s_io_settings_rate,
                      rate_valid ? s_pedal_rate_names[s_pedal_rate_index]
                                 : "--");
    lv_obj_set_style_bg_color(s_io_settings_accent, color(view->accent),
                              LV_PART_MAIN);
    lv_obj_set_style_text_color(s_io_settings_title, color(view->accent),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(s_io_settings_rate, color(0xffb15f),
                                LV_PART_MAIN);
    const bool pc_controls = (flags & STATUS_PC_RATE_OWNER) != 0U;
    const bool rate_syncing = (flags & STATUS_RATE_SYNCING) != 0U;
    if (pc_controls) {
        snprintf(s_io_owner_text, sizeof(s_io_owner_text),
                 "SHARED RATE  |  WINDOWS CONTROL");
    } else {
        snprintf(s_io_owner_text, sizeof(s_io_owner_text), "%s",
                 rate_syncing ? "SHARED RATE  |  APPLYING" :
                                "SHARED RATE  |  LOCAL CONTROL");
    }
    lv_label_set_text_static(s_io_settings_owner, s_io_owner_text);
    lv_obj_set_style_text_color(
        s_io_settings_owner,
        color(pc_controls ? 0x8ba9ff : rate_syncing ? 0xffb15f : 0x48e0a8),
        LV_PART_MAIN);
    for (unsigned rate = 0; rate < 4; ++rate) {
        const bool selected = rate_valid && rate == s_pedal_rate_index;
        if (pc_controls || rate_syncing) {
            lv_obj_add_state(s_io_rate_buttons[rate], LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_io_rate_buttons[rate], LV_STATE_DISABLED);
        }
        lv_obj_set_style_bg_color(s_io_rate_buttons[rate],
                                  color(selected ? 0x9b642f :
                                        pc_controls ? 0x111e2c : 0x17283a),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_color(s_io_rate_buttons[rate],
                                      color(selected ? 0xffb15f : 0x30465d),
                                      LV_PART_MAIN);
    }
    for (unsigned channel = 0; channel < 2; ++channel) {
        if (view->channel_enabled[channel]) {
            lv_obj_add_state(s_io_channel_switches[channel], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_io_channel_switches[channel], LV_STATE_CHECKED);
        }
        lv_obj_set_style_bg_color(s_io_channel_switches[channel],
                                  color(0x48e0a8), LV_PART_INDICATOR);
    }
    update_pedal_io_summary(view);
}

static void close_io_settings(void)
{
    if (s_io_settings_overlay != NULL) {
        lv_obj_add_flag(s_io_settings_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    s_io_settings_target = NULL;
}

static void io_settings_close_event(lv_event_t *event)
{
    (void)event;
    close_io_settings();
}

static void io_panel_long_press_event(lv_event_t *event)
{
    s_io_settings_target = lv_event_get_user_data(event);
    if (s_io_settings_target == NULL || s_io_settings_overlay == NULL) {
        return;
    }
    refresh_io_settings();
    lv_obj_clear_flag(s_io_settings_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_io_settings_overlay);
}

static void io_rate_event(lv_event_t *event)
{
    if (s_io_settings_target == NULL) {
        return;
    }
    const unsigned rate = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    const uint32_t flags = __atomic_load_n(&s_status_flags, __ATOMIC_ACQUIRE);
    if (rate >= 4 || (flags & (STATUS_PC_RATE_OWNER | STATUS_RATE_SYNCING))) {
        refresh_io_settings();
        return;
    }
    request_sample_rate(s_pedal_rates[rate]);
}

static void io_channel_event(lv_event_t *event)
{
    if (s_io_settings_target == NULL) {
        return;
    }
    const unsigned channel =
        (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (channel < 2) {
        lv_obj_t *control = lv_event_get_target_obj(event);
        s_io_settings_target->channel_enabled[channel] =
            lv_obj_has_state(control, LV_STATE_CHECKED);
        update_pedal_io_summary(s_io_settings_target);
        request_channel_masks();
    }
}

static void create_compact_io_panel(lv_obj_t *parent,
                                    pedal_io_view_t *view,
                                    int x)
{
    view->card = make_panel(parent, x, 231, 58, 172, 0x0d1b2a,
                            0x294159, 14);
    style_card(view->card);
    lv_obj_add_flag(view->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(view->card, io_panel_long_press_event,
                        LV_EVENT_LONG_PRESSED, view);

    view->status_bar = make_panel(view->card, 0, 0, 4, 172,
                                  0x48e0a8, 0x48e0a8, 2);
    view->status_dot = make_panel(view->card, 24, 13, 10, 10,
                                  0x48e0a8, 0x48e0a8, 5);
    make_label(view->card, 4, 31, 50, 23, view->title,
               &lv_font_montserrat_16, view->accent, LV_TEXT_ALIGN_CENTER);
    view->rate = make_label(view->card, 4, 58, 50, 21, "48",
                            &lv_font_montserrat_14, 0xffb15f,
                            LV_TEXT_ALIGN_CENTER);
    make_label(view->card, 4, 78, 50, 15, "kHz",
               &lv_font_montserrat_12, 0x61778e, LV_TEXT_ALIGN_CENTER);
    make_panel(view->card, 10, 99, 38, 1, 0x263b51, 0x263b51, 0);

    for (unsigned channel = 0; channel < 2; ++channel) {
        const int y = 109 + (int)channel * 24;
        view->channel_dot[channel] = make_panel(view->card, 12, y, 10, 10,
                                                0x48e0a8, 0x48e0a8, 5);
        char name[4];
        snprintf(name, sizeof(name), "%u", channel + 1U);
        view->channel_text[channel] = make_label(
            view->card, 30, y - 3, 18, 18, name, &lv_font_montserrat_12,
            0xdce7f2, LV_TEXT_ALIGN_LEFT);
    }
    make_label(view->card, 4, 153, 50, 14, "HOLD",
               &lv_font_montserrat_12, 0x657b91, LV_TEXT_ALIGN_CENTER);
    update_pedal_io_summary(view);
}

static void create_pedal_cpu_panel(lv_obj_t *parent)
{
    lv_obj_t *cpu = make_panel(parent, 812, 14, 184, 60, 0x0e1c2b,
                               0x28405a, 15);
    style_card(cpu);
    make_label(cpu, 14, 8, 106, 17, "SEED3 CPU",
               &lv_font_montserrat_12, 0x7f93aa, LV_TEXT_ALIGN_LEFT);
    s_pedal_cpu_bar = lv_bar_create(cpu);
    lv_obj_set_pos(s_pedal_cpu_bar, 14, 36);
    lv_obj_set_size(s_pedal_cpu_bar, 112, 9);
    lv_bar_set_range(s_pedal_cpu_bar, 0, 100);
    lv_bar_set_value(s_pedal_cpu_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_pedal_cpu_bar, color(0x203145), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pedal_cpu_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_pedal_cpu_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_pedal_cpu_bar, color(0xffa85c),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_pedal_cpu_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_pedal_cpu_bar, 5, LV_PART_INDICATOR);
    s_pedal_cpu_value = make_label(cpu, 132, 14, 42, 28, s_cpu_text,
                                   &lv_font_montserrat_16, 0xffa85c,
                                   LV_TEXT_ALIGN_RIGHT);
}

static void create_io_settings_overlay(lv_obj_t *parent)
{
    s_io_settings_overlay = make_panel(parent, 0, 0, DASHBOARD_WIDTH,
                                       DASHBOARD_HEIGHT, 0x02070d, 0x02070d, 0);
    lv_obj_set_style_bg_opa(s_io_settings_overlay, 190, LV_PART_MAIN);
    lv_obj_add_flag(s_io_settings_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *sheet = make_panel(s_io_settings_overlay, 292, 134, 440, 332,
                                 0x0d1b2a, 0x304961, 20);
    style_card(sheet);
    s_io_settings_accent = make_panel(sheet, 0, 0, 440, 4,
                                      0x42e8bd, 0x42e8bd, 0);
    s_io_settings_title = make_label(sheet, 35, 24, 260, 28,
                                     "INPUT SETTINGS",
                                     &lv_font_montserrat_20, 0xf2f6fb,
                                     LV_TEXT_ALIGN_LEFT);
    s_io_settings_owner = make_label(
        sheet, 35, 62, 280, 18, s_io_owner_text,
        &lv_font_montserrat_12, 0x48e0a8, LV_TEXT_ALIGN_LEFT);
    s_io_settings_rate = make_label(sheet, 315, 24, 90, 28, "48",
                                    &lv_font_montserrat_20, 0x42e8bd,
                                    LV_TEXT_ALIGN_RIGHT);

    for (unsigned rate = 0; rate < 4; ++rate) {
        lv_obj_t *button = lv_button_create(sheet);
        lv_obj_set_pos(button, 35 + (int)rate * 96, 88);
        lv_obj_set_size(button, 82, 46);
        lv_obj_set_style_bg_color(button, color(0x17283a), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(button, color(0x30465d), LV_PART_MAIN);
        lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(button, 11, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(button, LV_OPA_TRANSP, LV_PART_MAIN);
        make_centered_label(button, 0, 0, 82, 46,
                            s_pedal_rate_names[rate],
                            &lv_font_montserrat_14, 0xf2f6fb);
        lv_obj_add_event_cb(button, io_rate_event, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)rate);
        s_io_rate_buttons[rate] = button;
    }
    make_label(sheet, 35, 154, 220, 18, "CHANNELS",
               &lv_font_montserrat_12, 0x748ba2, LV_TEXT_ALIGN_LEFT);
    for (unsigned channel = 0; channel < 2; ++channel) {
        const int y = 187 + (int)channel * 48;
        char name[16];
        snprintf(name, sizeof(name), "CHANNEL %u", channel + 1U);
        make_label(sheet, 35, y + 4, 190, 24, name,
                   &lv_font_montserrat_14, 0xdbe6f1, LV_TEXT_ALIGN_LEFT);
        lv_obj_t *toggle = lv_switch_create(sheet);
        lv_obj_set_pos(toggle, 341, y);
        lv_obj_set_size(toggle, 64, 30);
        lv_obj_set_style_bg_color(toggle, color(0x26394d), LV_PART_MAIN);
        lv_obj_set_style_bg_color(toggle, color(0x42e8bd), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(toggle, color(0xe8f0f8), LV_PART_KNOB);
        lv_obj_add_event_cb(toggle, io_channel_event, LV_EVENT_VALUE_CHANGED,
                            (void *)(uintptr_t)channel);
        s_io_channel_switches[channel] = toggle;
    }
    lv_obj_t *close = lv_button_create(sheet);
    lv_obj_set_pos(close, 293, 282);
    lv_obj_set_size(close, 112, 34);
    lv_obj_set_style_bg_color(close, color(0x17283a), LV_PART_MAIN);
    lv_obj_set_style_radius(close, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(close, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(close, LV_OPA_TRANSP, LV_PART_MAIN);
    make_centered_label(close, 0, 0, 112, 34, "CLOSE",
                        &lv_font_montserrat_12, 0xc4d0dc);
    lv_obj_add_event_cb(close, io_settings_close_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_io_settings_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void pedalboard_switch_event(lv_event_t *event)
{
    if (s_updating_pedalboard_switch) return;
    lv_obj_t *control = lv_event_get_target_obj(event);
    const uint32_t flags = __atomic_load_n(&s_status_flags, __ATOMIC_ACQUIRE);
    if ((flags & STATUS_PEDALBOARD_FORCED) != 0U) {
        lv_obj_add_state(control, LV_STATE_CHECKED);
        return;
    }
    request_pedalboard_enabled(
        lv_obj_has_state(control, LV_STATE_CHECKED));
}

static void create_pedalboard_control(lv_obj_t *parent)
{
    s_pedalboard_control_panel = make_panel(
        parent, 450, 14, 240, 60, 0x0e1c2b, 0x28405a, 15);
    style_card(s_pedalboard_control_panel);
    make_label(s_pedalboard_control_panel, 14, 8, 136, 17, "PEDALBOARD",
               &lv_font_montserrat_12, 0x91a4b8, LV_TEXT_ALIGN_LEFT);
    s_pedalboard_mode = make_label(
        s_pedalboard_control_panel, 14, 34, 142, 16,
        s_pedalboard_mode_text,
        &lv_font_montserrat_12, 0x48e0a8, LV_TEXT_ALIGN_LEFT);
    s_pedalboard_switch = lv_switch_create(s_pedalboard_control_panel);
    lv_obj_set_pos(s_pedalboard_switch, 166, 15);
    lv_obj_set_size(s_pedalboard_switch, 60, 30);
    lv_obj_set_style_bg_color(s_pedalboard_switch, color(0x26394d),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_pedalboard_switch, color(0x48e0a8),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_pedalboard_switch, color(0xe8f0f8),
                              LV_PART_KNOB);
    lv_obj_add_state(s_pedalboard_switch,
                     LV_STATE_CHECKED | LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_pedalboard_switch, pedalboard_switch_event,
                        LV_EVENT_VALUE_CHANGED, NULL);
}

static void create_pedalboard(lv_obj_t *screen)
{
    s_pedalboard_page = make_panel(screen, 0, 0, DASHBOARD_WIDTH,
                                   DASHBOARD_HEIGHT, 0x07111f, 0x07111f, 0);
    make_panel(s_pedalboard_page, 0, 0, DASHBOARD_WIDTH, 5,
               0xffa85c, 0xffa85c, 0);
    make_label(s_pedalboard_page, 32, 20, 300, 36, "PEDALBOARD",
               &lv_font_montserrat_28, 0xf4f8ff, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *subtitle = make_label(
        s_pedalboard_page, 34, 57, 390, 18, "SIGNAL ROUTING WORKSPACE",
        &lv_font_montserrat_12, 0x7589a3, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(subtitle, 2, LV_PART_MAIN);
    make_chip(s_pedalboard_page, 702, 88, "0 NODES");
    create_pedalboard_control(s_pedalboard_page);
    create_pedal_cpu_panel(s_pedalboard_page);

    create_compact_io_panel(s_pedalboard_page,
                            &s_pedal_io[PEDAL_IO_INPUT], 12);
    create_compact_io_panel(s_pedalboard_page,
                            &s_pedal_io[PEDAL_IO_OUTPUT], 954);

    lv_obj_t *workspace = make_panel(s_pedalboard_page, 82, 104, 860, 426,
                                     0x091522, 0x263b51, 20);
    style_card(workspace);
    make_label(workspace, 18, 14, 220, 18, "SIGNAL CANVAS",
               &lv_font_montserrat_12, 0x536b83, LV_TEXT_ALIGN_LEFT);
    for (int x = 46; x < 848; x += 48) {
        lv_obj_t *line = make_panel(workspace, x, 42, 1, 366,
                                    0x15263a, 0x15263a, 0);
        lv_obj_set_style_opa(line, 130, LV_PART_MAIN);
    }
    for (int y = 42; y < 410; y += 48) {
        lv_obj_t *line = make_panel(workspace, 18, y, 824, 1,
                                    0x15263a, 0x15263a, 0);
        lv_obj_set_style_opa(line, 130, LV_PART_MAIN);
    }
    lv_obj_t *empty = make_panel(workspace, 270, 128, 320, 142,
                                 0x0d1c2b, 0x2d4359, 18);
    make_panel(empty, 151, 24, 18, 18, 0xffa85c, 0xffa85c, 9);
    make_label(empty, 20, 56, 280, 27, "EMPTY PEDALBOARD",
               &lv_font_montserrat_20, 0xe9eff7, LV_TEXT_ALIGN_CENTER);
    make_label(empty, 20, 92, 280, 20, "EFFECT NODES WILL APPEAR HERE",
               &lv_font_montserrat_12, 0x7389a0, LV_TEXT_ALIGN_CENTER);

    create_io_settings_overlay(s_pedalboard_page);
    lv_obj_add_flag(s_pedalboard_page, LV_OBJ_FLAG_HIDDEN);
}

static void auto_switch_views_event(lv_event_t *event)
{
    if (s_updating_auto_switch) return;
    lv_obj_t *control = lv_event_get_target_obj(event);
    request_auto_switch_views(
        lv_obj_has_state(control, LV_STATE_CHECKED));
}

static void transport_profile_event(lv_event_t *event)
{
    request_transport_profile((uint8_t)(uintptr_t)lv_event_get_user_data(event));
}

static void create_settings(lv_obj_t *screen)
{
    s_settings_page = make_panel(screen, 0, 0, DASHBOARD_WIDTH,
                                 DASHBOARD_HEIGHT, 0x07111f, 0x07111f, 0);
    make_panel(s_settings_page, 0, 0, DASHBOARD_WIDTH, 5,
               0xb596ff, 0xb596ff, 0);
    make_label(s_settings_page, 32, 20, 300, 36, "SETTINGS",
               &lv_font_montserrat_28, 0xf4f8ff, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *subtitle = make_label(
        s_settings_page, 34, 57, 390, 18, "SYSTEM AND DISPLAY BEHAVIOR",
        &lv_font_montserrat_12, 0x7589a3, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(subtitle, 2, LV_PART_MAIN);
    make_chip(s_settings_page, 848, 120, "SYSTEM");

    lv_obj_t *card = make_panel(s_settings_page, 80, 108, 864, 178,
                                0x0d1928, 0x2d4057, 18);
    style_card(card);
    make_panel(card, 0, 0, 5, 178, 0xb596ff, 0xb596ff, 2);
    make_label(card, 32, 24, 570, 28, "AUTO WINDOW SWITCH",
               &lv_font_montserrat_20, 0xf1f5fb, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *description = make_label(
        card, 32, 62, 650, 48,
        "PC CONNECTED: OPEN MONITOR\nPC DISCONNECTED: OPEN PEDALBOARD",
        &lv_font_montserrat_14, 0x8ea2b7, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(description, 8, LV_PART_MAIN);
    make_label(card, 32, 137, 650, 18,
               "MANUAL WINDOW SELECTION REMAINS AVAILABLE",
               &lv_font_montserrat_12, 0x607890, LV_TEXT_ALIGN_LEFT);

    s_auto_switch_views = lv_switch_create(card);
    lv_obj_set_pos(s_auto_switch_views, 754, 66);
    lv_obj_set_size(s_auto_switch_views, 72, 36);
    lv_obj_set_style_bg_color(s_auto_switch_views, color(0x26394d),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_auto_switch_views, color(0xb596ff),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_auto_switch_views, color(0xf1f4f8),
                              LV_PART_KNOB);
    lv_obj_add_state(s_auto_switch_views, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_auto_switch_views, auto_switch_views_event,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *transport = make_panel(s_settings_page, 80, 310, 864, 214,
                                     0x0d1928, 0x2d4057, 18);
    style_card(transport);
    make_panel(transport, 0, 0, 5, 214, 0x48e0a8, 0x48e0a8, 2);
    make_label(transport, 32, 22, 420, 28, "SPI TRANSPORT PROFILE",
               &lv_font_montserrat_20, 0xf1f5fb, LV_TEXT_ALIGN_LEFT);
    s_transport_active = make_label(
        transport, 476, 26, 350, 22, "ACTIVE: LOCAL",
        &lv_font_montserrat_14, 0x48e0a8, LV_TEXT_ALIGN_RIGHT);
    make_label(transport, 32, 60, 800, 36,
               "BALANCED SAVES SEED CPU AT 88.2/96 kHz.\nLOW LATENCY KEEPS 32-FRAME AUDIO BLOCKS.",
               &lv_font_montserrat_12, 0x8ea2b7, LV_TEXT_ALIGN_LEFT);
    static const char *const names[2] = {"BALANCED", "LOW LATENCY"};
    static const char *const details[2] = {
        "32 / 64 FRAMES", "32 FRAMES"
    };
    for (uint8_t profile = 0; profile < 2; ++profile) {
        lv_obj_t *button = lv_button_create(transport);
        lv_obj_set_pos(button, 32 + (int)profile * 408, 118);
        lv_obj_set_size(button, 384, 66);
        lv_obj_set_style_bg_color(button, color(0x17283a), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(button, color(0x30465d), LV_PART_MAIN);
        lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
        make_label(button, 18, 10, 220, 22, names[profile],
                   &lv_font_montserrat_16, 0xf2f6fb, LV_TEXT_ALIGN_LEFT);
        make_label(button, 18, 38, 220, 16, details[profile],
                   &lv_font_montserrat_12, 0x7d92a9, LV_TEXT_ALIGN_LEFT);
        lv_obj_add_event_cb(button, transport_profile_event,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)profile);
        s_transport_buttons[profile] = button;
    }
    lv_obj_add_flag(s_settings_page, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *create_window_button(lv_obj_t *parent,
                                      dashboard_page_t page,
                                      int x,
                                      const char *title,
                                      const char *description,
                                      uint32_t accent)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, 92);
    lv_obj_set_size(button, 286, 116);
    lv_obj_set_style_bg_color(button, color(0x0d1928), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, color(0x263b51), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(button, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_y(button, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    set_no_scroll(button);
    make_panel(button, 18, 20, 12, 12, accent, accent, 6);
    make_label(button, 46, 14, 216, 28, title,
               &lv_font_montserrat_20, 0xf2f6fb, LV_TEXT_ALIGN_LEFT);
    make_label(button, 46, 55, 216, 36, description,
               &lv_font_montserrat_12, 0x7d92a9, LV_TEXT_ALIGN_LEFT);
    lv_obj_add_event_cb(button, window_button_event, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)page);
    s_window_buttons[page] = button;
    return button;
}

static void create_window_menu(lv_obj_t *screen)
{
    s_window_overlay = make_panel(screen, 0, 0, DASHBOARD_WIDTH,
                                  DASHBOARD_HEIGHT, 0x02070d, 0x02070d, 0);
    lv_obj_set_style_bg_opa(s_window_overlay, 190, LV_PART_MAIN);
    s_window_sheet = make_panel(s_window_overlay, 0, 0, DASHBOARD_WIDTH, 254,
                                0x0a1624, 0x263b51, 0);
    lv_obj_set_style_border_side(s_window_sheet, LV_BORDER_SIDE_BOTTOM,
                                 LV_PART_MAIN);
    lv_obj_set_style_border_width(s_window_sheet, 1, LV_PART_MAIN);
    make_panel(s_window_sheet, 0, 0, DASHBOARD_WIDTH, 5,
               0x42e8bd, 0x42e8bd, 0);
    make_label(s_window_sheet, 32, 24, 300, 30, "WINDOWS",
               &lv_font_montserrat_20, 0xf4f8ff, LV_TEXT_ALIGN_LEFT);
    make_label(s_window_sheet, 34, 58, 360, 18, "CHOOSE THE ACTIVE VIEW",
               &lv_font_montserrat_12, 0x71879e, LV_TEXT_ALIGN_LEFT);

    create_window_button(s_window_sheet, DASHBOARD_PAGE_MONITOR, 48,
                         "MONITOR", "METERS / USB / CPU", 0x42e8bd);
    create_window_button(s_window_sheet, DASHBOARD_PAGE_PEDALBOARD, 369,
                         "PEDALBOARD", "ROUTING / EFFECTS", 0xffa85c);
    create_window_button(s_window_sheet, DASHBOARD_PAGE_SETTINGS, 690,
                         "SETTINGS", "SYSTEM / DISPLAY", 0xb596ff);

    lv_obj_t *close = lv_button_create(s_window_sheet);
    lv_obj_set_pos(close, 856, 24);
    lv_obj_set_size(close, 136, 44);
    lv_obj_set_style_bg_color(close, color(0x17283a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(close, color(0x2b4259), LV_PART_MAIN);
    lv_obj_set_style_border_width(close, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(close, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(close, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(close, LV_OPA_TRANSP, LV_PART_MAIN);
    make_centered_label(close, 0, 0, 136, 44, "CLOSE",
                        &lv_font_montserrat_12, 0xb7c6d6);
    lv_obj_add_event_cb(close, close_button_event, LV_EVENT_CLICKED, NULL);
    update_window_button_styles(DASHBOARD_PAGE_PEDALBOARD);
    lv_obj_add_flag(s_window_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void create_touch_indicator(lv_obj_t *screen)
{
    s_touch_indicator = lv_obj_create(screen);
    lv_obj_set_size(s_touch_indicator, 30, 30);
    lv_obj_set_style_radius(s_touch_indicator, 15, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_touch_indicator, color(0xb7bec7), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_touch_indicator, 96, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_touch_indicator, color(0xe1e5ea),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_touch_indicator, 128, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_touch_indicator, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_touch_indicator, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_touch_indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_touch_indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_touch_indicator, LV_OBJ_FLAG_HIDDEN);
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

    s_monitor_page = make_panel(screen, 0, 0, DASHBOARD_WIDTH,
                                DASHBOARD_HEIGHT, 0x07111f, 0x07111f, 0);
    make_panel(s_monitor_page, 0, 0, DASHBOARD_WIDTH, 5,
               0x42e8bd, 0x42e8bd, 0);
    make_label(s_monitor_page, 32, 20, 360, 36, "SEED3 / P4 AUDIO",
               &lv_font_montserrat_28, 0xf4f8ff, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *subtitle = make_label(
        s_monitor_page, 34, 57, 340, 18, "LOW-LATENCY USB AUDIO MONITOR",
        &lv_font_montserrat_12, 0x7589a3, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(subtitle, 2, LV_PART_MAIN);

    make_chip(s_monitor_page, 420, 112, "44.1-96 kHz");
    make_chip(s_monitor_page, 544, 96, "24 bit");
    make_chip(s_monitor_page, 652, 110, "USB HS");

    lv_obj_t *cpu = make_panel(s_monitor_page, 786, 14, 210, 60, 0x0e1c2b,
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

    lv_obj_t *side = make_panel(s_monitor_page, 28, 104, 210, 420, 0x0c1827,
                                0x1b3047, 20);
    style_card(side);
    lv_obj_t *monitor = make_label(side, 20, 20, 170, 22, "LIVE MONITOR",
                                   &lv_font_montserrat_14, 0xa8bbcf,
                                   LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(monitor, 2, LV_PART_MAIN);
    make_label(side, 20, 54, 170, 18, "INPUT / MICROPHONE",
               &lv_font_montserrat_12, 0x42e8bd, LV_TEXT_ALIGN_LEFT);
    s_capture_rate = make_label(side, 20, 74, 94, 40,
                                s_sample_rate_text,
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
                                 s_sample_rate_text,
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

    lv_obj_t *input_group = make_label(s_monitor_page, 270, 93, 300, 22,
        "INPUT  |  SEED3 ADC", &lv_font_montserrat_12, 0x42e8bd,
        LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(input_group, 2, LV_PART_MAIN);
    lv_obj_t *output_group = make_label(s_monitor_page, 626, 93, 330, 22,
        "OUTPUT  |  USB PLAYBACK", &lv_font_montserrat_12, 0x8ba9ff,
        LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(output_group, 2, LV_PART_MAIN);

    create_meter(s_monitor_page, 0, 264, "IN 1", true);
    create_meter(s_monitor_page, 1, 424, "IN 2", true);
    create_meter(s_monitor_page, 2, 620, "OUT 1", false);
    create_meter(s_monitor_page, 3, 780, "OUT 2", false);

    make_panel(s_monitor_page, 32, 544, 960, 1, 0x14283d, 0x14283d, 0);
    s_footer = make_label(s_monitor_page, 32, 558, 960, 22, s_footer_text,
                          &lv_font_montserrat_12, 0x647a92,
                          LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(s_footer, 1, LV_PART_MAIN);

    create_pedalboard(screen);
    create_settings(screen);
    create_window_menu(screen);
    create_touch_indicator(screen);
    set_active_page(DASHBOARD_PAGE_PEDALBOARD);
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

static void update_pedalboard_status(uint32_t flags,
                                     uint32_t sample_rate,
                                     bool sample_rate_valid)
{
    const bool forced = (flags & STATUS_PEDALBOARD_FORCED) != 0U;
    const bool enabled = (flags & STATUS_PEDALBOARD_ENABLED) != 0U;
    const bool pedalboard_syncing =
        (flags & STATUS_PEDALBOARD_SYNCING) != 0U;
    const bool rate_syncing = (flags & STATUS_RATE_SYNCING) != 0U;
    const bool rate_conflict = (flags & STATUS_RATE_CONFLICT) != 0U;

    if (sample_rate_valid) {
        const uint8_t rate_index = rate_index_from_hz(sample_rate);
        if (rate_index != s_pedal_rate_index) {
            s_pedal_rate_index = rate_index;
            update_pedal_io_summary(&s_pedal_io[PEDAL_IO_INPUT]);
            update_pedal_io_summary(&s_pedal_io[PEDAL_IO_OUTPUT]);
        }
    }

    const char *mode;
    uint32_t mode_color;
    if (forced) {
        mode = rate_syncing ? "LOCAL  |  SYNCING" : "LOCAL  |  FORCED ON";
        mode_color = rate_syncing ? 0xffb15f : 0x48e0a8;
    } else if (rate_conflict) {
        mode = "WINDOWS  |  CONFLICT";
        mode_color = 0xff6b6b;
    } else if (pedalboard_syncing || rate_syncing) {
        mode = "WINDOWS  |  APPLYING";
        mode_color = 0xffb15f;
    } else {
        mode = enabled ? "WINDOWS  |  ON" : "WINDOWS  |  BYPASS";
        mode_color = enabled ? 0x48e0a8 : 0x7d92a9;
    }
    snprintf(s_pedalboard_mode_text, sizeof(s_pedalboard_mode_text), "%s",
             mode);
    lv_label_set_text_static(s_pedalboard_mode, s_pedalboard_mode_text);
    lv_obj_set_style_text_color(s_pedalboard_mode, color(mode_color),
                                LV_PART_MAIN);

    s_updating_pedalboard_switch = true;
    if (enabled || forced) {
        lv_obj_add_state(s_pedalboard_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_pedalboard_switch, LV_STATE_CHECKED);
    }
    if (forced || pedalboard_syncing) {
        lv_obj_add_state(s_pedalboard_switch, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_pedalboard_switch, LV_STATE_DISABLED);
    }
    lv_obj_set_style_bg_color(
        s_pedalboard_switch,
        color(enabled || forced ? 0x48e0a8 : 0x4c5d6e),
        LV_PART_INDICATOR);
    s_updating_pedalboard_switch = false;

    /* In LOCAL the pedalboard is mandatory, so a disabled switch would only
     * consume space and suggest a choice that does not exist. */
    if (forced) {
        lv_obj_add_flag(s_pedalboard_control_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_pedalboard_control_panel, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_io_settings_target != NULL) refresh_io_settings();
}

static void update_auto_window_switch(uint32_t flags)
{
    const bool enabled = (flags & STATUS_AUTO_SWITCH_VIEWS) != 0U;
    const bool host_connected = (flags & STATUS_USB_CONNECTED) != 0U;

    s_updating_auto_switch = true;
    if (enabled) {
        lv_obj_add_state(s_auto_switch_views, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_auto_switch_views, LV_STATE_CHECKED);
    }
    s_updating_auto_switch = false;

    const bool transition = !s_auto_page_state_known
                            || host_connected != s_last_host_connected;
    const bool newly_enabled = s_auto_page_state_known
                               && enabled && !s_last_auto_switch;
    if (enabled && (transition || newly_enabled)) {
        set_active_page(host_connected ? DASHBOARD_PAGE_MONITOR
                                       : DASHBOARD_PAGE_PEDALBOARD);
    }
    s_auto_page_state_known = true;
    s_last_host_connected = host_connected;
    s_last_auto_switch = enabled;
}

static void update_transport_status(uint32_t profile, uint32_t mode)
{
    if (profile > AUDIO_DASHBOARD_TRANSPORT_LOW_LATENCY) {
        profile = AUDIO_DASHBOARD_TRANSPORT_BALANCED;
    }
    for (uint32_t index = 0; index < 2; ++index) {
        const bool selected = index == profile;
        lv_obj_set_style_bg_color(
            s_transport_buttons[index],
            color(selected ? 0x173b3c : 0x17283a), LV_PART_MAIN);
        lv_obj_set_style_border_color(
            s_transport_buttons[index],
            color(selected ? 0x48e0a8 : 0x30465d), LV_PART_MAIN);
        lv_obj_set_style_border_width(
            s_transport_buttons[index], selected ? 2 : 1, LV_PART_MAIN);
    }
    const char *active = mode == AUDIO_DASHBOARD_TRANSPORT_LOCAL
                             ? "ACTIVE: LOCAL"
                         : mode == AUDIO_DASHBOARD_TRANSPORT_LOW_LATENCY
                             ? "ACTIVE: LOW LATENCY"
                             : "ACTIVE: BALANCED";
    lv_label_set_text(s_transport_active, active);
    lv_obj_set_style_text_color(
        s_transport_active,
        color(mode == AUDIO_DASHBOARD_TRANSPORT_LOCAL ? 0xffca68 : 0x48e0a8),
        LV_PART_MAIN);
}

static void update_status_text(void)
{
    static uint32_t shown_cpu = UINT32_MAX;
    static uint32_t shown_sample_rate = UINT32_MAX;
    static uint32_t shown_buffer = UINT32_MAX;
    static uint32_t shown_flags = UINT32_MAX;
    static uint32_t shown_spi_errors = UINT32_MAX;
    static uint32_t shown_crc_errors = UINT32_MAX;
    static uint32_t shown_capture_mask = UINT32_MAX;
    static uint32_t shown_playback_mask = UINT32_MAX;
    static uint32_t shown_transport_profile = UINT32_MAX;
    static uint32_t shown_transport_mode = UINT32_MAX;

    const uint32_t cpu = __atomic_load_n(&s_seed_cpu_percent, __ATOMIC_ACQUIRE);
    const uint32_t sample_rate =
        __atomic_load_n(&s_sample_rate_hz, __ATOMIC_ACQUIRE);
    const uint32_t buffer = __atomic_load_n(&s_buffer_ms, __ATOMIC_ACQUIRE);
    const uint32_t flags = __atomic_load_n(&s_status_flags, __ATOMIC_ACQUIRE);
    const bool sample_rate_valid = (flags & STATUS_RATE_VALID) != 0U;
    const uint32_t spi_errors = __atomic_load_n(&s_spi_errors, __ATOMIC_ACQUIRE);
    const uint32_t crc_errors = __atomic_load_n(&s_crc_errors, __ATOMIC_ACQUIRE);
    const uint32_t capture_mask = __atomic_load_n(
        &s_capture_channel_mask, __ATOMIC_ACQUIRE) & 3U;
    const uint32_t playback_mask = __atomic_load_n(
        &s_playback_channel_mask, __ATOMIC_ACQUIRE) & 3U;
    const uint32_t transport_profile = __atomic_load_n(
        &s_transport_profile, __ATOMIC_ACQUIRE);
    const uint32_t transport_mode = __atomic_load_n(
        &s_transport_mode, __ATOMIC_ACQUIRE);

    const bool transport_changed = transport_profile != shown_transport_profile
                                   || transport_mode != shown_transport_mode;
    if (transport_changed) {
        shown_transport_profile = transport_profile;
        shown_transport_mode = transport_mode;
        update_transport_status(transport_profile, transport_mode);
    }

    if (capture_mask != shown_capture_mask ||
        playback_mask != shown_playback_mask) {
        shown_capture_mask = capture_mask;
        shown_playback_mask = playback_mask;
        for (unsigned channel = 0; channel < 2; ++channel) {
            s_pedal_io[PEDAL_IO_INPUT].channel_enabled[channel] =
                (capture_mask & (1U << channel)) != 0U;
            s_pedal_io[PEDAL_IO_OUTPUT].channel_enabled[channel] =
                (playback_mask & (1U << channel)) != 0U;
        }
        update_pedal_io_summary(&s_pedal_io[PEDAL_IO_INPUT]);
        update_pedal_io_summary(&s_pedal_io[PEDAL_IO_OUTPUT]);
        if (s_io_settings_target != NULL) refresh_io_settings();
    }

    if (cpu != shown_cpu) {
        shown_cpu = cpu;
        snprintf(s_cpu_text, sizeof(s_cpu_text), "%lu%%", (unsigned long)cpu);
        lv_label_set_text_static(s_cpu_value, s_cpu_text);
        lv_bar_set_value(s_cpu_bar, (int32_t)cpu, LV_ANIM_OFF);
        if (s_pedal_cpu_value != NULL) {
            lv_label_set_text_static(s_pedal_cpu_value, s_cpu_text);
        }
        if (s_pedal_cpu_bar != NULL) {
            lv_bar_set_value(s_pedal_cpu_bar, (int32_t)cpu, LV_ANIM_OFF);
        }
    }
    const bool rate_validity_changed = flags != shown_flags;
    const bool sample_rate_changed = sample_rate != shown_sample_rate
                                     || rate_validity_changed;
    if (sample_rate_changed) {
        shown_sample_rate = sample_rate;
        if (!sample_rate_valid) {
            snprintf(s_sample_rate_text, sizeof(s_sample_rate_text), "--");
        } else if (sample_rate % 1000U == 0U) {
            snprintf(s_sample_rate_text, sizeof(s_sample_rate_text), "%lu",
                     (unsigned long)(sample_rate / 1000U));
        } else {
            snprintf(s_sample_rate_text, sizeof(s_sample_rate_text),
                     "%lu.%lu", (unsigned long)(sample_rate / 1000U),
                     (unsigned long)((sample_rate % 1000U) / 100U));
        }
        lv_label_set_text_static(s_capture_rate, s_sample_rate_text);
        lv_label_set_text_static(s_playback_rate, s_sample_rate_text);
    }
    const bool flags_changed = flags != shown_flags;
    if (flags_changed) {
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
        update_auto_window_switch(flags);
    }
    if (sample_rate_changed || flags_changed) {
        update_pedalboard_status(flags, sample_rate, sample_rate_valid);
    }
    if (buffer != shown_buffer || flags_changed || transport_changed ||
        spi_errors != shown_spi_errors || crc_errors != shown_crc_errors) {
        shown_buffer = buffer;
        shown_spi_errors = spi_errors;
        shown_crc_errors = crc_errors;
        const char *transport_name =
            transport_mode == AUDIO_DASHBOARD_TRANSPORT_LOCAL ? "LOCAL"
            : transport_mode == AUDIO_DASHBOARD_TRANSPORT_LOW_LATENCY ? "LOW LAT"
                                                                      : "BALANCED";
        snprintf(s_footer_text, sizeof(s_footer_text),
                 "CLOCK %s  |  %s  |  2 IN / 2 OUT  |  BUFFER %lu ms  |  ERR %lu/%lu",
                 (flags & STATUS_PC_RATE_OWNER) ? "WINDOWS" : "LOCAL",
                 transport_name,
                 (unsigned long)buffer, (unsigned long)spi_errors,
                 (unsigned long)crc_errors);
        lv_label_set_text_static(s_footer, s_footer_text);
    }
}

static void status_poll_timer(lv_timer_t *timer)
{
    (void)timer;
    update_status_text();
}

static void dashboard_task(void *argument)
{
    (void)argument;
    while (true) {
        if (!audio_dashboard_needs_live_audio()) {
            vTaskDelay(pdMS_TO_TICKS(PEDALBOARD_REFRESH_MS));
            continue;
        }
        uint32_t peaks[DASHBOARD_CHANNELS];
        for (unsigned i = 0; i < DASHBOARD_CHANNELS; ++i) {
            peaks[i] = __atomic_exchange_n(&s_pending_peak[i], 0U,
                                           __ATOMIC_ACQ_REL);
        }
        if (bsp_display_lock(pdMS_TO_TICKS(50))) {
            for (unsigned i = 0; i < DASHBOARD_CHANNELS; ++i) {
                update_meter(i, peaks[i]);
            }
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
    lv_indev_t *touch = bsp_display_get_input_dev();
    if (touch == NULL) {
        ESP_LOGE(TAG, "GT911 was not registered as an LVGL input device");
        return ESP_ERR_NOT_FOUND;
    }
    bsp_display_backlight_on();
    if (!bsp_display_lock(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    create_dashboard();
    if (lv_timer_create(touch_poll_timer, 10, touch) == NULL) {
        bsp_display_unlock();
        return ESP_ERR_NO_MEM;
    }
    if (lv_timer_create(status_poll_timer, PEDALBOARD_REFRESH_MS, NULL) == NULL) {
        bsp_display_unlock();
        return ESP_ERR_NO_MEM;
    }
    bsp_display_unlock();

    const BaseType_t created = xTaskCreatePinnedToCore(
        dashboard_task, "audio_ui", 6144, NULL, 3, NULL, 0);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "1024x600 LVGL audio dashboard and GT911 touch started");
    return ESP_OK;
}

bool audio_dashboard_needs_live_audio(void)
{
    return __atomic_load_n(&s_active_page, __ATOMIC_ACQUIRE)
           == DASHBOARD_PAGE_MONITOR;
}

void audio_dashboard_submit_peaks(uint32_t input_left,
                                  uint32_t input_right,
                                  uint32_t output_left,
                                  uint32_t output_right)
{
    if (!audio_dashboard_needs_live_audio()) {
        return;
    }
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
    if (status->usb_connected) flags |= STATUS_USB_CONNECTED;
    if (status->usb_suspended) flags |= STATUS_USB_SUSPENDED;
    if (status->capture_active) flags |= STATUS_CAPTURE_ACTIVE;
    if (status->playback_active) flags |= STATUS_PLAYBACK_ACTIVE;
    if (status->pc_controls_rate) flags |= STATUS_PC_RATE_OWNER;
    if (status->pedalboard_enabled) flags |= STATUS_PEDALBOARD_ENABLED;
    if (status->pedalboard_forced) flags |= STATUS_PEDALBOARD_FORCED;
    if (status->pedalboard_syncing) flags |= STATUS_PEDALBOARD_SYNCING;
    if (status->auto_switch_views) flags |= STATUS_AUTO_SWITCH_VIEWS;
    if (status->rate_syncing) flags |= STATUS_RATE_SYNCING;
    if (status->rate_conflict) flags |= STATUS_RATE_CONFLICT;
    if (status->sample_rate_valid) flags |= STATUS_RATE_VALID;
    __atomic_store_n(&s_sample_rate_hz, status->sample_rate,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_buffer_ms, status->buffer_ms, __ATOMIC_RELEASE);
    __atomic_store_n(&s_status_flags, flags, __ATOMIC_RELEASE);
    __atomic_store_n(&s_spi_errors, status->spi_errors, __ATOMIC_RELEASE);
    __atomic_store_n(&s_crc_errors, status->crc_errors, __ATOMIC_RELEASE);
    __atomic_store_n(&s_capture_channel_mask,
                     status->capture_channel_mask & 3U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_playback_channel_mask,
                     status->playback_channel_mask & 3U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_transport_profile, status->transport_profile,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_transport_mode, status->transport_mode,
                     __ATOMIC_RELEASE);
}

bool audio_dashboard_take_command(audio_dashboard_command_t *command)
{
    if (command == NULL) return false;
    const uint32_t flags = __atomic_exchange_n(
        &s_command_flags, 0U, __ATOMIC_ACQ_REL);
    memset(command, 0, sizeof(*command));
    if (flags == 0U) return false;
    if ((flags & COMMAND_SET_RATE) != 0U) {
        command->set_sample_rate = true;
        command->sample_rate = __atomic_load_n(
            &s_command_rate, __ATOMIC_ACQUIRE);
    }
    if ((flags & COMMAND_SET_PEDALBOARD) != 0U) {
        command->set_pedalboard_enabled = true;
        command->pedalboard_enabled = __atomic_load_n(
            &s_command_pedalboard, __ATOMIC_ACQUIRE) != 0U;
    }
    if ((flags & COMMAND_SET_CHANNEL_MASKS) != 0U) {
        command->set_channel_masks = true;
        command->capture_channel_mask = (uint8_t)__atomic_load_n(
            &s_command_capture_mask, __ATOMIC_ACQUIRE);
        command->playback_channel_mask = (uint8_t)__atomic_load_n(
            &s_command_playback_mask, __ATOMIC_ACQUIRE);
    }
    if ((flags & COMMAND_SET_AUTO_SWITCH_VIEWS) != 0U) {
        command->set_auto_switch_views = true;
        command->auto_switch_views = __atomic_load_n(
            &s_command_auto_switch_views, __ATOMIC_ACQUIRE) != 0U;
    }
    if ((flags & COMMAND_SET_TRANSPORT_PROFILE) != 0U) {
        command->set_transport_profile = true;
        command->transport_profile = (uint8_t)__atomic_load_n(
            &s_command_transport_profile, __ATOMIC_ACQUIRE);
    }
    return true;
}
