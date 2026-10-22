#pragma once
#include "esp_err.h"

#define DEBUG_WIFI_SSID "RF-Manager"
#define DEBUG_WIFI_PASSWORD "rfmanager123"
#define DEBUG_OTA_URL "http://192.168.4.1"

typedef enum { DEBUG_OTA_OFF, DEBUG_OTA_STARTING, DEBUG_OTA_READY, DEBUG_OTA_STOPPING, DEBUG_OTA_ERROR } debug_ota_state_t;

// Starts a worker task; Wi-Fi remains off until this is called.
esp_err_t debug_ota_start(void);
esp_err_t debug_ota_stop(void);
debug_ota_state_t debug_ota_state(void);
// Mark an OTA boot valid only after the board/UI initialization succeeds.
esp_err_t debug_ota_confirm_boot(void);
// Called from HTTP task before any OTA flash write; resume on failed upload only.
void debug_ota_set_radio_hooks(esp_err_t (*pause)(void), void (*resume)(void));
