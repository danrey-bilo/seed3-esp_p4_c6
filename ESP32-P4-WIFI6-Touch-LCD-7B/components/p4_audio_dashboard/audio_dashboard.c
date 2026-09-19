#include "audio_dashboard.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "seedfx_graph_protocol.h"
#include "pedalboard_widgets.h"
#include "seedfx_ports.h"
#include "seedfx_routing.h"
#include "seedfx_routing_catalog.h"
#include "pedal_patch.h"
#include "pedal_layout.h"
#include "pedal_categories.h"
#include "pedal_gesture.h"
#include "wifi_view.h"
#include "freertos/queue.h"

enum {
    DASHBOARD_WIDTH = 1024,
    DASHBOARD_HEIGHT = 600,
    DASHBOARD_REFRESH_MS = 33,
    PEDALBOARD_REFRESH_MS = 200,
    DASHBOARD_CHANNELS = 4,
    TOP_GESTURE_START_Y = 80,
    WINDOW_SWIPE_DISTANCE = 70,
    PEDALBOARD_VISIBLE_NODES = 12,
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
    lv_obj_t *channel_dot[2];
    lv_obj_t *channel_text[2];
    lv_obj_t *level[2];
    float displayed[2];
} pedal_io_view_t;

typedef struct {
    uint16_t type;
    uint16_t id;
    uint8_t source_channel;
    bool enabled;
    float parameters[SEEDFX_MAX_PARAMS];
} pedal_node_t;

static SeedFxCatalogEntry s_effect_catalog[SEEDFX_MAX_EFFECTS] = {
    {.package_id=0x10001, .effect_type=SEEDFX_EFFECT_BYPASS,
     .shape=SEEDFX_SHAPE_RECTANGLE, .color_rgb=0x5d7892,
     .name="BYPASS", .category="utility", .cpu_label="~0.02% CPU"},
    {.package_id=0x10002, .effect_type=SEEDFX_EFFECT_GAIN,
     .shape=SEEDFX_SHAPE_ROUNDED, .parameter_count=1, .color_rgb=0x42e8bd,
     .name="GAIN", .category="utility", .cpu_label="~0.08% CPU",
     .parameters={{.name="Gain", .unit="dB", .minimum=-60, .maximum=18,
                   .step=.5f, .default_value=0}}},
    {.package_id=0x10003, .effect_type=SEEDFX_EFFECT_MUTE,
     .shape=SEEDFX_SHAPE_RECTANGLE, .color_rgb=0x718096,
     .name="MUTE", .category="utility", .cpu_label="~0.02% CPU"},
    {.package_id=0x10004, .effect_type=SEEDFX_EFFECT_POLARITY,
     .shape=SEEDFX_SHAPE_ROUNDED, .color_rgb=0xb596ff,
     .name="POLARITY", .category="utility", .cpu_label="~0.05% CPU"},
    {.package_id=0x10005, .effect_type=SEEDFX_EFFECT_SOFT_CLIP,
     .shape=SEEDFX_SHAPE_PILL, .parameter_count=2, .color_rgb=0xff9d5c,
     .name="SOFT CLIP", .category="drive", .cpu_label="~0.45% CPU",
     .parameters={{.name="Drive", .unit="dB", .minimum=0, .maximum=30,
                   .step=.5f, .default_value=6},
                  {.name="Mix", .unit="%", .minimum=0, .maximum=1,
                   .step=.01f, .default_value=1}}},
};
static size_t s_effect_catalog_count = 5U;
static void jack_event(lv_event_t *event);

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
static lv_obj_t *s_io_channel_switches[4];
static lv_obj_t *s_pedal_rate;
static lv_obj_t *s_node_edit_body;
static lv_obj_t *s_node_info;
static lv_obj_t *s_node_info_title;
static lv_obj_t *s_node_info_text;
static pedal_patch_t s_patch;
static pedal_colors_t s_route_colors;
static lv_obj_t *s_node_cards[SEEDFX_MAX_NODES];
static lv_obj_t *s_node_jacks[SEEDFX_MAX_NODES][2][2];
static void refresh_patch_highlights(void);
static void board_toggle_event(lv_event_t *event);
static void graph_workspace_click_event(lv_event_t *event);
static pedal_knob_t s_node_knobs[SEEDFX_MAX_PARAMS];
static unsigned s_node_knob_count;
static pedal_cable_t s_cables[SEEDFX_MAX_EDGES];
static QueueHandle_t s_graph_commands;
static StaticQueue_t s_graph_queue;
static uint8_t s_graph_queue_storage[32 * sizeof(audio_dashboard_command_t)];
static pedal_io_view_t *s_io_settings_target;
static lv_obj_t *s_graph_workspace;
static lv_obj_t *s_graph_node_layer;
static lv_obj_t *s_effect_selector;
static lv_obj_t *s_effect_selector_title;
static lv_obj_t *s_effect_list;
static lv_obj_t *s_effect_back;
static int s_effect_category=-1;
static lv_obj_t *s_node_context;
static lv_obj_t *s_node_context_title;
static lv_obj_t *s_node_edit;
static pedal_node_t s_pedal_nodes[PEDALBOARD_VISIBLE_NODES];
static uint8_t s_pedal_node_count;
static SeedFxEdgeDefinition s_ui_edges[SEEDFX_MAX_EDGES]={{0,0,1,0,0,0},{0,0,1,1,1,0}};
static uint8_t s_ui_edge_count=2;
static lv_timer_t *s_render_test_timer;
static uint8_t s_graph_target_index = UINT8_MAX;
static uint16_t s_visual_order[SEEDFX_MAX_NODES];
static uint8_t s_visual_slot[SEEDFX_MAX_NODES];
static pedal_gesture_t s_node_gesture;
static unsigned s_drag_index;
static lv_obj_t *s_drag_preview;
static lv_timer_t *s_drag_timer;
static lv_indev_t *s_drag_input;
static void cancel_node_gesture(void);

static volatile uint32_t s_pending_peak[DASHBOARD_CHANNELS];
static volatile uint32_t s_pending_local_peak[DASHBOARD_CHANNELS];
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
    COMMAND_EDIT_GRAPH = 1U << 5,
};

static void close_io_settings(void);
static void close_graph_overlays(void);

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

