#include "user_data.h"
#include "rf_protocol.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include <stdatomic.h>
static atomic_uint revision;
unsigned user_signals_revision(void) { return atomic_load(&revision); }
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// 128 KiB signal partition; keep enough free space for old/new 48 KiB blobs.
#define SIGNAL_LIMIT 32
#define SIGNAL_BYTES (48 * 1024)
static bool separate_signals;
static esp_err_t open_signals(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return separate_signals ? nvs_open_from_partition("signals", "signals", mode, handle) : nvs_open("signals", mode, handle);
}
static SemaphoreHandle_t lock;
static void *json_allocate(size_t bytes)
{
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory ? memory : malloc(bytes);
}

esp_err_t user_data_init(void)
{
    esp_err_t err = nvs_flash_init(); // Never erase user data to recover an initialization error.
    if (err != ESP_OK) return err;
    err = nvs_flash_init_partition("signals");
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
    separate_signals = err == ESP_OK;
    cJSON_Hooks hooks = {.malloc_fn = json_allocate, .free_fn = free};
    cJSON_InitHooks(&hooks); // Install once at boot, before HTTP/RF tasks exist.
    if (!lock) lock = xSemaphoreCreateMutex();
    return lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t read_signals(cJSON **signals)
{
    *signals = NULL;
    nvs_handle_t handle;
    esp_err_t err = open_signals(NVS_READONLY, &handle);
    size_t size = 0;
    bool legacy = false;
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, "library", NULL, &size);
        if (err != ESP_OK) { nvs_close(handle); }
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        legacy = true;
        err = nvs_open("signals", NVS_READONLY, &handle);
        if (err == ESP_OK) {
            err = nvs_get_str(handle, "records", NULL, &size);
            if (err != ESP_OK) nvs_close(handle);
        }
    }
    if (err != ESP_OK) {
        // Read-only nvs_open can return NOT_FOUND before assigning a handle.
        if (legacy && err == ESP_ERR_NVS_NOT_FOUND) {
            *signals = cJSON_CreateArray();
            return *signals ? ESP_OK : ESP_ERR_NO_MEM;
        }
        return err;
    }
    char *text = size && size <= SIGNAL_BYTES ? malloc(size) : NULL;
    if (!text) { nvs_close(handle); return size > SIGNAL_BYTES ? ESP_ERR_INVALID_SIZE : ESP_ERR_NO_MEM; }
    err = legacy ? nvs_get_str(handle, "records", text, &size) : nvs_get_blob(handle, "library", text, &size);
    nvs_close(handle);
    if (err == ESP_OK) {
        if (!size || text[size - 1] != 0) err = ESP_ERR_INVALID_RESPONSE;
        else {
            *signals = cJSON_Parse(text);
            if (!cJSON_IsArray(*signals)) { cJSON_Delete(*signals); *signals = NULL; err = ESP_ERR_INVALID_RESPONSE; }
        }
    }
    free(text);
    return err;
}

esp_err_t user_signals_read(cJSON **signals)
{
    if (!lock || !signals) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = read_signals(signals);
    xSemaphoreGive(lock);
    return err;
}

static esp_err_t write_signals(cJSON *signals)
{
    char *text = cJSON_PrintUnformatted(signals);
    if (!text) return ESP_ERR_NO_MEM;
    if (strlen(text) + 1 > (separate_signals ? SIGNAL_BYTES : 8192)) { cJSON_free(text); return ESP_ERR_INVALID_SIZE; }
    nvs_handle_t handle;
    esp_err_t err = open_signals(NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "library", text, strlen(text) + 1);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    cJSON_free(text);
    if (err == ESP_OK) atomic_fetch_add(&revision, 1);
    return err;
}

static bool valid_fields(const char *name, const char *code)
{
    if (!name || !*name || strlen(name) > 63 || !code || !*code || strlen(code) > 128) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) if (*p < 32 || *p == 127) return false;
    for (const unsigned char *p = (const unsigned char *)code; *p; p++) if (!isxdigit(*p)) return false;
    return true;
}

