#include "app.h"
#include "app_internal.h"
#include "bsp.h"
#include "bsp_radio.h"
#include "rf_ui.h"
#include "debug_ota.h"
#include "user_data.h"
#include "rf_service.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void sync_debug(lv_timer_t *timer)
{
    (void)timer;
    if (rf_ui_take_debug_request()) {
        esp_err_t err = debug_ota_state() == DEBUG_OTA_READY ? debug_ota_stop() : debug_ota_start();
        if (err != ESP_OK) ESP_LOGW("app", "Debug toggle: %s", esp_err_to_name(err));
    }
    switch (debug_ota_state()) {
        case DEBUG_OTA_OFF: rf_ui_set_debug_state(RF_DEBUG_OFF); break;
        case DEBUG_OTA_STOPPING: rf_ui_set_debug_state(RF_DEBUG_STOPPING); break;
        case DEBUG_OTA_STARTING: rf_ui_set_debug_state(RF_DEBUG_STARTING); break;
        case DEBUG_OTA_READY: rf_ui_set_debug_state(RF_DEBUG_READY); break;
        case DEBUG_OTA_ERROR: rf_ui_set_debug_state(RF_DEBUG_ERROR); break;
        default: break;
    }
}

static void create_periodic_timer(lv_timer_cb_t callback, uint32_t period_ms)
{
    lv_timer_t *timer = lv_timer_create(callback, period_ms, NULL);
    ESP_ERROR_CHECK(timer ? ESP_OK : ESP_ERR_NO_MEM);
}

static void wake_ui(void *task)
{
    // ESP_TIMER_TASK dispatch: notify only; ADC and UI work stay in app_main.
    xTaskNotifyGive((TaskHandle_t)task);
}

void app_run(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    }
    ESP_ERROR_CHECK(user_data_init());
    ESP_ERROR_CHECK(bsp_init());
    ESP_ERROR_CHECK(bsp_display_init());

    bsp_radio_info_t radio_info = {0};
    esp_err_t radio_err = bsp_radio_init(&radio_info);
    if (radio_err == ESP_OK) {
        ESP_LOGI("app", "CC1101 PARTNUM=0x%02X VERSION=0x%02X (IDLE)",
                 radio_info.partnum, radio_info.version);
    } else {
        ESP_LOGW("app", "Radio probe failed: %s; display/buttons remain available",
                 esp_err_to_name(radio_err));
    }

    ESP_ERROR_CHECK(rf_service_init(radio_err == ESP_OK));
    debug_ota_set_radio_hooks(rf_service_pause_for_ota, rf_service_resume_after_ota);
    rf_ui_create();
    app_radio_init();
    app_power_init();
    create_periodic_timer(app_power_sync_backlight, 10);
    create_periodic_timer(app_power_update_battery, 1000);
    create_periodic_timer(sync_debug, 50);
    create_periodic_timer(app_radio_sync, 50);
    create_periodic_timer(app_console_poll, 20);

    esp_timer_handle_t ui_wakeup;
    const esp_timer_create_args_t wakeup_config = {
        .callback = wake_ui, .arg = xTaskGetCurrentTaskHandle(),
        .dispatch_method = ESP_TIMER_TASK, .name = "ui_wakeup",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&wakeup_config, &ui_wakeup));
    ESP_ERROR_CHECK(esp_timer_start_periodic(ui_wakeup, 1000)); // microseconds
    ESP_ERROR_CHECK(debug_ota_confirm_boot());
    app_power_restore_settings();
    app_radio_boot_receive();
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        app_power_process_keys();
        lv_timer_handler();
        app_power_poll(ui_wakeup);
    }
}
