#include "p4_audio_control.h"
#include <math.h>
#include "seedfx_ports.h"
#include "seedfx_routing.h"
#include "seedfx_routing_catalog.h"

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
static uint32_t s_auto_switch_views = 1U;
static uint32_t s_transport_profile = P4_AUDIO_TRANSPORT_BALANCED;
static uint32_t s_capture_channel_mask = SPI_AUDIO_CONTROL_CHANNEL_MASK;
static uint32_t s_playback_channel_mask = SPI_AUDIO_CONTROL_CHANNEL_MASK;
static uint32_t s_spi_errors;
static uint32_t s_crc_errors;
static SeedFxGraphDefinition s_seedfx_graph;
static uint32_t s_seedfx_graph_seqlock;
static uint32_t s_seedfx_graph_serial;
static nvs_handle_t s_settings_handle;
static bool s_settings_ready;
static bool s_prepared;
static TaskHandle_t s_control_task;
static SeedFxCatalogEntry s_seedfx_catalog[SEEDFX_MAX_EFFECTS];
static size_t s_seedfx_catalog_count;

static const SeedFxCatalogEntry *effect_info(uint16_t effect_type)
{
    for (size_t index = 0; index < s_seedfx_catalog_count; ++index)
        if (s_seedfx_catalog[index].effect_type == effect_type)
            return &s_seedfx_catalog[index];
    return NULL;
}

static bool effect_type_valid(uint16_t effect_type)
{
    return effect_info(effect_type) != NULL;
}

static void configure_node(SeedFxNodeDefinition *node,
                           uint16_t id,
                           const SeedFxCatalogEntry *effect)
{
    memset(node, 0, sizeof(*node));
    node->id = id;
    node->effect_type = effect->effect_type;
    node->package_id = effect->package_id;
    node->flags = SEEDFX_NODE_ENABLED;
    node->parameter_count = effect->parameter_count;
    node->shape = effect->shape;
    node->reserved = __atomic_load_n(&s_capture_channel_mask, __ATOMIC_ACQUIRE) == 2U ? 1 : 0;
    for (uint8_t p = 0; p < effect->parameter_count; ++p)
        node->parameters[p] = effect->parameters[p].default_value;
}

static void init_fallback_catalog(void)
{
    memset(s_seedfx_catalog, 0, sizeof(s_seedfx_catalog));
    const uint16_t types[] = {SEEDFX_EFFECT_BYPASS, SEEDFX_EFFECT_GAIN,
        SEEDFX_EFFECT_MUTE, SEEDFX_EFFECT_POLARITY, SEEDFX_EFFECT_SOFT_CLIP};
    for (size_t index = 0; index < sizeof(types) / sizeof(types[0]); ++index) {
        SeedFxCatalogEntry *effect = &s_seedfx_catalog[index];
        effect->package_id = 0x00010000U + types[index];
        effect->effect_type = types[index];
        effect->shape = types[index] == SEEDFX_EFFECT_SOFT_CLIP
            ? SEEDFX_SHAPE_PILL : (types[index] == SEEDFX_EFFECT_GAIN
                || types[index] == SEEDFX_EFFECT_POLARITY
                    ? SEEDFX_SHAPE_ROUNDED : SEEDFX_SHAPE_RECTANGLE);
        effect->parameter_count = types[index] == SEEDFX_EFFECT_SOFT_CLIP
            ? 2U : (types[index] == SEEDFX_EFFECT_GAIN ? 1U : 0U);
    }
    s_seedfx_catalog[1].parameters[0].minimum = -60.0f;
    s_seedfx_catalog[1].parameters[0].maximum = 18.0f;
    s_seedfx_catalog[1].parameters[0].step = 0.5f;
    s_seedfx_catalog[4].parameters[0].maximum = 30.0f;
    s_seedfx_catalog[4].parameters[0].step = 0.5f;
    s_seedfx_catalog[4].parameters[0].default_value = 6.0f;
    s_seedfx_catalog[4].parameters[1].maximum = 1.0f;
    s_seedfx_catalog[4].parameters[1].step = 0.01f;
    s_seedfx_catalog[4].parameters[1].default_value = 1.0f;
    s_seedfx_catalog_count = sizeof(types) / sizeof(types[0]);
    seedfx_add_routing_catalog(s_seedfx_catalog,&s_seedfx_catalog_count);
}