static int find_signal(cJSON *signals, uint32_t id)
{
    for (int i = 0; i < cJSON_GetArraySize(signals); i++) {
        cJSON *number = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(signals, i), "id");
        if (cJSON_IsNumber(number) && number->valuedouble == id) return i;
    }
    return -1;
}

esp_err_t user_signal_save(cJSON *signal, uint32_t *id)
{
    if (!lock || !id || !cJSON_IsObject(signal)) return ESP_ERR_INVALID_ARG;
    if (!valid_fields(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(signal, "name")),
                      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(signal, "key_code")))) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON *signals = NULL;
    esp_err_t err = read_signals(&signals);
    if (err == ESP_OK) {
        if (cJSON_GetArraySize(signals) >= SIGNAL_LIMIT) err = ESP_ERR_NO_MEM;
        else {
            uint32_t next = 1;
            nvs_handle_t ids;
            if (open_signals(NVS_READWRITE, &ids) == ESP_OK) {
                nvs_get_u32(ids, "next_id", &next);
                nvs_close(ids);
            }
            for (int i = 0; i < cJSON_GetArraySize(signals); i++) {
                cJSON *number = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(signals, i), "id");
                if (cJSON_IsNumber(number) && number->valuedouble >= next) next = (uint32_t)number->valuedouble + 1;
            }
            cJSON *copy = cJSON_Duplicate(signal, true);
            if (!copy) err = ESP_ERR_NO_MEM;
            else {
                cJSON_DeleteItemFromObjectCaseSensitive(copy, "id");
                if (!cJSON_AddNumberToObject(copy, "id", next) || !cJSON_AddItemToArray(signals, copy)) {
                    cJSON_Delete(copy); err = ESP_ERR_NO_MEM;
                } else {
                    nvs_handle_t ids;
                    err = open_signals(NVS_READWRITE, &ids);
                    if (err == ESP_OK) {
                        err = nvs_set_u32(ids, "next_id", next + 1);
                        if (err == ESP_OK) err = nvs_commit(ids);
                        nvs_close(ids);
                    }
                    if (err == ESP_OK) err = write_signals(signals);
                    if (err == ESP_OK) *id = next;
                }
            }
        }
    }
    cJSON_Delete(signals);
    xSemaphoreGive(lock);
    return err;
}

int user_signal_button_code(const cJSON *record)
{
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "protocol"));
    cJSON *first = cJSON_GetObjectItemCaseSensitive(record, "first_level");
    cJSON *hz = cJSON_GetObjectItemCaseSensitive(record, "frequency_hz");
    cJSON *raw = cJSON_GetObjectItemCaseSensitive(record, "raw");
    if (!kind || strcmp(kind, "RAW") || !cJSON_IsNumber(first) || first->valuedouble != 1 ||
        !cJSON_IsNumber(hz) || hz->valuedouble != 433920000 || !cJSON_IsArray(raw) ||
        cJSON_GetArraySize(raw) != 82) return -1;
    unsigned d[82];
    for (unsigned i = 0; i < 82; i++) {
        cJSON *v = cJSON_GetArrayItem(raw, i);
        if (!cJSON_IsNumber(v) || v->valuedouble < 1 || v->valuedouble > 32767 ||
            v->valuedouble != v->valueint) return -1;
        d[i] = v->valueint;
    }
    if (d[0] < 4000 || d[0] > 5600 || d[1] < 1100 || d[1] > 1900 ||
        !((d[80] >= 250 && d[80] <= 480) || (d[80] >= 560 && d[80] <= 900)) ||
        d[81] < 6000 || d[81] > 9500) return -1;
    unsigned tail = 0;
    for (unsigned bit = 0; bit < 39; bit++) {
        unsigned h = d[2 + bit*2], l = d[3 + bit*2];
        bool zero = h >= 250 && h <= 480 && l >= 560 && l <= 900;
        bool one = l >= 250 && l <= 480 && h >= 560 && h <= 900;
        if (!zero && !one) return -1;
        if (bit >= 32) tail = (tail << 1) | one;
    }
    // Infer the terminal bit from its high; the low merges into the frame gap.
    return (int)((tail << 1) | (d[80] >= 560));
}

