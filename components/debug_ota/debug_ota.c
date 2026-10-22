#include "debug_ota.h"
#include "user_data.h"
#include <stdatomic.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "debug_ota";
static atomic_int state;
static bool wifi_initialized, wifi_started, netif_initialized;
static esp_netif_t *ap;
static httpd_handle_t server;
static esp_timer_handle_t restart_timer;
static atomic_bool reboot_pending, operation_busy;
static esp_err_t (*pause_radio)(void);
static void (*resume_radio)(void);
static bool radio_paused;
void debug_ota_set_radio_hooks(esp_err_t (*pause)(void), void (*resume)(void))
{
    pause_radio = pause; resume_radio = resume;
}
extern const unsigned char index_start[] asm("_binary_index_html_start");
extern const unsigned char index_end[] asm("_binary_index_html_end");

static void restart_device(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t page_get(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)index_start, index_end - index_start - 1);
}

static esp_err_t info_get(httpd_req_t *request)
{
    const esp_app_desc_t *app_info = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    cJSON *info = cJSON_CreateObject();
    if (!info) return httpd_resp_send_500(request);
    cJSON_AddStringToObject(info, "project", app_info->project_name);
    cJSON_AddStringToObject(info, "version", app_info->version);
    cJSON_AddStringToObject(info, "chip", "ESP32-S3");
    cJSON_AddStringToObject(info, "partition", running->label);
    cJSON_AddNumberToObject(info, "max_size", next ? next->size : 0);
    char *body = cJSON_PrintUnformatted(info);
    cJSON_Delete(info);
    if (!body) return httpd_resp_send_500(request);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, body);
    cJSON_free(body);
    return err;
}

static esp_err_t signals_get(httpd_req_t *request)
{
    cJSON *signals = NULL;
    esp_err_t err = user_signals_read(&signals);
    if (err != ESP_OK) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    cJSON *record;
    cJSON_ArrayForEach(record, signals) {
        int button = user_signal_button_code(record);
        cJSON_DeleteItemFromObjectCaseSensitive(record, "button_code");
        if (button >= 0 && !cJSON_AddNumberToObject(record, "button_code", button)) {
            cJSON_Delete(signals); return httpd_resp_send_500(request);
        }
    }
    char *body = cJSON_PrintUnformatted(signals);
    cJSON_Delete(signals);
    if (!body) return httpd_resp_send_500(request);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    err = httpd_resp_sendstr(request, body);
    cJSON_free(body);
    return err;
}

static esp_err_t signal_post(httpd_req_t *request)
{
    char marker[8], body[768];
    if (reboot_pending || request->content_len == 0 || request->content_len >= sizeof(body) ||
        httpd_req_get_hdr_value_str(request, "X-RF-API", marker, sizeof(marker)) != ESP_OK || strcmp(marker, "1"))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid signal request");
    size_t received = 0;
    unsigned timeouts = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        if (count <= 0) return httpd_resp_send_408(request);
        received += count;
    }
    body[received] = 0;
    cJSON *record = cJSON_Parse(body);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(record, "id");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsNumber(id) && id->valuedouble >= 1 && id->valuedouble <= UINT32_MAX &&
        id->valuedouble == (uint32_t)id->valuedouble) {
        if (!strcmp(request->uri, "/api/signals/delete")) err = user_signal_delete((uint32_t)id->valuedouble);
        else err = user_signal_edit((uint32_t)id->valuedouble,
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "name")),
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "key_code")));
    }
    cJSON_Delete(record);
    if (err != ESP_OK) return httpd_resp_send_err(request,
        err == ESP_ERR_NOT_FOUND ? HTTPD_404_NOT_FOUND : err == ESP_ERR_INVALID_ARG ? HTTPD_400_BAD_REQUEST : HTTPD_500_INTERNAL_SERVER_ERROR,
        esp_err_to_name(err));
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

static bool option_header(httpd_req_t *request, const char *name, bool fallback, bool *valid)
{
    char value[8];
    esp_err_t err = httpd_req_get_hdr_value_str(request, name, value, sizeof(value));
    if (err == ESP_ERR_NOT_FOUND) return fallback;
    if (err != ESP_OK || (strcmp(value, "0") && strcmp(value, "1"))) { *valid = false; return fallback; }
    return !strcmp(value, "1");
}