static void init_thru_edges(SeedFxGraphDefinition *graph)
{
    memset(graph->edges, 0, sizeof(graph->edges));
    graph->edge_count = 2;
    graph->edges[0]=(SeedFxEdgeDefinition){0,0,1,0,0,0};
    graph->edges[1]=(SeedFxEdgeDefinition){0,0,1,1,1,0};
}

static void finish_graph_edit(void)
{
    ++s_seedfx_graph.revision;
    s_seedfx_graph.crc32 = seedfx_graph_crc32(&s_seedfx_graph);
    ++s_seedfx_graph_serial;
    __atomic_fetch_add(&s_seedfx_graph_seqlock, 1U, __ATOMIC_RELEASE);
}

static void init_default_graph(void)
{
    memset(&s_seedfx_graph, 0, sizeof(s_seedfx_graph));
    s_seedfx_graph.magic = SEEDFX_GRAPH_MAGIC;
    s_seedfx_graph.version = SEEDFX_GRAPH_VERSION;
    s_seedfx_graph.bytes = sizeof(s_seedfx_graph);
    s_seedfx_graph.revision = 1U;
    s_seedfx_graph.flags = SEEDFX_GRAPH_ENABLED;
    memcpy(s_seedfx_graph.name, "Pedalboard", 10U);
    init_thru_edges(&s_seedfx_graph);
    s_seedfx_graph.crc32 = seedfx_graph_crc32(&s_seedfx_graph);
    s_seedfx_graph_serial = 1U;
}

static void apply_graph_edit(const audio_dashboard_command_t *command)
{
    if (command == NULL || !command->edit_graph) return;
    const uint8_t index = command->graph_node_index;
    bool changed = false;
    __atomic_fetch_add(&s_seedfx_graph_seqlock, 1U, __ATOMIC_ACQ_REL);
    switch ((audio_dashboard_graph_action_t)command->graph_action) {
        case AUDIO_DASHBOARD_GRAPH_ADD:
            if (s_seedfx_graph.node_count < SEEDFX_MAX_NODES
                && effect_type_valid(command->graph_effect_type)) {
                const uint16_t id=seedfx_next_node_id(&s_seedfx_graph);
                SeedFxNodeDefinition *node =
                    &s_seedfx_graph.nodes[s_seedfx_graph.node_count++];
                configure_node(node, id,
                               effect_info(command->graph_effect_type));
                changed = true;
            }
            break;
        case AUDIO_DASHBOARD_GRAPH_DELETE:
            if (index < s_seedfx_graph.node_count) {
                memmove(&s_seedfx_graph.nodes[index],
                        &s_seedfx_graph.nodes[index + 1U],
                        (s_seedfx_graph.node_count - index - 1U)
                            * sizeof(s_seedfx_graph.nodes[0]));
                --s_seedfx_graph.node_count;
                seedfx_prune_invalid_ports(&s_seedfx_graph);
                changed = true;
            }
            break;
        case AUDIO_DASHBOARD_GRAPH_REPLACE:
            if (index < s_seedfx_graph.node_count
                && effect_type_valid(command->graph_effect_type)) {
                SeedFxNodeDefinition *node = &s_seedfx_graph.nodes[index];
                configure_node(node, node->id,
                               effect_info(command->graph_effect_type));
                seedfx_prune_invalid_ports(&s_seedfx_graph);
                changed = true;
            }
            break;
        case AUDIO_DASHBOARD_GRAPH_TOGGLE:
            if (index < s_seedfx_graph.node_count) {
                s_seedfx_graph.nodes[index].flags ^= SEEDFX_NODE_ENABLED;
                changed = true;
            }
            break;
        case AUDIO_DASHBOARD_GRAPH_SET_PARAMETER:
            if (index < s_seedfx_graph.node_count
                && command->graph_parameter_index
                       < s_seedfx_graph.nodes[index].parameter_count) {
                const SeedFxCatalogEntry *effect = effect_info(
                    s_seedfx_graph.nodes[index].effect_type);
                const uint8_t p = command->graph_parameter_index;
                if (effect != NULL && p < effect->parameter_count
                    && isfinite(command->graph_parameter_value)) {
                    const SeedFxParameterDescriptor *range = &effect->parameters[p];
                    s_seedfx_graph.nodes[index].parameters[p] = fminf(range->maximum,
                        fmaxf(range->minimum, command->graph_parameter_value));
                    changed = true;
                }
            }
            break;
        case AUDIO_DASHBOARD_GRAPH_CONNECT:
            changed=seedfx_connect(&s_seedfx_graph,command->graph_source_id,
                command->graph_source_port,command->graph_destination_id,command->graph_destination_port);
            if(!changed) ESP_LOGW(TAG,"Rejected invalid/cyclic route");
            break;
        case AUDIO_DASHBOARD_GRAPH_DISCONNECT:
            seedfx_disconnect_input(&s_seedfx_graph,command->graph_destination_id,command->graph_destination_port);
            changed=true;
            break;
        default:
            break;
    }
    if (changed) {
        finish_graph_edit();
    } else {
        __atomic_fetch_add(&s_seedfx_graph_seqlock, 1U, __ATOMIC_RELEASE);
    }
}

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