static esp_err_t change_signal(uint32_t id, const char *name, const char *code, bool remove)
{
    if (!lock || !id || (!remove && !valid_fields(name, code))) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON *signals = NULL;
    esp_err_t err = read_signals(&signals);
    if (err == ESP_OK) {
        int index = find_signal(signals, id);
        if (index < 0) err = ESP_ERR_NOT_FOUND;
        else {
            if (remove) cJSON_DeleteItemFromArray(signals, index);
            else {
                cJSON *record = cJSON_GetArrayItem(signals, index);
                cJSON *protocol = cJSON_GetObjectItemCaseSensitive(record, "protocol");
                const char *kind = cJSON_GetStringValue(protocol);
                const char *old = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "key_code"));
                if (kind && !strcmp(kind, "RAW") && old && strcmp(old, code)) {
                    int button = user_signal_button_code(record);
                    if (button < 0) err = ESP_ERR_NOT_SUPPORTED;
                    uint32_t after = 0;
                    if (button >= 0 && (strlen(code) != 2 || !rf_code_parse(code, 8, &after)))
                        err = ESP_ERR_INVALID_ARG;
                    if (err == ESP_OK) {
                        unsigned before = (unsigned)button;
                        cJSON *raw = cJSON_GetObjectItemCaseSensitive(record, "raw");
                        // Swap only changed high/low pairs in the final byte.
                        // The first 32 bits, measured timings and terminal gap survive.
                        for (unsigned bit = 0; bit < 7; bit++) {
                            if (!((before ^ after) & (1U << (7-bit)))) continue;
                            cJSON *h = cJSON_GetArrayItem(raw, 66 + bit*2);
                            cJSON *l = cJSON_GetArrayItem(raw, 67 + bit*2);
                            double duration = h->valuedouble;
                            cJSON_SetNumberValue(h, l->valuedouble);
                            cJSON_SetNumberValue(l, duration);
                        }
                        if ((before ^ after) & 1) {
                            // The last low includes the inter-frame idle: preserve
                            // the pair's total duration when changing its high.
                            cJSON *a = cJSON_GetArrayItem(raw, 2), *b = cJSON_GetArrayItem(raw, 3);
                            double short_us = a->valuedouble < b->valuedouble ? a->valuedouble : b->valuedouble;
                            double long_us = a->valuedouble > b->valuedouble ? a->valuedouble : b->valuedouble;
                            cJSON *h = cJSON_GetArrayItem(raw, 80), *l = cJSON_GetArrayItem(raw, 81);
                            double total = h->valuedouble + l->valuedouble;
                            cJSON_SetNumberValue(h, ((after & 1) ? long_us : short_us));
                            cJSON_SetNumberValue(l, total - h->valuedouble);
                        }
                    }
                }
                if (kind && !strcmp(kind, "OOK_PWM")) {
                    uint32_t number;
                    cJSON *bits = cJSON_GetObjectItemCaseSensitive(record, "bits");
                    cJSON *pulse = cJSON_GetObjectItemCaseSensitive(record, "pulse_us");
                    rf_frame_t *frame = calloc(1, sizeof(*frame));
                    if (!frame) err = ESP_ERR_NO_MEM;
                    else if (!cJSON_IsNumber(bits) || !cJSON_IsNumber(pulse) ||
                        !rf_code_parse(code, bits->valueint, &number) ||
                        !rf_frame_encode(frame, number, bits->valueint, pulse->valueint)) err = ESP_ERR_INVALID_ARG;
                    if (err == ESP_OK) {
                        cJSON *raw = cJSON_CreateArray();
                        if (!raw) err = ESP_ERR_NO_MEM;
                        else {
                            for (unsigned i = 0; i < frame->count; i++) {
                                cJSON *value = cJSON_CreateNumber(frame->duration[i]);
                                if (!value || !cJSON_AddItemToArray(raw, value)) { cJSON_Delete(value); err = ESP_ERR_NO_MEM; break; }
                            }
                            if (err == ESP_OK) {
                                cJSON_DeleteItemFromObjectCaseSensitive(record, "raw");
                                if (!cJSON_AddItemToObject(record, "raw", raw)) { cJSON_Delete(raw); err = ESP_ERR_NO_MEM; }
                            } else cJSON_Delete(raw);
                        }
                    }
                    free(frame);
                }
                if (err != ESP_OK) { cJSON_Delete(signals); xSemaphoreGive(lock); return err; }
                cJSON *n = cJSON_CreateString(name), *k = cJSON_CreateString(code);
                if (!n || !k) { cJSON_Delete(n); cJSON_Delete(k); err = ESP_ERR_NO_MEM; }
                else {
                    cJSON_ReplaceItemInObjectCaseSensitive(record, "name", n);
                    cJSON_ReplaceItemInObjectCaseSensitive(record, "key_code", k);
                }
            }
            if (err == ESP_OK) err = write_signals(signals);
        }
    }
    cJSON_Delete(signals);
    xSemaphoreGive(lock);
    return err;
}
esp_err_t user_signal_edit(uint32_t id, const char *name, const char *code) { return change_signal(id, name, code, false); }
esp_err_t user_signal_delete(uint32_t id) { return change_signal(id, NULL, NULL, true); }