static esp_err_t upload_image(httpd_req_t *request)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next || reboot_pending)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "OTA unavailable or restart pending");
    const size_t prefix_size = sizeof(esp_image_header_t) +
        sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (request->content_len < prefix_size)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Application image is too small");
    if (request->content_len > next->size)
        return httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE, "Image exceeds OTA slot");
    char content_type[64], marker[8];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, "application/octet-stream") ||
        httpd_req_get_hdr_value_str(request, "X-RF-OTA", marker, sizeof(marker)) != ESP_OK ||
        strcmp(marker, "1"))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Use the firmware upload page");

    bool valid = true;
    bool retain = option_header(request, "X-RF-Retain", true, &valid);
    bool reset = option_header(request, "X-RF-Reset", false, &valid);
    if (!valid) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid upgrade options");
    uint8_t buffer[1024];
    size_t received = 0;
    unsigned timeouts = 0;
    while (received < prefix_size) {
        int count = httpd_req_recv(request, (char *)buffer + received, prefix_size - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        if (count <= 0) return httpd_resp_send_408(request);
        received += count;
    }
    esp_image_header_t header;
    esp_app_desc_t descriptor;
    memcpy(&header, buffer, sizeof(header));
    memcpy(&descriptor, buffer + sizeof(header) + sizeof(esp_image_segment_header_t), sizeof(descriptor));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32S3 ||
        descriptor.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        memcmp(descriptor.project_name, esp_app_get_description()->project_name, sizeof(descriptor.project_name)))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Wrong chip/project; upload the application .bin");

    if (pause_radio) {
        esp_err_t pause_err = pause_radio();
        if (pause_err != ESP_OK) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot stop RF for OTA");
        radio_paused = true;
    }
    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(next, request->content_len, &handle);
    if (err != ESP_OK) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    err = esp_ota_write(handle, buffer, received);
    timeouts = 0;
    while (err == ESP_OK && received < request->content_len) {
        size_t remaining = request->content_len - received;
        int count = httpd_req_recv(request, (char *)buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        if (count <= 0) {
            esp_ota_abort(handle);
            return httpd_resp_send_408(request);
        }
        timeouts = 0;
        err = esp_ota_write(handle, buffer, count);
        received += count;
    }
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    err = esp_ota_end(handle); // Validates the complete image/checksum; frees the handle.
    if (err != ESP_OK) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    err = user_upgrade_stage(next->subtype, retain, reset);
    if (err != ESP_OK) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        user_upgrade_finish(0xff, false); // Cancel intent without erasing data.
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    reboot_pending = true;
    httpd_resp_set_type(request, "application/json");
    esp_err_t response = httpd_resp_sendstr(request, "{\"ok\":true,\"message\":\"校验成功，设备将在两秒后重启。请重新进入调试并连接热点。\"}");
    err = esp_timer_start_once(restart_timer, 2000000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Restart timer failed: %s", esp_err_to_name(err));
        reboot_pending = false;
    }
    return response;
}

static esp_err_t upload_post(httpd_req_t *request)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&operation_busy, &expected, true))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Device is stopping or upgrading");
    esp_err_t err = upload_image(request);
    if (radio_paused && !reboot_pending) {
        if (resume_radio) resume_radio();
        radio_paused = false;
    }
    atomic_store(&operation_busy, false);
    return err;
}

static esp_err_t start_wifi(void)
{
    if (!netif_initialized) {
        ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS init");
        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        netif_initialized = true;
    }
    if (!ap) {
        ap = esp_netif_create_default_wifi_ap();
        if (!ap) return ESP_ERR_NO_MEM;
    }
    if (!wifi_initialized) {
        const wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&config), TAG, "Wi-Fi init");
        wifi_initialized = true;
    }
    if (!wifi_started) {
        wifi_config_t config = {.ap = {
            .ssid = DEBUG_WIFI_SSID, .password = DEBUG_WIFI_PASSWORD,
            .ssid_len = sizeof(DEBUG_WIFI_SSID) - 1, .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK, .max_connection = 2,
        }};
        ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage");
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "AP mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG, "AP config");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
        wifi_started = true;
    }
    return ESP_OK;
}