static bool request_graph_edit(uint8_t action, uint8_t node_index,
                               uint16_t effect_type, uint8_t parameter_index,
                               float parameter_value)
{
    const audio_dashboard_command_t command = {
        .edit_graph = true, .graph_action = action,
        .graph_node_index = node_index, .graph_effect_type = effect_type,
        .graph_parameter_index = parameter_index,
        .graph_parameter_value = parameter_value,
    };
    if (s_graph_commands == NULL || xQueueSend(s_graph_commands, &command, 0) != pdTRUE) {
        ESP_LOGE(TAG, "effect control queue full");
        return false;
    }
    return true;
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
    cancel_node_gesture();
    wifi_view_close();
    close_graph_overlays();
    lv_obj_add_flag(s_monitor_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pedalboard_page, LV_OBJ_FLAG_HIDDEN);
    wifi_view_create(s_settings_page);
    lv_obj_add_flag(s_settings_page, LV_OBJ_FLAG_HIDDEN);
    if (page == DASHBOARD_PAGE_MONITOR) {
        lv_obj_clear_flag(s_monitor_page, LV_OBJ_FLAG_HIDDEN);
    } else if (page == DASHBOARD_PAGE_PEDALBOARD) {
        lv_obj_clear_flag(s_pedalboard_page, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_settings_page, LV_OBJ_FLAG_HIDDEN);
    }
    for (unsigned i = 0; i < DASHBOARD_CHANNELS; ++i) {
        __atomic_store_n(&s_pending_peak[i], 0U, __ATOMIC_RELEASE);
        __atomic_store_n(&s_pending_local_peak[i], 0U, __ATOMIC_RELEASE);
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
    if (view == NULL || view->card == NULL) return;
    for (unsigned ch = 0; ch < 2; ++ch) {
        const bool on = view->channel_enabled[ch];
        if (on) lv_obj_clear_flag(view->channel_dot[ch], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(view->channel_dot[ch], LV_OBJ_FLAG_HIDDEN);
        if(view->level[ch]) {
            if(on) lv_obj_clear_flag(view->level[ch],LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(view->level[ch],LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (view->channel_enabled[0] || view->channel_enabled[1])
        lv_obj_clear_flag(view->card, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(view->card, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_io_settings(void)
{
    pedal_io_view_t *view = s_io_settings_target;
    if (view == NULL) {
        return;
    }
    lv_label_set_text(s_io_settings_title, "AUDIO SETTINGS");
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
    for (unsigned index = 0; index < 4; ++index) {
        const bool on = s_pedal_io[index / 2].channel_enabled[index % 2];
        if (on) lv_obj_add_state(s_io_channel_switches[index], LV_STATE_CHECKED);
        else lv_obj_clear_state(s_io_channel_switches[index], LV_STATE_CHECKED);
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
    (void)event;
    s_io_settings_target = &s_pedal_io[PEDAL_IO_INPUT];
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

static void render_pedal_nodes(void);

static void io_channel_event(lv_event_t *event)
{
    const unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (index >= 4 || s_io_settings_target == NULL) return;
    s_pedal_io[index / 2].channel_enabled[index % 2] =
        lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    request_channel_masks();
    render_pedal_nodes();
}

static int physical_jack_y(unsigned channel) { return 140 + channel*204; }

static void create_compact_io_panel(lv_obj_t *parent,
                                    pedal_io_view_t *view, int x)
{
    view->card = make_panel(parent, x, 0, 72, 466, 0x091522, 0x091522, 0);
    /* Wires terminate at the socket, not at an opaque side-panel edge. */
    lv_obj_set_style_bg_opa(view->card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->card, 0, 0);
    lv_obj_clear_flag(view->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(view->card, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    make_label(view->card, 0, 10, 72, 18, view->title,
               &lv_font_montserrat_12, 0x8fa2b6, LV_TEXT_ALIGN_CENTER);
    for (unsigned ch = 0; ch < 2; ++ch) {
        view->channel_dot[ch] = make_panel(view->card, 15, physical_jack_y(ch)-21,
                                            42, 42, 0x23443c, 0x48e0a8, 21);
        view->channel_text[ch] = make_centered_label(view->channel_dot[ch],
            0, 0, 42, 42, ch == 0 ? "1" : "2", &lv_font_montserrat_20, 0xeaf5ef);
        lv_obj_add_flag(view->channel_dot[ch], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(view->channel_dot[ch], LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_add_event_cb(view->channel_dot[ch],jack_event,LV_EVENT_SHORT_CLICKED,
            (void *)(uintptr_t)((view==&s_pedal_io[0]?2:0)|ch));
        view->level[ch]=lv_bar_create(view->card);
        lv_obj_set_pos(view->level[ch],31,ch==0?36:373);
        lv_obj_set_size(view->level[ch],10,76);
        lv_bar_set_range(view->level[ch],0,100);
        lv_obj_set_style_bg_color(view->level[ch],color(0x203145),LV_PART_MAIN);
        lv_obj_set_style_bg_opa(view->level[ch],LV_OPA_COVER,LV_PART_MAIN);
        lv_obj_set_style_bg_color(view->level[ch],color(0x48e0a8),LV_PART_INDICATOR);
        lv_obj_set_style_radius(view->level[ch],3,LV_PART_MAIN);
        lv_obj_set_style_radius(view->level[ch],3,LV_PART_INDICATOR);
        lv_obj_clear_flag(view->level[ch],LV_OBJ_FLAG_CLICKABLE);
        lv_bar_set_value(view->level[ch],(int)view->displayed[ch],LV_ANIM_OFF);
    }
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
    s_io_settings_overlay = make_panel(parent, 0, 0, 1024, 600,
                                       0x02070d, 0x02070d, 0);
    lv_obj_set_style_bg_opa(s_io_settings_overlay, 210, 0);
    lv_obj_add_flag(s_io_settings_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *sheet = make_panel(s_io_settings_overlay, 242, 106, 540, 388,
                                 0x0d1b2a, 0x304961, 18);
    s_io_settings_accent = make_panel(sheet, 0, 0, 540, 4, 0x42e8bd, 0x42e8bd, 0);
    s_io_settings_title = make_label(sheet, 28, 24, 340, 30, "AUDIO SETTINGS",
        &lv_font_montserrat_20, 0xe8eef6, LV_TEXT_ALIGN_LEFT);
    s_io_settings_owner = make_label(sheet, 28, 62, 450, 20, "",
        &lv_font_montserrat_12, 0x8ba9ff, LV_TEXT_ALIGN_LEFT);
    s_io_settings_rate = make_label(sheet, 420, 28, 92, 26, "",
        &lv_font_montserrat_20, 0xffb15f, LV_TEXT_ALIGN_RIGHT);
    for (unsigned rate = 0; rate < 4; ++rate) {
        lv_obj_t *b = lv_button_create(sheet);
        lv_obj_set_pos(b, 28 + rate * 124, 100);
        lv_obj_set_size(b, 112, 48);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        char text[20];
        snprintf(text, sizeof(text), "%s kHz", s_pedal_rate_names[rate]);
        make_centered_label(b, 0, 0, 112, 48, text, &lv_font_montserrat_14, 0xe8eef6);
        lv_obj_add_event_cb(b, io_rate_event, LV_EVENT_CLICKED, (void *)(uintptr_t)rate);
        s_io_rate_buttons[rate] = b;
    }
    for (unsigned side = 0; side < 2; ++side) {
        const int x = 28 + side * 260;
        make_label(sheet, x, 178, 220, 20, side ? "OUTPUTS" : "INPUTS",
                    &lv_font_montserrat_14, side ? 0x8ba9ff : 0x42e8bd, LV_TEXT_ALIGN_LEFT);
        for (unsigned ch = 0; ch < 2; ++ch) {
            const int y = 216 + ch * 52;
            make_centered_label(sheet, x, y, 86, 32, ch ? "CH 2" : "CH 1",
                                 &lv_font_montserrat_14, 0xe8eef6);
            lv_obj_t *toggle = lv_switch_create(sheet);
            lv_obj_set_pos(toggle, x + 148, y);
            lv_obj_set_size(toggle, 66, 32);
            lv_obj_set_style_bg_color(toggle, color(0x344456), LV_PART_MAIN);
            lv_obj_set_style_bg_color(toggle, color(0x48e0a8), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(toggle, color(0x48e0a8), LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_bg_color(toggle, color(0xf0f3f7), LV_PART_KNOB);
            lv_obj_add_event_cb(toggle, io_channel_event, LV_EVENT_VALUE_CHANGED,
                                (void *)(uintptr_t)(side * 2 + ch));
            s_io_channel_switches[side * 2 + ch] = toggle;
        }
    }
    lv_obj_t *close = lv_button_create(sheet);
    lv_obj_set_pos(close, 380, 330);
    lv_obj_set_size(close, 132, 40);
    lv_obj_set_style_pad_all(close, 0, 0);
    lv_obj_set_style_bg_color(close, color(0x203449), 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    make_centered_label(close, 0, 0, 132, 40, "DONE", &lv_font_montserrat_14, 0xe8eef6);
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
        parent, 552, 14, 240, 60, 0x0e1c2b, 0x28405a, 15);
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

static const SeedFxCatalogEntry *effect_info(uint16_t type)
{
    for (size_t index = 0; index < s_effect_catalog_count; ++index) {
        if (s_effect_catalog[index].effect_type == type)
            return &s_effect_catalog[index];
    }
    return &s_effect_catalog[0];
}

static int effect_radius(const SeedFxCatalogEntry *effect)
{
    if (effect->shape == SEEDFX_SHAPE_PILL) return 40;
    if (effect->shape == SEEDFX_SHAPE_ROUNDED) return 16;
    return 6;
}

static void close_graph_overlays(void)
{
    s_patch.active=false;
    refresh_patch_highlights();
    if (s_effect_selector != NULL)
        lv_obj_add_flag(s_effect_selector, LV_OBJ_FLAG_HIDDEN);
    if (s_node_context != NULL)
        lv_obj_add_flag(s_node_context, LV_OBJ_FLAG_HIDDEN);
    for (unsigned p = 0; p < s_node_knob_count; ++p) pedal_knob_flush(&s_node_knobs[p]);
    if (s_node_edit != NULL) lv_obj_add_flag(s_node_edit, LV_OBJ_FLAG_HIDDEN);
    if (s_node_info != NULL) lv_obj_add_flag(s_node_info, LV_OBJ_FLAG_HIDDEN);
}

static void graph_workspace_long_press_event(lv_event_t *event);

static void graph_node_click_event(lv_event_t *event);
static void graph_node_pointer_event(lv_event_t *event);
static void jack_event(lv_event_t *event);

static void ui_graph(SeedFxGraphDefinition *g)
{
    memset(g,0,sizeof(*g)); g->node_count=s_pedal_node_count; g->edge_count=s_ui_edge_count;
    for(unsigned n=0;n<s_pedal_node_count;++n) {
        g->nodes[n].id=s_pedal_nodes[n].id; g->nodes[n].effect_type=s_pedal_nodes[n].type;
    }
    memcpy(g->edges,s_ui_edges,sizeof(s_ui_edges));
}

static void ui_set_edges(const SeedFxGraphDefinition *g)
{
    s_ui_edge_count=g->edge_count; memcpy(s_ui_edges,g->edges,sizeof(s_ui_edges));
}

static void prune_ui_edges(void)
{
    SeedFxGraphDefinition g; ui_graph(&g); seedfx_prune_invalid_ports(&g); ui_set_edges(&g);
}

static void rebuild_ui_edges(void)
{
    /* Used only by the non-audio display stress test. Editing never rewires
     * other nodes or reassigns their stable IDs. */
    s_ui_edge_count = 0;
    for (uint8_t i = 0; i < s_pedal_node_count; ++i) s_pedal_nodes[i].id = i + 1;
    for (unsigned i=0;i<=s_pedal_node_count;++i) {
        const unsigned src=i?seedfx_output_channels(s_pedal_nodes[i-1].type):2;
        const unsigned dst=i<s_pedal_node_count?seedfx_input_channels(s_pedal_nodes[i].type):2;
        for(unsigned p=0;p<dst;++p)
            s_ui_edges[s_ui_edge_count++]=(SeedFxEdgeDefinition){i,i==s_pedal_node_count?0:i+1,1,
                seedfx_route_channel(src,dst,0,p),p,0};
    }
}

static pedal_layout_t s_cable_layout;
static int node_x(unsigned index) { return pedal_layout_x(&s_cable_layout,s_visual_slot[index]); }
static int node_y(unsigned index) { return pedal_layout_y(&s_cable_layout,s_visual_slot[index]); }

static void order_visual_graph(SeedFxGraphDefinition *g)
{
    SeedFxNodeDefinition ordered[SEEDFX_MAX_NODES];
    bool used[SEEDFX_MAX_NODES]={0}; unsigned count=0;
    for(unsigned slot=0;slot<SEEDFX_MAX_NODES;++slot)
        for(unsigned n=0;n<g->node_count;++n)
            if(!used[n] && s_visual_order[slot]==g->nodes[n].id) {
                ordered[count++]=g->nodes[n];used[n]=true;break;
            }
    for(unsigned n=0;n<g->node_count;++n) if(!used[n]) ordered[count++]=g->nodes[n];
    memset(s_visual_order,0,sizeof(s_visual_order));
    for(unsigned slot=0;slot<count;++slot) {
        s_visual_order[slot]=ordered[slot].id;
        for(unsigned n=0;n<g->node_count;++n)
            if(g->nodes[n].id==ordered[slot].id) s_visual_slot[n]=slot;
    }
    memcpy(g->nodes,ordered,count*sizeof(ordered[0]));
}

static void render_cables(void)
{
    SeedFxGraphDefinition graph; ui_graph(&graph);
    pedal_colors_sync(&s_route_colors,&graph);
    order_visual_graph(&graph);
    const uint8_t inputs=s_pedal_io[0].channel_enabled[0]|(s_pedal_io[0].channel_enabled[1]<<1);
    const uint8_t outputs=s_pedal_io[1].channel_enabled[0]|(s_pedal_io[1].channel_enabled[1]<<1);
    pedal_layout_build(&s_cable_layout,&graph,inputs,outputs);
    lv_obj_set_height(s_graph_node_layer,s_cable_layout.canvas_height);
    for(unsigned e=0;e<graph.edge_count;++e) {
        const pedal_route_t *route=&s_cable_layout.routes[e];
        if(!route->count) continue;
        lv_point_precise_t points[6];
        for(unsigned p=0;p<route->count;++p)
            points[p]=(lv_point_precise_t){route->points[p].x,route->points[p].y};
        const SeedFxEdgeDefinition *r=&graph.edges[e];
        const uint32_t rgb=pedal_color_for(&s_route_colors,r);
        const uint32_t end_rgb=!r->destination_id?pedal_physical_color(r->destination_port):rgb;
        pedal_cable_create(s_graph_node_layer,&s_cables[e],points,route->count,rgb,end_rgb);
    }
}
static void render_pedal_nodes(void)
{
    if (s_graph_node_layer == NULL) return;
    cancel_node_gesture();
    lv_obj_clean(s_graph_node_layer);
    memset(s_node_cards,0,sizeof(s_node_cards));
    memset(s_node_jacks,0,sizeof(s_node_jacks));
    render_cables();
    create_compact_io_panel(s_graph_node_layer, &s_pedal_io[0], 0);
    create_compact_io_panel(s_graph_node_layer, &s_pedal_io[1], 952);
    if (s_pedal_node_count == 0) {
        make_label(s_graph_node_layer, 342, 224, 340, 30, "YOUR PEDALBOARD",
                    &lv_font_montserrat_20, 0xc8d4e0, LV_TEXT_ALIGN_CENTER);
        make_label(s_graph_node_layer, 342, 264, 340, 20, "Hold empty space to add an effect",
                    &lv_font_montserrat_14, 0x7e93a9, LV_TEXT_ALIGN_CENTER);
    }
    for (unsigned i = 0; i < s_pedal_node_count; ++i) {
        const SeedFxCatalogEntry *fx = effect_info(s_pedal_nodes[i].type);
        const int width=pedal_layout_node_width(fx->effect_type);
        const bool forward=pedal_layout_forward(&s_cable_layout,s_visual_slot[i]);
        lv_obj_t *node = make_panel(s_graph_node_layer, node_x(i), node_y(i),
                                     width, 210, 0x142333, fx->color_rgb, effect_radius(fx));
        s_node_cards[i]=node;
        lv_obj_set_style_border_width(node, 2, 0);
        lv_obj_add_flag(node, LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_add_flag(node, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_add_event_cb(node, graph_node_pointer_event, LV_EVENT_ALL,
                            (void *)(uintptr_t)(i + 1));
        lv_obj_t *led = make_panel(node, (width-42)/2, 14, 42, 42,
            s_pedal_nodes[i].enabled ? 0x48e0a8 : 0xf05b68,
            s_pedal_nodes[i].enabled ? 0x48e0a8 : 0xf05b68, 21);
        lv_obj_add_flag(led, LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_add_event_cb(led,board_toggle_event,LV_EVENT_SHORT_CLICKED,(void *)(uintptr_t)i);
        lv_obj_t *name = make_label(node, 6, 70, width-12, 42, fx->name,
            strlen(fx->name) >= 10 ? &lv_font_montserrat_12 : &lv_font_montserrat_14,
            0xeaf0f6, LV_TEXT_ALIGN_CENTER);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        for (unsigned side = 0; side < 2; ++side) {
            const bool output_side = forward ? side != 0 : side == 0;
            const unsigned ports = output_side ? seedfx_output_channels(fx->effect_type)
                                               : seedfx_input_channels(fx->effect_type);
            for (unsigned ch = 0; ch < ports; ++ch) {
                const int center = ports == 1 ? 172 : 138 + ch * 46;
                lv_obj_t *jack = make_panel(node, side ? width-42 : 0, center-21,
                    40, 42, 0x203346, 0x708090, 10);
                lv_obj_add_flag(jack, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
                s_node_jacks[i][output_side?1:0][ch]=jack;
                lv_obj_add_flag(jack, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(jack,jack_event,LV_EVENT_SHORT_CLICKED,
                    (void *)(uintptr_t)((s_pedal_nodes[i].id<<3)|(output_side?2:0)|ch));
                char port_text[8];
                snprintf(port_text,sizeof(port_text),forward?"%u >":"< %u",ch+1);
                make_centered_label(jack,0,0,40,42,port_text,&lv_font_montserrat_12,0xe5edf5);

            }
        }
    }
    refresh_patch_highlights();

#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "board nodes=%u heap=%u largest=%u internal=%u stack=%u",
        (unsigned)s_pedal_node_count,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
}

static struct {
    pedal_node_t nodes[PEDALBOARD_VISIBLE_NODES];
    SeedFxEdgeDefinition edges[SEEDFX_MAX_EDGES];
    uint8_t node_count, edge_count;
    dashboard_page_t page;
    unsigned step;
    lv_obj_t *shield;
} s_render_test;

static void render_test_tick(lv_timer_t *timer)
{
    if (s_render_test.step == 240) {
        memcpy(s_pedal_nodes, s_render_test.nodes, sizeof(s_pedal_nodes));
        memcpy(s_ui_edges, s_render_test.edges, sizeof(s_ui_edges));
        s_pedal_node_count = s_render_test.node_count;
        s_ui_edge_count = s_render_test.edge_count;
        lv_obj_delete(s_render_test.shield);
        render_pedal_nodes();
        set_active_page(s_render_test.page);
        lv_timer_delete(timer); s_render_test_timer = NULL;
        ESP_LOGI(TAG, "board stress PASS: 10 cycles 0..12..0; original board restored; DSP unchanged");
        return;
    }
    const unsigned step = s_render_test.step++ % 24;
    s_pedal_node_count = step < 12 ? step + 1 : 23 - step;
    for (unsigned i = 0; i < s_pedal_node_count; ++i) {
        const SeedFxCatalogEntry *fx = &s_effect_catalog[i % s_effect_catalog_count];
        s_pedal_nodes[i] = (pedal_node_t){.type=fx->effect_type, .enabled=true};
    }
    rebuild_ui_edges(); render_pedal_nodes();
}

esp_err_t audio_dashboard_test_pedalboard(void)
{
    if (!bsp_display_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    if (s_render_test_timer || !s_graph_node_layer) {
        bsp_display_unlock(); return ESP_FAIL;
    }
    memcpy(s_render_test.nodes, s_pedal_nodes, sizeof(s_pedal_nodes));
    memcpy(s_render_test.edges, s_ui_edges, sizeof(s_ui_edges));
    s_render_test.node_count = s_pedal_node_count;
    s_render_test.edge_count = s_ui_edge_count;
    s_render_test.page = s_active_page;
    s_render_test.step = 0;
    close_graph_overlays(); close_io_settings(); close_window_menu();
    set_active_page(DASHBOARD_PAGE_PEDALBOARD);
    /* Block accidental touch edits during the temporary preview. */
    s_render_test.shield = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_render_test.shield);
    lv_obj_set_size(s_render_test.shield, 1024, 600);
    lv_obj_add_flag(s_render_test.shield, LV_OBJ_FLAG_CLICKABLE);
    make_label(s_render_test.shield, 320, 72, 400, 22, "DISPLAY TEST - AUDIO UNCHANGED",
        &lv_font_montserrat_14, 0xffbe69, LV_TEXT_ALIGN_CENTER);
    s_render_test_timer = lv_timer_create(render_test_tick, 200, NULL);
    bsp_display_unlock();
    return ESP_OK;
}

static lv_obj_t *make_modal_button(lv_obj_t *,int,int,int,int,const char *,uint32_t);
static void effect_selector_event(lv_event_t *event);
static void refresh_effect_selector(void);
static void category_event(lv_event_t *event)
{
    s_effect_category=(int)(intptr_t)lv_event_get_user_data(event);
    refresh_effect_selector();
}
static void refresh_effect_selector(void)
{
    if(!s_effect_list) return;
    lv_obj_clean(s_effect_list);
    lv_obj_scroll_to_y(s_effect_list,0,LV_ANIM_OFF);
    lv_label_set_text(s_effect_selector_title,s_effect_category<0?
        (s_graph_target_index==UINT8_MAX?"ADD EFFECT":"REPLACE EFFECT"):
        pedal_categories[s_effect_category]);
    if(s_effect_category<0) lv_obj_add_flag(s_effect_back,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(s_effect_back,LV_OBJ_FLAG_HIDDEN);
    unsigned row=0;
    if(s_effect_category<0) {
        for(unsigned cat=0;cat<PEDAL_CATEGORY_COUNT;++cat) {
            unsigned count=0;
            for(unsigned i=0;i<s_effect_catalog_count;++i)
                count+=pedal_category(s_effect_catalog[i].effect_type)==cat;
            if(!count && cat!=PEDAL_CATEGORY_COUNT-1) continue;
            char text[64];snprintf(text,sizeof(text),"%s   (%u)  >",pedal_categories[cat],count);
            lv_obj_t *button=make_modal_button(s_effect_list,6,6+row++*58,500,48,text,0x617d99);
            lv_obj_add_event_cb(button,category_event,LV_EVENT_CLICKED,(void *)(intptr_t)cat);
        }
    } else {
        for(unsigned i=0;i<s_effect_catalog_count;++i) {
            const SeedFxCatalogEntry *fx=&s_effect_catalog[i];
            if(pedal_category(fx->effect_type)!=(unsigned)s_effect_category) continue;
            lv_obj_t *button=make_modal_button(s_effect_list,6,6+row++*58,500,48,"",fx->color_rgb);
            make_label(button,18,13,290,22,fx->name,&lv_font_montserrat_14,0xf0f5fa,LV_TEXT_ALIGN_LEFT);
            make_label(button,330,14,142,20,fx->cpu_label,&lv_font_montserrat_12,fx->color_rgb,LV_TEXT_ALIGN_RIGHT);
            lv_obj_add_event_cb(button,effect_selector_event,LV_EVENT_CLICKED,(void *)(uintptr_t)i);
        }
        if(!row) make_label(s_effect_list,24,30,450,40,"No effects in this category",
                            &lv_font_montserrat_16,0x8ea2b7,LV_TEXT_ALIGN_CENTER);
    }
}
static void open_effect_selector(uint8_t replace_index)
{
    if (s_effect_selector == NULL) return;
    s_graph_target_index = replace_index;
    s_effect_category=-1;
    refresh_effect_selector();
    lv_obj_clear_flag(s_effect_selector, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_effect_selector);
}

static void graph_workspace_long_press_event(lv_event_t *event)
{
    (void)event;
    if (s_pedal_node_count < PEDALBOARD_VISIBLE_NODES)
        open_effect_selector(UINT8_MAX);
}

static void cancel_node_gesture(void)
{
    if(s_drag_timer) {lv_timer_delete(s_drag_timer);s_drag_timer=NULL;}
    if(s_drag_preview) {lv_obj_delete(s_drag_preview);s_drag_preview=NULL;}
    s_node_gesture.active=false;s_drag_input=NULL;
    if(s_graph_workspace) lv_obj_add_flag(s_graph_workspace,LV_OBJ_FLAG_SCROLLABLE);
}
static void drag_tick(lv_timer_t *timer)
{
    (void)timer;
    if(!s_drag_input || !s_node_gesture.active || s_drag_index>=s_pedal_node_count) return;
    lv_point_t point;lv_indev_get_point(s_drag_input,&point);
    const unsigned action=pedal_gesture_update(&s_node_gesture,lv_tick_get(),point.x,point.y);
    if(!s_node_gesture.cancelled && lv_tick_get()-s_node_gesture.started>=1000)
        lv_obj_remove_flag(s_graph_workspace,LV_OBJ_FLAG_SCROLLABLE);
    if(action==PEDAL_GESTURE_DRAG) {
        if(!s_drag_preview) {
            const SeedFxCatalogEntry *fx=effect_info(s_pedal_nodes[s_drag_index].type);
            const int width=pedal_layout_node_width(fx->effect_type);
            s_drag_preview=make_panel(lv_layer_top(),0,0,width,210,0x263950,0xffffff,12);
            lv_obj_set_style_bg_opa(s_drag_preview,220,0);
            make_label(s_drag_preview,8,80,width-16,50,fx->name,&lv_font_montserrat_14,0xffffff,LV_TEXT_ALIGN_CENTER);
            lv_obj_remove_flag(s_drag_preview,LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_set_pos(s_drag_preview,point.x-lv_obj_get_width(s_drag_preview)/2,point.y-105);
    } else if(action==PEDAL_GESTURE_MENU) {
        s_graph_target_index=s_drag_index;
        lv_label_set_text(s_node_context_title,effect_info(s_pedal_nodes[s_drag_index].type)->name);
        lv_obj_clear_flag(s_node_context,LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_node_context);
    }
}
static void graph_node_pointer_event(lv_event_t *event)
{
    if(lv_event_get_target(event)!=lv_event_get_current_target(event)) return;
    const unsigned index=(uintptr_t)lv_event_get_user_data(event)-1;
    if(index>=s_pedal_node_count) return;
    const lv_event_code_t code=lv_event_get_code(event);
    if(code==LV_EVENT_PRESSED) {
        cancel_node_gesture();
        s_drag_input=lv_indev_active();if(!s_drag_input) return;
        lv_point_t point;lv_indev_get_point(s_drag_input,&point);
        lv_area_t area;lv_obj_get_coords(s_node_cards[index],&area);
        s_drag_index=index;pedal_gesture_start(&s_node_gesture,lv_tick_get(),area.x1,area.y1,area.x2,area.y2);
        /* Finger motion within the card remains a hold, not a scroll. Scroll
         * the empty board instead; keep the original card bounds stable. */
        lv_obj_remove_flag(s_graph_workspace,LV_OBJ_FLAG_SCROLLABLE);
        s_drag_timer=lv_timer_create(drag_tick,25,NULL);
    } else if(code==LV_EVENT_RELEASED && s_node_gesture.active && index==s_drag_index) {
        const bool click=!s_node_gesture.dragging && !s_node_gesture.menu && !s_node_gesture.cancelled
            && lv_tick_get()-s_node_gesture.started<500;
        bool changed=false;
        if(s_node_gesture.dragging && s_drag_input) {
            lv_point_t point;lv_indev_get_point(s_drag_input,&point);
            lv_area_t area;lv_obj_get_coords(s_graph_workspace,&area);
            if(point.x>=area.x1 && point.x<=area.x2 && point.y>=area.y1 && point.y<=area.y2) {
                unsigned nearest=index;int best=INT32_MAX;
                for(unsigned n=0;n<s_pedal_node_count;++n) {
                    lv_area_t card;lv_obj_get_coords(s_node_cards[n],&card);
                    int dx=point.x-(card.x1+card.x2)/2,dy=point.y-(card.y1+card.y2)/2;
                    int distance=dx*dx+dy*dy;
                    if(distance<best) {best=distance;nearest=n;}
                }
                if(nearest!=index) {
                    const unsigned a=s_visual_slot[index],b=s_visual_slot[nearest];
                    uint16_t id=s_visual_order[a];s_visual_order[a]=s_visual_order[b];s_visual_order[b]=id;
                    changed=true;
                }
            }
        }
        cancel_node_gesture();
        if(changed) render_pedal_nodes();
        else if(click) graph_node_click_event(event);
    } else if(code==LV_EVENT_PRESS_LOST && index==s_drag_index) cancel_node_gesture();
}

static void effect_selector_event(lv_event_t *event)
{
    const size_t catalog_index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (catalog_index >= s_effect_catalog_count)
        return;
    const SeedFxCatalogEntry *effect = &s_effect_catalog[catalog_index];
    const uint16_t type = effect->effect_type;
    if (s_graph_target_index == UINT8_MAX) {
        if (s_pedal_node_count >= PEDALBOARD_VISIBLE_NODES) return;
        if (!request_graph_edit(AUDIO_DASHBOARD_GRAPH_ADD,
                                s_pedal_node_count, type, 0U, 0.0f)) return;
        pedal_node_t *node = &s_pedal_nodes[s_pedal_node_count];
        SeedFxGraphDefinition g; ui_graph(&g);
        memset(node, 0, sizeof(*node));
        node->id=seedfx_next_node_id(&g);
        node->type = type;
        node->enabled = true;
        node->source_channel = !s_pedal_io[0].channel_enabled[0] && s_pedal_io[0].channel_enabled[1];
        for (uint8_t p = 0; p < effect->parameter_count; ++p)
            node->parameters[p] = effect->parameters[p].default_value;
        ++s_pedal_node_count;
    } else if (s_graph_target_index < s_pedal_node_count) {
        if (!request_graph_edit(AUDIO_DASHBOARD_GRAPH_REPLACE,
                                s_graph_target_index, type, 0U, 0.0f)) return;
        pedal_node_t *node = &s_pedal_nodes[s_graph_target_index];
        const uint16_t id = node->id;
        memset(node, 0, sizeof(*node));
        node->id = id;
        node->type = type;
        node->enabled = true;
        node->source_channel = !s_pedal_io[0].channel_enabled[0] && s_pedal_io[0].channel_enabled[1];
        for (uint8_t p = 0; p < effect->parameter_count; ++p)
            node->parameters[p] = effect->parameters[p].default_value;
    }
    prune_ui_edges();
    close_graph_overlays();
    render_pedal_nodes();
}

static void graph_overlay_close_event(lv_event_t *event)
{
    (void)event;
    close_graph_overlays();
}

static void knob_changed(uint8_t parameter, float value, void *user)
{
    (void)user;
    if (s_graph_target_index >= s_pedal_node_count) return;
    pedal_node_t *node = &s_pedal_nodes[s_graph_target_index];
    if (parameter >= SEEDFX_MAX_PARAMS) return;
    if (request_graph_edit(AUDIO_DASHBOARD_GRAPH_SET_PARAMETER,
                           s_graph_target_index, node->type, parameter, value))
        node->parameters[parameter] = value;
}

static lv_obj_t *make_modal_button(lv_obj_t *parent, int x, int y,
                                   int width, int height, const char *text,
                                   uint32_t accent);
static void graph_overlay_close_event(lv_event_t *event);

static void editor_switch_event(lv_event_t *event)
{
    if (s_graph_target_index >= s_pedal_node_count) return;
    pedal_node_t *node = &s_pedal_nodes[s_graph_target_index];
    if (!request_graph_edit(AUDIO_DASHBOARD_GRAPH_TOGGLE,
                            s_graph_target_index, node->type, 0, 0)) return;
    node->enabled = !node->enabled;
    lv_obj_t *b = lv_event_get_target_obj(event);
    lv_obj_set_style_bg_color(b, color(node->enabled ? 0x48e0a8 : 0xe96872), 0);
    render_pedal_nodes();
}

static void refresh_node_edit(void)
{
    if (s_graph_target_index >= s_pedal_node_count) return;
    for (unsigned p = 0; p < s_node_knob_count; ++p) pedal_knob_flush(&s_node_knobs[p]);
    s_node_knob_count = 0;
    lv_obj_clean(s_node_edit_body);
    const pedal_node_t *node = &s_pedal_nodes[s_graph_target_index];
    const SeedFxCatalogEntry *fx = effect_info(node->type);
    const bool wide=pedal_layout_node_width(node->type)>104;
    const int width=wide?680:392,height=wide?440:560;
    lv_obj_set_size(s_node_edit_body,width,height);
    lv_obj_set_pos(s_node_edit_body,(1024-width)/2,(600-height)/2);
    lv_obj_set_style_border_color(s_node_edit_body, color(fx->color_rgb), 0);
    lv_obj_set_style_border_width(s_node_edit_body, 3, 0);
    lv_obj_t *title = make_label(s_node_edit_body, 26, 24, width-52, 48, fx->name,
                        &lv_font_montserrat_20, fx->color_rgb, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    const unsigned count = fx->parameter_count;
    for (unsigned p = 0; p < count; ++p) {
        const int x = wide ? (width-count*160)/2+p*160 : (count == 1 ? 126 : 28 + (p % 2) * 196);
        const int y = wide ? 110 : (count <= 2 ? 164 : 82 + (p / 2) * 158);
        pedal_knob_create(&s_node_knobs[p], s_node_edit_body, x, y,
            &fx->parameters[p], node->parameters[p], fx->color_rgb, p, knob_changed, NULL);
        ++s_node_knob_count;
    }
    if (!count) make_label(s_node_edit_body, 30, 216, 332, 72,
        "This utility has no adjustable parameters. Tap its light to toggle it.",
        &lv_font_montserrat_16, 0xa8bacb, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *led=make_panel(s_node_edit_body,(width-42)/2,height-132,42,42,
        node->enabled?0x48e0a8:0xe96872,0x8195a8,21);
    lv_obj_add_flag(led,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(led,editor_switch_event,LV_EVENT_CLICKED,NULL);
    lv_obj_t *done = make_modal_button(s_node_edit_body, (width-184)/2, height-64, 184, 40,
                                       "DONE", 0x42576b);
    lv_obj_add_event_cb(done, graph_overlay_close_event, LV_EVENT_CLICKED, NULL);
}

static void graph_node_click_event(lv_event_t *event)
{
    const uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index == 0 || index > s_pedal_node_count) return;
    close_graph_overlays();
    s_graph_target_index = index - 1;
    refresh_node_edit();
    lv_obj_clear_flag(s_node_edit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_node_edit);
}

static void show_node_info(void)
{
    const SeedFxCatalogEntry *fx = effect_info(s_pedal_nodes[s_graph_target_index].type);
    lv_label_set_text(s_node_info_title, fx->name);
    const char *description = "Stereo signal processing.";
    switch (fx->effect_type) {
        case SEEDFX_EFFECT_SPLITTER:
            description = "One input, two identical outputs. Send each output to a separate branch. No hidden level change."; break;
        case SEEDFX_EFFECT_MIXER:
            description = "Two independent inputs summed to one output. Branch A, Branch B and Master set levels. Default 50% + 50% preserves the level of identical signals. Bypass passes A only."; break;
        case SEEDFX_EFFECT_DELAY: case SEEDFX_EFFECT_SLAPBACK:
            description = "Repeats the sound after a delay. Tone softens the repeats; Feedback controls their number."; break;
        case SEEDFX_EFFECT_PING_PONG_DELAY:
            description = "Echoes alternate between the left and right channels. Time, Feedback, Mix and Tone shape the repeats."; break;
        case SEEDFX_EFFECT_REVERB:
            description = "Adds a diffuse stereo tail. Size shapes the space; Damping darkens it; Decay controls the feedback."; break;
        case SEEDFX_EFFECT_COMPRESSOR:
            description = "Reduces level above Threshold. Ratio sets the strength; Attack and Release set the response time."; break;
        case SEEDFX_EFFECT_LIMITER:
            description = "Controls peaks above Ceiling. Input adjusts the level into the limiter; Release controls recovery."; break;
        case SEEDFX_EFFECT_NOISE_GATE:
            description = "Attenuates quiet signals below Threshold. Attack and Release shape opening and closing."; break;
        case SEEDFX_EFFECT_CHORUS: case SEEDFX_EFFECT_FLANGER: case SEEDFX_EFFECT_VIBRATO:
            description = "A moving short delay adds modulation. Rate sets speed and Depth sets the amount of movement."; break;
        case SEEDFX_EFFECT_GAIN: case SEEDFX_EFFECT_STEREO_WIDENER:
            description = "Adjusts the stereo signal. Balance attenuates one side without swapping the channel connections."; break;
        default:
            if (!strcmp(fx->category, "drive")) description = "Adds nonlinear distortion. Adjust the available drive, tone, level and blend controls to shape the sound.";
            else if (!strcmp(fx->category, "filter")) description = "Shapes the frequency spectrum. Higher resonance emphasizes the region near the selected frequency.";
            else if (!strcmp(fx->category, "modulation")) description = "Modulates the signal over time. Rate sets the speed; the other controls shape depth and blend.";
            else if (!strcmp(fx->category, "utility")) description = "A utility for controlling level, polarity or signal flow in the pedalboard.";
            break;
    }
    char text[640];
    snprintf(text, sizeof(text),
        "%s\n\nCategory: %s  |  Controls: %u\n"
        "Seed3 load: %s at 48 kHz (estimate)\n\n"
        "Audio jacks: %u IN / %u OUT\n"
        "Mono pedals use one selected source channel.\n\n"
        "Actual total load is shown in the top bar.",
        description, fx->category, (unsigned)fx->parameter_count,
        fx->cpu_label[0] ? fx->cpu_label : "not measured",
        seedfx_input_channels(fx->effect_type), seedfx_output_channels(fx->effect_type));
    lv_label_set_text(s_node_info_text, text);
    lv_obj_clear_flag(s_node_info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_node_info);
}

static uint32_t jack_color(pedal_jack_t jack)
{
    if(!jack.id) return pedal_physical_color(jack.port);
    for(unsigned e=0;e<s_ui_edge_count;++e) {
        const SeedFxEdgeDefinition *r=&s_ui_edges[e];
        if(jack.output ? r->source_id==jack.id && r->source_port==jack.port
                       : r->destination_id==jack.id && r->destination_port==jack.port)
            return pedal_color_for(&s_route_colors,r);
    }
    return 0x708090;
}

static void style_jack(lv_obj_t *object,pedal_jack_t jack)
{
    if(!object) return;
    const bool selected=pedal_patch_is_selected(&s_patch,jack);
    lv_obj_set_style_border_color(object,color(selected?0xffffff:jack_color(jack)),0);
    lv_obj_set_style_border_width(object,selected?3:2,0);
    lv_obj_set_style_bg_color(object,color(selected?0x516078:0x203346),0);
}

static void refresh_patch_highlights(void)
{
    for(unsigned side=0;side<2;++side) for(unsigned ch=0;ch<2;++ch)
        style_jack(s_pedal_io[side].channel_dot[ch],(pedal_jack_t){0,ch,side==0});
    for(unsigned n=0;n<s_pedal_node_count;++n) {
        if(!s_node_cards[n]) continue;
        const bool selected=s_patch.active && s_patch.jack.id==s_pedal_nodes[n].id;
        const SeedFxCatalogEntry *fx=effect_info(s_pedal_nodes[n].type);
        lv_obj_set_style_border_color(s_node_cards[n],color(selected?0xffffff:fx->color_rgb),0);
        lv_obj_set_style_border_width(s_node_cards[n],selected?3:2,0);
        lv_obj_set_style_bg_color(s_node_cards[n],color(selected?0x263950:0x142333),0);
        for(unsigned out=0;out<2;++out) for(unsigned ch=0;ch<2;++ch)
            style_jack(s_node_jacks[n][out][ch],(pedal_jack_t){s_pedal_nodes[n].id,ch,out!=0});
    }
}

static bool apply_patch_edit(pedal_patch_result_t result,const SeedFxEdgeDefinition *r)
{
    if(result!=PEDAL_PATCH_CONNECT && result!=PEDAL_PATCH_DISCONNECT) {
        refresh_patch_highlights(); return false;
    }
    audio_dashboard_command_t c={.edit_graph=true,
        .graph_action=result==PEDAL_PATCH_CONNECT?AUDIO_DASHBOARD_GRAPH_CONNECT:AUDIO_DASHBOARD_GRAPH_DISCONNECT,
        .graph_source_id=r->source_id,.graph_source_port=r->source_port,
        .graph_destination_id=r->destination_id,.graph_destination_port=r->destination_port};
    if(!s_graph_commands || xQueueSend(s_graph_commands,&c,0)!=pdTRUE) {
        refresh_patch_highlights(); return false;
    }
    SeedFxGraphDefinition g; ui_graph(&g);
    if(result==PEDAL_PATCH_CONNECT)
        (void)seedfx_connect(&g,r->source_id,r->source_port,r->destination_id,r->destination_port);
    else seedfx_disconnect_input(&g,r->destination_id,r->destination_port);
    ui_set_edges(&g); render_pedal_nodes(); return true;
}

static void jack_event(lv_event_t *event)
{
    const uintptr_t encoded=(uintptr_t)lv_event_get_user_data(event);
    const pedal_jack_t jack={(uint16_t)(encoded>>3),encoded&1,(encoded&2)!=0};
    if(!jack.id && !s_pedal_io[jack.output?0:1].channel_enabled[jack.port]) return;
    SeedFxGraphDefinition g; ui_graph(&g);
    SeedFxEdgeDefinition edit={0};
    const pedal_patch_result_t result=pedal_patch_tap(&s_patch,&g,jack,&edit);
    (void)apply_patch_edit(result,&edit);
}

static void graph_workspace_click_event(lv_event_t *event)
{
    if(lv_event_get_target(event)!=s_graph_node_layer) return;
    SeedFxGraphDefinition g; ui_graph(&g); SeedFxEdgeDefinition edit={0};
    (void)apply_patch_edit(pedal_patch_blank(&s_patch,&g,&edit),&edit);
}

static void board_toggle_event(lv_event_t *event)
{
    const unsigned n=(uintptr_t)lv_event_get_user_data(event);
    if(n>=s_pedal_node_count) return;
    if(!request_graph_edit(AUDIO_DASHBOARD_GRAPH_TOGGLE,n,s_pedal_nodes[n].type,0,0)) return;
    s_patch.active=false; s_pedal_nodes[n].enabled=!s_pedal_nodes[n].enabled;
    render_pedal_nodes();
}

static void overlay_outside_event(lv_event_t *event)
{
    if(lv_event_get_target(event)==lv_event_get_current_target(event)) close_graph_overlays();
}

static void node_context_action_event(lv_event_t *event)
{
    const uint8_t action = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (s_graph_target_index >= s_pedal_node_count) return;
    if (action == 100) {
        lv_obj_add_flag(s_node_context, LV_OBJ_FLAG_HIDDEN);
        show_node_info();

    } else if (action == AUDIO_DASHBOARD_GRAPH_DELETE) {
        if (!request_graph_edit(action, s_graph_target_index, 0U, 0U, 0.0f)) return;
        memmove(&s_pedal_nodes[s_graph_target_index],
                &s_pedal_nodes[s_graph_target_index + 1U],
                (s_pedal_node_count - s_graph_target_index - 1U)
                    * sizeof(s_pedal_nodes[0]));
        --s_pedal_node_count;
        prune_ui_edges();
        close_graph_overlays();
        render_pedal_nodes();
    } else if (action == AUDIO_DASHBOARD_GRAPH_REPLACE) {
        lv_obj_add_flag(s_node_context, LV_OBJ_FLAG_HIDDEN);
        open_effect_selector(s_graph_target_index);
    } else if (action == AUDIO_DASHBOARD_GRAPH_TOGGLE) {
        if (!request_graph_edit(action, s_graph_target_index,
                           s_pedal_nodes[s_graph_target_index].type,
                           0U, 0.0f)) return;
        s_pedal_nodes[s_graph_target_index].enabled =
            !s_pedal_nodes[s_graph_target_index].enabled;
        close_graph_overlays();
        render_pedal_nodes();
    } else if (action == AUDIO_DASHBOARD_GRAPH_SET_PARAMETER) {
        lv_obj_add_flag(s_node_context, LV_OBJ_FLAG_HIDDEN);
        refresh_node_edit();
        lv_obj_clear_flag(s_node_edit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_node_edit);
    }
}

static lv_obj_t *make_modal_button(lv_obj_t *parent,
                                   int x,
                                   int y,
                                   int width,
                                   int height,
                                   const char *text,
                                   uint32_t accent)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, color(0x152638), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, color(accent), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    make_centered_label(button, 0, 0, width, height, text,
                        &lv_font_montserrat_14, 0xe5edf5);
    return button;
}

static void create_graph_overlays(lv_obj_t *parent)
{
    s_effect_selector = make_panel(parent, 0, 0, DASHBOARD_WIDTH,
                                   DASHBOARD_HEIGHT, 0x02070d, 0x02070d, 0);
    lv_obj_set_style_bg_opa(s_effect_selector, 210, LV_PART_MAIN);
    lv_obj_add_flag(s_effect_selector, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *selector = make_panel(s_effect_selector, 232, 78, 560, 444,
                                    0x0c1928, 0x304961, 18);
    s_effect_selector_title = make_label(
        selector, 30, 22, 350, 30, "ADD EFFECT",
        &lv_font_montserrat_20, 0xf2f6fb, LV_TEXT_ALIGN_LEFT);
    make_label(selector, 30, 54, 450, 18,
               "ESTIMATED SEED3 LOAD AT 48 kHz",
               &lv_font_montserrat_12, 0x748ba2, LV_TEXT_ALIGN_LEFT);
    s_effect_list = make_panel(selector, 24, 80, 512, 294,
                                0x091522, 0x1e3348, 10);
    lv_obj_add_flag(s_effect_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_effect_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_effect_list, LV_SCROLLBAR_MODE_AUTO);
    s_effect_back=make_modal_button(selector,30,392,120,34,"< BACK",0x465c72);
    lv_obj_add_event_cb(s_effect_back,category_event,LV_EVENT_CLICKED,(void *)(intptr_t)-1);
    refresh_effect_selector();
    lv_obj_t *selector_close = make_modal_button(
        selector, 410, 392, 120, 34, "CLOSE", 0x465c72);
    lv_obj_add_event_cb(selector_close, graph_overlay_close_event,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_effect_selector, LV_OBJ_FLAG_HIDDEN);

    s_node_context = make_panel(parent, 0, 0, DASHBOARD_WIDTH,
                                DASHBOARD_HEIGHT, 0x02070d, 0x02070d, 0);
    lv_obj_set_style_bg_opa(s_node_context, 210, LV_PART_MAIN);
    lv_obj_t *context = make_panel(s_node_context, 322, 154, 380, 292,
                                   0x0c1928, 0x304961, 18);
    s_node_context_title = make_label(context, 28, 22, 260, 28, "NODE",
                                      &lv_font_montserrat_20, 0xf2f6fb,
                                      LV_TEXT_ALIGN_LEFT);
    struct { const char *name; uint8_t action; uint32_t color; } actions[] = {
        {"EDIT", AUDIO_DASHBOARD_GRAPH_SET_PARAMETER, 0x42e8bd},
        {"REPLACE", AUDIO_DASHBOARD_GRAPH_REPLACE, 0xffa85c},
        {"INFORMATION", 100, 0x8ba9ff},
        {"DELETE", AUDIO_DASHBOARD_GRAPH_DELETE, 0xf05b68},
    };
    for (size_t index = 0; index < sizeof(actions)/sizeof(actions[0]); ++index) {
        lv_obj_t *button = make_modal_button(
            context, 28 + (int)(index % 2U) * 166,
            72 + (int)(index / 2U) * 64, 150, 48,
            actions[index].name, actions[index].color);
        lv_obj_add_event_cb(button, node_context_action_event,
                            LV_EVENT_CLICKED,
                            (void *)(uintptr_t)actions[index].action);
    }
    lv_obj_t *context_close = make_modal_button(
        context, 226, 230, 120, 40, "CLOSE", 0x465c72);
    lv_obj_add_event_cb(context_close, graph_overlay_close_event,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_node_context, LV_OBJ_FLAG_HIDDEN);

    s_node_edit = make_panel(parent, 0, 0, DASHBOARD_WIDTH,
                             DASHBOARD_HEIGHT, 0x02070d, 0x02070d, 0);
    lv_obj_set_style_bg_opa(s_node_edit, 210, LV_PART_MAIN);
    s_node_edit_body = make_panel(s_node_edit, 316, 20, 392, 560,
                                  0x142333, 0x304961, 24);
    lv_obj_add_flag(s_node_edit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_node_edit, LV_OBJ_FLAG_HIDDEN);
    s_node_info = make_panel(parent, 0, 0, 1024, 600, 0x02070d, 0x02070d, 0);
    lv_obj_set_style_bg_opa(s_node_info, 210, 0);
    lv_obj_add_flag(s_node_info, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *info = make_panel(s_node_info, 242, 70, 540, 460, 0x0d1b2a, 0x304961, 18);
    s_node_info_title = make_label(info, 28, 24, 484, 32, "INFORMATION",
        &lv_font_montserrat_20, 0x8ba9ff, LV_TEXT_ALIGN_LEFT);
    s_node_info_text = make_label(info, 28, 82, 484, 300, "",
        &lv_font_montserrat_14, 0xd3dfeb, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(s_node_info_text, LV_LABEL_LONG_WRAP);
    lv_obj_t *info_close = make_modal_button(info, 380, 398, 132, 40, "CLOSE", 0x42576b);
    lv_obj_add_event_cb(info_close, graph_overlay_close_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_node_info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_node_edit,overlay_outside_event,LV_EVENT_SHORT_CLICKED,NULL);
    lv_obj_add_event_cb(s_node_info,overlay_outside_event,LV_EVENT_SHORT_CLICKED,NULL);
    lv_obj_add_event_cb(s_node_context,overlay_outside_event,LV_EVENT_SHORT_CLICKED,NULL);
    lv_obj_add_event_cb(s_effect_selector,overlay_outside_event,LV_EVENT_SHORT_CLICKED,NULL);
}



static void create_pedalboard(lv_obj_t *screen)
{
    s_pedalboard_page = make_panel(screen, 0, 0, 1024, 600, 0x07111f, 0x07111f, 0);
    make_label(s_pedalboard_page, 24, 20, 260, 36, "PEDALBOARD",
                &lv_font_montserrat_28, 0xeaf0f7, LV_TEXT_ALIGN_LEFT);
    s_pedal_rate = make_label(s_pedalboard_page, 26, 60, 180, 22, "-- kHz",
                &lv_font_montserrat_14, 0xffb15f, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *settings = make_modal_button(s_pedalboard_page, 320, 22, 208, 48,
                                           "AUDIO SETTINGS", 0x42e8bd);
    lv_obj_add_event_cb(settings, io_panel_long_press_event, LV_EVENT_LONG_PRESSED, NULL);
    create_pedalboard_control(s_pedalboard_page);
    create_pedal_cpu_panel(s_pedalboard_page);
    s_graph_workspace = make_panel(s_pedalboard_page, 0, 104, 1024, 490,
                                   0x091522, 0x091522, 0);
    lv_obj_add_flag(s_graph_workspace,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_graph_workspace,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_graph_workspace,LV_SCROLLBAR_MODE_AUTO);
    s_graph_node_layer = make_panel(s_graph_workspace, 0, 0, 1024, 490,
                                    0x091522, 0x091522, 0);
    lv_obj_add_flag(s_graph_node_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_graph_node_layer, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_event_cb(s_graph_node_layer, graph_workspace_long_press_event,
                        LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_graph_node_layer,graph_workspace_click_event,LV_EVENT_SHORT_CLICKED,NULL);
    render_pedal_nodes();
    create_io_settings_overlay(s_pedalboard_page);
    create_graph_overlays(s_pedalboard_page);
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
        render_pedal_nodes();
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
        char rate_text[24];
        snprintf(rate_text, sizeof(rate_text), "%s kHz", s_sample_rate_text);
        lv_label_set_text(s_pedal_rate, rate_text);
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

static void update_pedal_meters(void)
{
    for(unsigned side=0;side<2;++side) for(unsigned ch=0;ch<2;++ch) {
        pedal_io_view_t *v=&s_pedal_io[side];
        const uint32_t peak=__atomic_exchange_n(&s_pending_local_peak[side*2+ch],0U,__ATOMIC_ACQ_REL);
        const float target=v->channel_enabled[ch]?(peak_to_db(peak)+60)*(100.f/60):0;
        v->displayed[ch]=fmaxf(target,v->displayed[ch]-2.4f);
        if(!v->channel_enabled[ch]) v->displayed[ch]=0;
        if(v->level[ch]) {
            lv_bar_set_value(v->level[ch],(int)v->displayed[ch],LV_ANIM_OFF);
            lv_obj_set_style_bg_color(v->level[ch],color(target>97?0xf05b68:target>85?0xffce68:0x48e0a8),LV_PART_INDICATOR);
        }
    }
}

static void dashboard_task(void *argument)
{
    (void)argument;
    while (true) {
        if (__atomic_load_n(&s_active_page,__ATOMIC_ACQUIRE)==DASHBOARD_PAGE_SETTINGS) {
            vTaskDelay(pdMS_TO_TICKS(PEDALBOARD_REFRESH_MS));
            continue;
        }
        uint32_t peaks[DASHBOARD_CHANNELS];
        for (unsigned i = 0; i < DASHBOARD_CHANNELS; ++i) {
            peaks[i] = __atomic_exchange_n(&s_pending_peak[i], 0U,
                                           __ATOMIC_ACQ_REL);
        }
        if (bsp_display_lock(pdMS_TO_TICKS(50))) {
            if(s_active_page==DASHBOARD_PAGE_MONITOR)
                for (unsigned i=0;i<DASHBOARD_CHANNELS;++i) update_meter(i,peaks[i]);
            else if(s_active_page==DASHBOARD_PAGE_PEDALBOARD)
                update_pedal_meters();
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(DASHBOARD_REFRESH_MS));
    }
}

esp_err_t audio_dashboard_install_seedfx_catalog(
    const SeedFxCatalogEntry *entries, size_t count)
{
    if (entries == NULL || count == 0U || count > SEEDFX_MAX_EFFECTS
        || s_graph_workspace != NULL) return ESP_ERR_INVALID_ARG;
    for (size_t index = 0; index < count; ++index) {
        if (entries[index].effect_type < SEEDFX_EFFECT_BYPASS
            || entries[index].effect_type > SEEDFX_EFFECT_LAST
            || entries[index].parameter_count > SEEDFX_MAX_PARAMS) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    memcpy(s_effect_catalog, entries, count * sizeof(entries[0]));
    s_effect_catalog_count = count;
    seedfx_add_routing_catalog(s_effect_catalog,&s_effect_catalog_count);
    return ESP_OK;
}

esp_err_t audio_dashboard_install_seedfx_graph(
    const SeedFxGraphDefinition *graph)
{
    if (!seedfx_graph_header_is_valid(graph)
        || seedfx_graph_crc32(graph) != graph->crc32
        || !seedfx_routes_valid(graph)
        || graph->node_count > PEDALBOARD_VISIBLE_NODES
        || s_graph_workspace != NULL) return ESP_ERR_INVALID_ARG;
    memset(s_pedal_nodes, 0, sizeof(s_pedal_nodes));
    s_pedal_node_count = graph->node_count;
    s_ui_edge_count = graph->edge_count;
    memcpy(s_ui_edges, graph->edges, sizeof(s_ui_edges));
    for (uint8_t index = 0; index < graph->node_count; ++index) {
        s_pedal_nodes[index].id = graph->nodes[index].id;
        s_pedal_nodes[index].source_channel = graph->nodes[index].reserved & 1U;
        s_pedal_nodes[index].type = graph->nodes[index].effect_type;
        s_pedal_nodes[index].enabled =
            (graph->nodes[index].flags & SEEDFX_NODE_ENABLED) != 0U;
        memcpy(s_pedal_nodes[index].parameters, graph->nodes[index].parameters,
               sizeof(s_pedal_nodes[index].parameters));
        const SeedFxCatalogEntry *fx = effect_info(s_pedal_nodes[index].type);
        for (unsigned p = graph->nodes[index].parameter_count; p < fx->parameter_count; ++p)
            s_pedal_nodes[index].parameters[p] = fx->parameters[p].default_value;
    }
    return ESP_OK;
}

esp_err_t audio_dashboard_init(void)
{
    seedfx_add_routing_catalog(s_effect_catalog,&s_effect_catalog_count);
    s_graph_commands = xQueueCreateStatic(32, sizeof(audio_dashboard_command_t),
                                          s_graph_queue_storage, &s_graph_queue);
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
    /* Candidate graph validation and connection dialogs run outside audio,
     * with enough stack for bounded graph copies and nested LVGL events. */
#ifdef ESP_PLATFORM
    config.lv_adapter_cfg.task_stack_size=16*1024;
    /* USB worker is core 0 / priority 12; SPI is core 1 / priority 15.
     * Pin the LVGL owner below SPI; its single SW worker also has priority 3. */
    config.lv_adapter_cfg.task_core_id=1;
    config.lv_adapter_cfg.task_priority=3;
#endif
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
    if (flags == 0U)
        return s_graph_commands != NULL && xQueueReceive(s_graph_commands, command, 0) == pdTRUE;
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


void audio_dashboard_submit_local_peaks(uint32_t in1,uint32_t in2,uint32_t out1,uint32_t out2)
{
    if(__atomic_load_n(&s_active_page,__ATOMIC_ACQUIRE)!=DASHBOARD_PAGE_PEDALBOARD) return;
    const uint32_t values[]={in1,in2,out1,out2};
    for(unsigned i=0;i<4;++i) atomic_max_u32(&s_pending_local_peak[i],values[i]);
}