uint8_t user_brightness_load(void)
{
    nvs_handle_t handle;
    uint8_t value = 70;
    if (nvs_open("settings", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "brightness", &value);
        nvs_close(handle);
    }
    return value >= 1 && value <= 100 ? value : 70;
}
esp_err_t user_brightness_save(uint8_t value)
{
    if (value < 1 || value > 100) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "brightness", value);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}

void user_sleep_load(uint8_t *light, uint8_t *deep)
{
    uint32_t value = 10U | (20U << 8);
    nvs_handle_t handle;
    if (nvs_open("settings", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u32(handle, "sleep_minutes", &value);
        nvs_close(handle);
    }
    if ((value & 0xff) > 120 || (value >> 8) > 120) value = 10U | (20U << 8);
    *light = value & 0xff; *deep = value >> 8;
}

esp_err_t user_sleep_save(uint8_t light, uint8_t deep)
{
    if (light > 120 || deep > 120) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "sleep_minutes", light | ((uint32_t)deep << 8));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}

esp_err_t user_upgrade_stage(uint8_t partition, bool retain, bool reset)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("upgrade", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "pending", partition | (retain ? 0 : 0x100) | (reset ? 0x200 : 0));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}

uint8_t user_receive_options_load(void)
{
    uint8_t options = 0;
    nvs_handle_t handle;
    if (nvs_open("settings", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "receive_options", &options);
        nvs_close(handle);
    }
    return options <= 3 ? options : 0;
}

esp_err_t user_receive_options_save(uint8_t options)
{
    if (options > 3) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "receive_options", options);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}

static esp_err_t erase_namespace(const char *name)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(name, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_all(handle);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}

esp_err_t user_upgrade_finish(uint8_t running_partition, bool verified)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("upgrade", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    uint32_t pending;
    err = nvs_get_u32(handle, "pending", &pending);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    else if (err == ESP_OK) {
        // Rollback boots a different slot: discard intent and keep all user data.
        if ((pending & 0xff) == running_partition && !verified) { nvs_close(handle); return ESP_OK; }
        if ((pending & 0xff) == running_partition) {
            if (pending & 0x100) {
                cJSON *empty = cJSON_CreateArray();
                err = empty ? write_signals(empty) : ESP_ERR_NO_MEM;
                cJSON_Delete(empty);
                // Keep the empty blob as a migration marker. Erasing this NVS
                // namespace would make read_signals() fall back to the old library.
            }
            if (err == ESP_OK && (pending & 0x200)) err = erase_namespace("settings");
        }
        // Cleanup is idempotent; keep intent on error so the next boot can retry.
        if (err == ESP_OK) err = nvs_erase_key(handle, "pending");
        if (err == ESP_OK) err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