static void start_worker(void *argument)
{
    (void)argument;
    esp_err_t err = start_wifi();
    if (err == ESP_OK && !restart_timer) {
        const esp_timer_create_args_t config = {.callback = restart_device, .name = "ota_restart"};
        err = esp_timer_create(&config, &restart_timer);
    }
    if (err == ESP_OK && !server) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.stack_size = 6144;
        config.max_uri_handlers = 6;
        config.recv_wait_timeout = 5;
        config.lru_purge_enable = true;
        err = httpd_start(&server, &config);
        if (err == ESP_OK) {
            const httpd_uri_t routes[] = {
                {.uri = "/", .method = HTTP_GET, .handler = page_get},
                {.uri = "/api/info", .method = HTTP_GET, .handler = info_get},
                {.uri = "/api/ota", .method = HTTP_POST, .handler = upload_post},
                {.uri = "/api/signals", .method = HTTP_GET, .handler = signals_get},
                {.uri = "/api/signals/update", .method = HTTP_POST, .handler = signal_post},
                {.uri = "/api/signals/delete", .method = HTTP_POST, .handler = signal_post},
            };
            for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
                err = httpd_register_uri_handler(server, &routes[i]);
                if (err != ESP_OK) break;
            }
            if (err != ESP_OK) { httpd_stop(server); server = NULL; }
        }
    }
    if (err != ESP_OK && wifi_started) {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err == ESP_OK) wifi_started = false;
        else ESP_LOGE(TAG, "Failed to stop Wi-Fi after startup error: %s", esp_err_to_name(stop_err));
    }
    atomic_store(&state, err == ESP_OK ? DEBUG_OTA_READY : DEBUG_OTA_ERROR);
    if (err == ESP_OK) ESP_LOGI(TAG, "Hotspot %s, OTA page %s", DEBUG_WIFI_SSID, DEBUG_OTA_URL);
    else ESP_LOGE(TAG, "Debug start failed: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

esp_err_t debug_ota_start(void)
{
    int current = atomic_load(&state);
    if (current == DEBUG_OTA_STOPPING) return ESP_ERR_INVALID_STATE;
    if (current == DEBUG_OTA_STARTING || current == DEBUG_OTA_READY) return ESP_OK;
    atomic_store(&state, DEBUG_OTA_STARTING);
    if (xTaskCreate(start_worker, "debug_start", 6144, NULL, 5, NULL) != pdPASS) {
        atomic_store(&state, DEBUG_OTA_ERROR);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void stop_worker(void *argument)
{
    (void)argument;
    esp_err_t err = ESP_OK;
    if (server) { err = httpd_stop(server); if (err == ESP_OK) server = NULL; }
    if (err == ESP_OK && wifi_started) {
        err = esp_wifi_stop();
        if (err == ESP_OK) wifi_started = false;
    }
    atomic_store(&state, err == ESP_OK ? DEBUG_OTA_OFF : DEBUG_OTA_ERROR);
    atomic_store(&operation_busy, false);
    if (err != ESP_OK) ESP_LOGE(TAG, "Debug stop failed: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

esp_err_t debug_ota_stop(void)
{
    int current = atomic_load(&state);
    if (current == DEBUG_OTA_OFF || current == DEBUG_OTA_STOPPING) return ESP_OK;
    bool expected = false;
    if (current == DEBUG_OTA_STARTING || reboot_pending ||
        !atomic_compare_exchange_strong(&operation_busy, &expected, true)) return ESP_ERR_INVALID_STATE;
    atomic_store(&state, DEBUG_OTA_STOPPING);
    if (xTaskCreate(stop_worker, "debug_stop", 4096, NULL, 5, NULL) != pdPASS) {
        atomic_store(&state, current);
        atomic_store(&operation_busy, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

debug_ota_state_t debug_ota_state(void) { return (debug_ota_state_t)atomic_load(&state); }

esp_err_t debug_ota_confirm_boot(void)
{
    esp_ota_img_states_t image_state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_err_t err = esp_ota_get_state_partition(running, &image_state);
    if (err != ESP_OK) return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
    if (image_state == ESP_OTA_IMG_PENDING_VERIFY) {
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) return err;
        image_state = ESP_OTA_IMG_VALID;
    }
    return user_upgrade_finish(running->subtype, image_state == ESP_OTA_IMG_VALID);
}
