#include "p4_seedfx_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_dashboard.h"
#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_log.h"
#include "p4_audio_control.h"
#include "seedfx_catalog.h"
#include "seedfx_extensions.h"
#include "seedfx_routing.h"

enum { MAX_INDEX_BYTES = 16384, MAX_PACKAGE_BYTES = 16384 };
static const char *TAG = "seedfx_sd";
static const char *ROOT = "/sdcard/SEEDFX";
static SeedFxCatalogEntry s_catalog[SEEDFX_MAX_EFFECTS];

static bool safe_relative_path(const char *path)
{
    if (path == NULL || path[0] == '\0' || path[0] == '/'
        || strchr(path, '\\') != NULL || strstr(path, "..") != NULL
        || strlen(path) > 180U) return false;
    return true;
}

static esp_err_t read_file(const char *path, size_t maximum,
                           uint8_t **data, size_t *bytes)
{
    *data = NULL;
    *bytes = 0U;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return ESP_ERR_NOT_FOUND;
    esp_err_t result = ESP_FAIL;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long length = ftell(file);
        if (length > 0 && (size_t)length <= maximum
            && fseek(file, 0, SEEK_SET) == 0) {
            uint8_t *buffer = malloc((size_t)length + 1U);
            if (buffer == NULL) result = ESP_ERR_NO_MEM;
            else if (fread(buffer, 1, (size_t)length, file) == (size_t)length) {
                buffer[length] = 0;
                *data = buffer;
                *bytes = (size_t)length;
                result = ESP_OK;
            } else {
                free(buffer);
            }
        } else if (length > (long)maximum) result = ESP_ERR_INVALID_SIZE;
    }
    fclose(file);
    return result;
}

static uint32_t json_color(const cJSON *item)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) return 0x5d7892U;
    const char *text = item->valuestring;
    if (*text == '#') ++text;
    return (uint32_t)strtoul(text, NULL, 16) & 0xffffffU;
}

static uint8_t json_shape(const cJSON *item)
{
    if (!cJSON_IsString(item)) return SEEDFX_SHAPE_RECTANGLE;
    if (strcmp(item->valuestring, "pill") == 0) return SEEDFX_SHAPE_PILL;
    if (strcmp(item->valuestring, "rounded") == 0) return SEEDFX_SHAPE_ROUNDED;
    return SEEDFX_SHAPE_RECTANGLE;
}

static void copy_json_string(char *destination, size_t capacity,
                             const cJSON *item, const char *fallback)
{
    const char *source = cJSON_IsString(item) ? item->valuestring : fallback;
    snprintf(destination, capacity, "%s", source != NULL ? source : "");
}

