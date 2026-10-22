#include "app_internal.h"
#include "rf_ui.h"
#include "user_data.h"
#include "rf_service.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static uint8_t saved_receive_options;
static unsigned auto_save_stage;
static int64_t auto_save_due;
static uint32_t auto_receive_frequency, last_auto_frequency;
static char last_auto_code[17];

void app_radio_sync(lv_timer_t *timer)
{
    (void)timer;
    uint8_t options = rf_ui_get_receive_options();
    if (options != saved_receive_options) {
        esp_err_t err = user_receive_options_save(options);
        if (err == ESP_OK) saved_receive_options = options;
        else { rf_ui_set_receive_options(saved_receive_options); ESP_LOGW("app", "Save receive options: %s", esp_err_to_name(err)); }
    }
    rf_command_t request;
    bool user_action = rf_ui_take_radio_action(&request);
    if (user_action) {
        auto_save_stage = 0;
        esp_err_t err = rf_service_request(&request);
        if (err != ESP_OK) {
            rf_ui_radio_error(esp_err_to_name(err));
            return;
        }
    }
    rf_status_t status;
    rf_service_status(&status);
    rf_ui_update_radio(&status);
    if (user_action || !(saved_receive_options & 1)) { auto_save_stage = 0; return; }
    int64_t now = esp_timer_get_time();
    if (status.state == RF_CAPTURED) {
        if (!auto_save_stage) {
            auto_receive_frequency = rf_ui_receive_frequency();
            auto_save_due = now + 350000; auto_save_stage = 1;
        }
        if (auto_save_stage == 1 && now >= auto_save_due) {
            // Holding one decoded key must not fill the library with duplicates.
            if (status.code[0] && strncmp(status.code, "RAW", 3) &&
                status.frequency_hz == last_auto_frequency && !strcmp(status.code, last_auto_code)) {
                rf_ui_remote_receive(auto_receive_frequency, true); auto_save_stage = 0;
            } else {
                const rf_command_t save = {.type = RF_CMD_SAVE, .keep_result = true};
                esp_err_t err = rf_service_request(&save);
                if (err != ESP_OK) { rf_ui_radio_error(esp_err_to_name(err)); auto_save_stage = 0; }
                else auto_save_stage = 2;
            }
        }
    } else if (status.state == RF_SAVED && auto_save_stage >= 2) {
        if (auto_save_stage == 2) {
            auto_save_stage = 3; auto_save_due = now + 1000000;
            last_auto_frequency = status.frequency_hz;
            snprintf(last_auto_code, sizeof(last_auto_code), "%s", status.code);
        } else if (now >= auto_save_due) {
            rf_ui_remote_receive(auto_receive_frequency, true); auto_save_stage = 0;
        }
    } else auto_save_stage = 0;
}


void app_radio_init(void)
{
    saved_receive_options = user_receive_options_load();
    rf_ui_set_receive_options(saved_receive_options);
}

void app_radio_boot_receive(void)
{
    if (saved_receive_options & 2) { rf_ui_remote_receive(0, true); app_radio_sync(NULL); }
}