static void save_auto_switch_views(void)
{
    if (!s_settings_ready) return;
    esp_err_t result = nvs_set_u8(
        s_settings_handle, "auto_views",
        __atomic_load_n(&s_auto_switch_views, __ATOMIC_ACQUIRE) ? 1U : 0U);
    if (result == ESP_OK) result = nvs_commit(s_settings_handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "cannot persist automatic view switching: %s",
                 esp_err_to_name(result));
    }
}

static void save_transport_profile(void)
{
    if (!s_settings_ready) return;
    esp_err_t result = nvs_set_u8(
        s_settings_handle, "transport",
        (uint8_t)__atomic_load_n(&s_transport_profile, __ATOMIC_ACQUIRE));
    if (result == ESP_OK) result = nvs_commit(s_settings_handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "cannot persist transport profile: %s",
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
    const p4_audio_transport_mode_t transport_profile =
        p4_audio_control_transport_profile();
    const p4_audio_transport_mode_t transport_mode =
        (usb->capture_active || usb->playback_active)
            ? transport_profile : P4_AUDIO_TRANSPORT_LOCAL;

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
        .auto_switch_views = __atomic_load_n(
            &s_auto_switch_views, __ATOMIC_ACQUIRE) != 0U,
        .transport_profile = (uint8_t)transport_profile,
        .transport_mode = (uint8_t)transport_mode,
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
            apply_graph_edit(&command);
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
            if (command.set_auto_switch_views) {
                __atomic_store_n(&s_auto_switch_views,
                                 command.auto_switch_views ? 1U : 0U,
                                 __ATOMIC_RELEASE);
                save_auto_switch_views();
                ESP_LOGI(TAG, "automatic view switching %s",
                         command.auto_switch_views ? "enabled" : "disabled");
            }
            if (command.set_transport_profile) {
                const esp_err_t result = p4_audio_control_set_transport_profile(
                    (p4_audio_transport_mode_t)command.transport_profile);
                if (result != ESP_OK) {
                    ESP_LOGW(TAG, "transport profile rejected: %s",
                             esp_err_to_name(result));
                }
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

    init_fallback_catalog();
    init_default_graph();
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
        uint8_t stored_auto_switch = 1U;
        const esp_err_t auto_result = nvs_get_u8(
            s_settings_handle, "auto_views", &stored_auto_switch);
        if (auto_result == ESP_OK) {
            s_auto_switch_views = stored_auto_switch != 0U;
        } else if (auto_result != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "automatic view preference ignored: %s",
                     esp_err_to_name(auto_result));
        }
        uint8_t stored_transport = P4_AUDIO_TRANSPORT_BALANCED;
        const esp_err_t transport_result = nvs_get_u8(
            s_settings_handle, "transport", &stored_transport);
        if (transport_result == ESP_OK
            && stored_transport <= P4_AUDIO_TRANSPORT_LOW_LATENCY) {
            s_transport_profile = stored_transport;
        } else if (transport_result != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "transport preference ignored: %s",
                     esp_err_to_name(transport_result));
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

p4_audio_transport_mode_t p4_audio_control_transport_profile(void)
{
    const uint32_t profile = __atomic_load_n(
        &s_transport_profile, __ATOMIC_ACQUIRE);
    return profile == P4_AUDIO_TRANSPORT_LOW_LATENCY
               ? P4_AUDIO_TRANSPORT_LOW_LATENCY
               : P4_AUDIO_TRANSPORT_BALANCED;
}

p4_audio_transport_mode_t p4_audio_control_transport_mode(void)
{
    if (!p4_uac2_capture_active() && !p4_uac2_playback_active()) {
        return P4_AUDIO_TRANSPORT_LOCAL;
    }
    return p4_audio_control_transport_profile();
}

esp_err_t p4_audio_control_set_transport_profile(
    p4_audio_transport_mode_t profile)
{
    if (profile != P4_AUDIO_TRANSPORT_BALANCED
        && profile != P4_AUDIO_TRANSPORT_LOW_LATENCY) {
        return ESP_ERR_INVALID_ARG;
    }
    __atomic_store_n(&s_transport_profile, (uint32_t)profile,
                     __ATOMIC_RELEASE);
    save_transport_profile();
    ESP_LOGI(TAG, "transport profile %s",
             profile == P4_AUDIO_TRANSPORT_LOW_LATENCY
                 ? "LOW LATENCY" : "BALANCED");
    return ESP_OK;
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
    snapshot->auto_switch_views = __atomic_load_n(
        &s_auto_switch_views, __ATOMIC_ACQUIRE) != 0U;
    snapshot->transport_profile = p4_audio_control_transport_profile();
    snapshot->transport_mode = p4_audio_control_transport_mode();
    snapshot->capture_channel_mask =
        p4_audio_control_capture_channel_mask();
    snapshot->playback_channel_mask =
        p4_audio_control_playback_channel_mask();
}

bool p4_audio_control_get_seedfx_graph(SeedFxGraphDefinition *graph,
                                      uint32_t *serial)
{
    if (graph == NULL || serial == NULL) return false;
    /* UART has higher priority than this graph's writer on the same core.
     * Never spin on an interrupted writer: let UART's next 20 ms poll retry. */
    for (unsigned attempt=0; attempt<3; ++attempt) {
        const uint32_t before = __atomic_load_n(
            &s_seedfx_graph_seqlock, __ATOMIC_ACQUIRE);
        if ((before & 1U) != 0U) return false;
        memcpy(graph, &s_seedfx_graph, sizeof(*graph));
        *serial = __atomic_load_n(&s_seedfx_graph_serial, __ATOMIC_ACQUIRE);
        const uint32_t after = __atomic_load_n(
            &s_seedfx_graph_seqlock, __ATOMIC_ACQUIRE);
        if (before == after && (after & 1U) == 0U) return true;
    }
    return false;
}

esp_err_t p4_audio_control_install_seedfx_catalog(
    const SeedFxCatalogEntry *entries, size_t count)
{
    if (entries == NULL || count == 0U || count > SEEDFX_MAX_EFFECTS)
        return ESP_ERR_INVALID_ARG;
    for (size_t index = 0; index < count; ++index) {
        if (entries[index].effect_type < SEEDFX_EFFECT_BYPASS
            || entries[index].effect_type > SEEDFX_EFFECT_LAST
            || entries[index].parameter_count > SEEDFX_MAX_PARAMS)
            return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_seedfx_catalog, entries, count * sizeof(entries[0]));
    s_seedfx_catalog_count = count;
    seedfx_add_routing_catalog(s_seedfx_catalog,&s_seedfx_catalog_count);
    return ESP_OK;
}

esp_err_t p4_audio_control_load_seedfx_graph(
    const SeedFxGraphDefinition *graph)
{
    if (!seedfx_graph_header_is_valid(graph)
        || seedfx_graph_crc32(graph) != graph->crc32 || !seedfx_routes_valid(graph))
        return ESP_ERR_INVALID_ARG;
    for (uint8_t index = 0; index < graph->node_count; ++index)
        if (!effect_type_valid(graph->nodes[index].effect_type))
            return ESP_ERR_NOT_SUPPORTED;
    __atomic_fetch_add(&s_seedfx_graph_seqlock, 1U, __ATOMIC_ACQ_REL);
    memcpy(&s_seedfx_graph, graph, sizeof(s_seedfx_graph));
    for (unsigned n = 0; n < s_seedfx_graph.node_count; ++n) {
        SeedFxNodeDefinition *node = &s_seedfx_graph.nodes[n];
        const SeedFxCatalogEntry *fx = effect_info(node->effect_type);
        if (fx == NULL) continue;
        for (unsigned p = node->parameter_count; p < fx->parameter_count; ++p)
            node->parameters[p] = fx->parameters[p].default_value;
        node->parameter_count = fx->parameter_count;
    }
    s_seedfx_graph.crc32 = seedfx_graph_crc32(&s_seedfx_graph);
    ++s_seedfx_graph_serial;
    __atomic_fetch_add(&s_seedfx_graph_seqlock, 1U, __ATOMIC_RELEASE);
    return ESP_OK;
}