static esp_err_t parse_effect_package(const char *path,
                                      SeedFxCatalogEntry *entry)
{
    uint8_t *blob = NULL;
    size_t bytes = 0U;
    esp_err_t result = read_file(path, MAX_PACKAGE_BYTES, &blob, &bytes);
    if (result != ESP_OK) return result;
    if (bytes < sizeof(SeedFxPackageHeader)) {
        free(blob); return ESP_ERR_INVALID_SIZE;
    }
    SeedFxPackageHeader header;
    memcpy(&header, blob, sizeof(header));
    const size_t expected = sizeof(header) + header.manifest_bytes
                            + header.payload_bytes;
    if (header.magic != SEEDFX_PACKAGE_MAGIC
        || header.version != SEEDFX_PACKAGE_VERSION
        || header.header_bytes != sizeof(header) || expected != bytes
        || header.manifest_bytes == 0U) {
        free(blob); return ESP_ERR_INVALID_VERSION;
    }
    const uint32_t stored_crc = header.package_crc32;
    memset(blob + offsetof(SeedFxPackageHeader, package_crc32), 0, 4);
    const uint32_t calculated_crc = seedfx_crc32(blob, bytes);
    memcpy(blob + offsetof(SeedFxPackageHeader, package_crc32),
           &stored_crc, sizeof(stored_crc));
    if (calculated_crc != stored_crc
        || (header.payload_bytes != 0U
            && seedfx_crc32(blob + sizeof(header) + header.manifest_bytes,
                            header.payload_bytes) != header.payload_crc32)) {
        free(blob); return ESP_ERR_INVALID_CRC;
    }

    cJSON *manifest = cJSON_ParseWithLength(
        (const char *)blob + sizeof(header), header.manifest_bytes);
    if (manifest == NULL) { free(blob); return ESP_ERR_INVALID_RESPONSE; }
    memset(entry, 0, sizeof(*entry));
    entry->package_id = header.package_id;
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(manifest, "format");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(manifest, "version");
    const cJSON *processor = cJSON_GetObjectItemCaseSensitive(manifest, "processor");
    const cJSON *effect_type = cJSON_GetObjectItemCaseSensitive(
        manifest, "effect_type");
    const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(
        manifest, "parameters");
    if (!cJSON_IsString(format)
        || strcmp(format->valuestring, "seedfx-effect") != 0
        || !cJSON_IsNumber(version) || version->valueint != 1
        || !cJSON_IsString(processor)
        || strncmp(processor->valuestring, "builtin:", 8) != 0
        || !cJSON_IsNumber(effect_type) || effect_type->valueint < 1
        || effect_type->valueint > SEEDFX_EFFECT_LAST
        || (!cJSON_IsArray(parameters) && !cJSON_IsNull(parameters))) {
        cJSON_Delete(manifest); free(blob); return ESP_ERR_INVALID_ARG;
    }
    entry->effect_type = (uint16_t)effect_type->valueint;
    entry->color_rgb = json_color(cJSON_GetObjectItemCaseSensitive(
        manifest, "color"));
    entry->shape = json_shape(cJSON_GetObjectItemCaseSensitive(
        manifest, "shape"));
    copy_json_string(entry->name, sizeof(entry->name),
        cJSON_GetObjectItemCaseSensitive(manifest, "name"), "Effect");
    copy_json_string(entry->category, sizeof(entry->category),
        cJSON_GetObjectItemCaseSensitive(manifest, "category"), "other");
    const cJSON *cpu = cJSON_GetObjectItemCaseSensitive(manifest, "cpu_percent");
    const cJSON *cpu48 = cJSON_IsObject(cpu)
        ? cJSON_GetObjectItemCaseSensitive(cpu, "48000") : NULL;
    snprintf(entry->cpu_label, sizeof(entry->cpu_label), "~%.2g%% CPU",
             cJSON_IsNumber(cpu48) ? cpu48->valuedouble : 0.0);
    const int parameter_count = cJSON_IsArray(parameters)
        ? cJSON_GetArraySize(parameters) : 0;
    if (parameter_count > SEEDFX_MAX_PARAMS) {
        cJSON_Delete(manifest); free(blob); return ESP_ERR_INVALID_SIZE;
    }
    entry->parameter_count = (uint8_t)parameter_count;
    for (int index = 0; index < parameter_count; ++index) {
        const cJSON *parameter = cJSON_GetArrayItem(parameters, index);
        SeedFxParameterDescriptor *target = &entry->parameters[index];
        copy_json_string(target->name, sizeof(target->name),
            cJSON_GetObjectItemCaseSensitive(parameter, "name"), "Value");
        copy_json_string(target->unit, sizeof(target->unit),
            cJSON_GetObjectItemCaseSensitive(parameter, "unit"), "");
        const cJSON *low = cJSON_GetObjectItemCaseSensitive(parameter, "min");
        const cJSON *high = cJSON_GetObjectItemCaseSensitive(parameter, "max");
        const cJSON *step = cJSON_GetObjectItemCaseSensitive(parameter, "step");
        const cJSON *def = cJSON_GetObjectItemCaseSensitive(parameter, "default");
        if (!cJSON_IsNumber(low) || !cJSON_IsNumber(high)
            || !cJSON_IsNumber(step) || !cJSON_IsNumber(def)
            || low->valuedouble > high->valuedouble
            || def->valuedouble < low->valuedouble
            || def->valuedouble > high->valuedouble
            || step->valuedouble <= 0.0) {
            cJSON_Delete(manifest); free(blob); return ESP_ERR_INVALID_ARG;
        }
        target->minimum = (float)low->valuedouble;
        target->maximum = (float)high->valuedouble;
        target->step = (float)step->valuedouble;
        target->default_value = (float)def->valuedouble;
    }
    seedfx_extend_catalog(entry);
    cJSON_Delete(manifest);
    free(blob);
    return ESP_OK;
}

