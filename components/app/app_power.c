#include "app_internal.h"
#include "bsp_backlight.h"
#include "bsp_battery.h"
#include "bsp_buttons.h"
#include "bsp_display.h"
#include "bsp_sleep.h"
#include "rf_ui.h"
#include "debug_ota.h"
#include "user_data.h"
#include "rf_service.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/usb_serial_jtag.h"

static uint8_t applied_brightness, saved_brightness;
static uint32_t brightness_changed;
static bool battery_ready;
static esp_err_t battery_last_error;
static uint8_t saved_light_minutes, saved_deep_minutes;
static uint8_t pending_light_minutes, pending_deep_minutes;
static int64_t sleep_setting_changed, last_activity;
static bool suppress_wake_key;
static void sync_sleep_settings(void)
{
    uint8_t light, deep;
    rf_ui_get_sleep_minutes(&light, &deep);
    if (light != pending_light_minutes || deep != pending_deep_minutes) {
        pending_light_minutes = light; pending_deep_minutes = deep;
        sleep_setting_changed = esp_timer_get_time();
    }
    if ((light != saved_light_minutes || deep != saved_deep_minutes) &&
        esp_timer_get_time() - sleep_setting_changed >= 1000000) {
        esp_err_t err = user_sleep_save(light, deep);
        if (err == ESP_OK) { saved_light_minutes = light; saved_deep_minutes = deep; }
        else { ESP_LOGW("app", "Save sleep settings: %s", esp_err_to_name(err)); sleep_setting_changed = esp_timer_get_time(); }
    }
}

static void sleep_if_idle(esp_timer_handle_t ui_wakeup)
{
    int64_t now = esp_timer_get_time();
    // Do not interrupt the management server or an OTA write with sleep.
    if (usb_serial_jtag_is_connected() || bsp_buttons_raw() || debug_ota_state() != DEBUG_OTA_OFF) {
        last_activity = now; return;
    }
    if (!pending_light_minutes || now - last_activity < (int64_t)pending_light_minutes * 60000000) return;
    rf_status_t radio_status;
    rf_service_status(&radio_status);
    if (radio_status.last_signal_us > last_activity) last_activity = radio_status.last_signal_us;
    if (now - last_activity < (int64_t)pending_light_minutes * 60000000) return;
    if (pending_light_minutes != saved_light_minutes || pending_deep_minutes != saved_deep_minutes) return;

    esp_err_t err = rf_service_sleep(true);
    if (err != ESP_OK) {
        ESP_LOGW("app", "RF sleep: %s", esp_err_to_name(err)); last_activity = now; return;
    }
    ESP_ERROR_CHECK(esp_timer_stop(ui_wakeup));
    bsp_buttons_suspend();
    ESP_ERROR_CHECK(bsp_backlight_set(0));
    err = bsp_display_sleep(true); // Waits for outstanding screen DMA first.
    if (err == ESP_OK && !bsp_buttons_raw()) {
        ESP_LOGI("app", "Light sleep; deep sleep in %u min (0=disabled)", pending_deep_minutes);
        err = bsp_light_sleep((uint64_t)pending_deep_minutes * 60000000ULL);
        if (err == ESP_OK && pending_deep_minutes &&
            esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER && !bsp_buttons_raw()) {
            ESP_LOGI("app", "Deep sleep; peripheral power off");
            bsp_deep_sleep();
        }
    }
    if (err != ESP_OK) ESP_LOGW("app", "Light sleep: %s", esp_err_to_name(err));
    ESP_ERROR_CHECK(bsp_display_sleep(false));
    err = rf_service_sleep(false);
    if (err != ESP_OK) rf_ui_radio_error(esp_err_to_name(err));
    rf_ui_set_keys(0);
    // Restore the screen immediately; consume the wake press through its release.
    suppress_wake_key = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(bsp_display_get());
    ESP_ERROR_CHECK(bsp_backlight_set(applied_brightness));
    last_activity = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_timer_start_periodic(ui_wakeup, 1000));
}

void app_power_sync_backlight(lv_timer_t *timer)
{
    (void)timer;
    uint8_t requested = rf_ui_get_brightness();
    if (requested != applied_brightness) {
        ESP_ERROR_CHECK(bsp_backlight_set(requested));
        applied_brightness = requested;
        brightness_changed = lv_tick_get();
    }
    if (saved_brightness != requested && lv_tick_elaps(brightness_changed) >= 1000) {
        esp_err_t err = user_brightness_save(requested);
        if (err == ESP_OK) saved_brightness = requested;
        else { ESP_LOGW("app", "Save brightness: %s", esp_err_to_name(err)); brightness_changed = lv_tick_get(); }
    }
}

void app_power_update_battery(lv_timer_t *timer)
{
    (void)timer;
    if (!battery_ready) return;
    uint16_t millivolts, raw_average;
    esp_err_t err = bsp_battery_read_mv(&millivolts, &raw_average);
    rf_ui_set_battery_mv(err == ESP_OK ? millivolts : 0);
    rf_ui_set_battery_raw(err == ESP_OK ? raw_average : -1);
    if (err != ESP_OK && err != battery_last_error)
        ESP_LOGW("app", "Battery read failed: %s", esp_err_to_name(err));
    battery_last_error = err;
}


void app_power_init(void)
{
    user_sleep_load(&saved_light_minutes, &saved_deep_minutes);
    pending_light_minutes = saved_light_minutes; pending_deep_minutes = saved_deep_minutes;
    rf_ui_set_sleep_minutes(saved_light_minutes, saved_deep_minutes);
    rf_ui_set_brightness(user_brightness_load());
    battery_last_error = bsp_battery_init();
    battery_ready = battery_last_error == ESP_OK;
    if (battery_ready) app_power_update_battery(NULL);
    else ESP_LOGW("app", "Battery measurement unavailable: %s", esp_err_to_name(battery_last_error));
    lv_refr_now(bsp_display_get());
    applied_brightness = rf_ui_get_brightness();
    ESP_ERROR_CHECK(bsp_backlight_set(applied_brightness));

    ESP_ERROR_CHECK(bsp_buttons_start(xTaskGetCurrentTaskHandle()));
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        bsp_buttons_suspend();
        suppress_wake_key = true;
    }
}

void app_power_restore_settings(void)
{
    saved_brightness = user_brightness_load();
    rf_ui_set_brightness(saved_brightness);
    last_activity = esp_timer_get_time();
}

void app_power_activity(void)
{
    last_activity = esp_timer_get_time();
}

void app_power_process_keys(void)
{
    if (suppress_wake_key) {
        last_activity = esp_timer_get_time();
        if (!bsp_buttons_raw()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            if (!bsp_buttons_raw()) {
                bsp_buttons_resume();
                suppress_wake_key = false;
            }
        }
    }
    uint32_t keys;
    while (bsp_buttons_take_event(&keys)) {
        last_activity = esp_timer_get_time();
        if (suppress_wake_key) continue;
        rf_ui_set_keys(keys);
        app_radio_sync(NULL); // Dispatch before another queued key can replace the action.
    }
}

void app_power_poll(esp_timer_handle_t ui_wakeup)
{
    sync_sleep_settings();
    if (!suppress_wake_key) sleep_if_idle(ui_wakeup);
}