static esp_err_t load_autorun(const char *relative)
{
    if (!safe_relative_path(relative)) return ESP_ERR_INVALID_ARG;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", ROOT, relative);
    uint8_t *blob = NULL;
    size_t bytes = 0U;
    esp_err_t result = read_file(path, sizeof(SeedFxGraphDefinition),
                                 &blob, &bytes);
    if (result != ESP_OK) return result;
    {
        SeedFxGraphDefinition graph;
        if (!seedfx_graph_decode(blob,bytes,&graph))
            result = ESP_ERR_INVALID_CRC;
        else {
            result = p4_audio_control_load_seedfx_graph(&graph);
            if (result == ESP_OK)
                result = audio_dashboard_install_seedfx_graph(&graph);
        }
    }
    free(blob);
    return result;
}

esp_err_t p4_seedfx_storage_init(void)
{
    esp_err_t result = bsp_sdcard_mount();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "microSD unavailable (%s); built-in catalogue active",
                 esp_err_to_name(result));
        return ESP_OK;
    }
    char index_path[256];
    snprintf(index_path, sizeof(index_path), "%s/index.json", ROOT);
    uint8_t *index_blob = NULL;
    size_t index_bytes = 0U;
    result = read_file(index_path, MAX_INDEX_BYTES, &index_blob, &index_bytes);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "index.json unavailable (%s); built-in catalogue active",
                 esp_err_to_name(result));
        return ESP_OK;
    }
    cJSON *index = cJSON_ParseWithLength((char *)index_blob, index_bytes);
    free(index_blob);
    if (index == NULL) {
        ESP_LOGW(TAG, "invalid index.json; built-in catalogue active");
        return ESP_OK;
    }
    const cJSON *effects = cJSON_GetObjectItemCaseSensitive(index, "effects");
    const cJSON *autorun = cJSON_GetObjectItemCaseSensitive(index, "autorun");
    size_t count = 0U;
    if (cJSON_IsArray(effects)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, effects) {
            if (count >= SEEDFX_MAX_EFFECTS || !cJSON_IsString(item)
                || !safe_relative_path(item->valuestring)) continue;
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", ROOT, item->valuestring);
            SeedFxCatalogEntry candidate;
            result = parse_effect_package(path, &candidate);
            bool duplicate = false;
            if (result == ESP_OK)
                for (size_t i = 0; i < count; ++i)
                    duplicate |= s_catalog[i].effect_type == candidate.effect_type;
            if (result == ESP_OK && !duplicate) s_catalog[count++] = candidate;
            else ESP_LOGW(TAG, "ignored effect %s: %s", item->valuestring,
                          duplicate ? "duplicate type" : esp_err_to_name(result));
        }
    }
    if (count != 0U
        && p4_audio_control_install_seedfx_catalog(s_catalog, count) == ESP_OK
        && audio_dashboard_install_seedfx_catalog(s_catalog, count) == ESP_OK) {
        ESP_LOGI(TAG, "microSD catalogue loaded: %u effects", (unsigned)count);
        if (cJSON_IsString(autorun)) {
            result = load_autorun(autorun->valuestring);
            if (result != ESP_OK)
                ESP_LOGW(TAG, "AUTORUN ignored: %s", esp_err_to_name(result));
            else ESP_LOGI(TAG, "AUTORUN graph loaded");
        }
    } else {
        ESP_LOGW(TAG, "no valid effects; built-in catalogue active");
    }
    cJSON_Delete(index);
    return ESP_OK;
}
