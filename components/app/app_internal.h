#pragma once
#include "lvgl.h"
#include "esp_timer.h"

// All controller functions execute on the application/LVGL owner task.
void app_radio_init(void);
void app_radio_boot_receive(void);
void app_radio_sync(lv_timer_t *timer);
void app_console_poll(lv_timer_t *timer);
void app_power_init(void);
void app_power_restore_settings(void);
void app_power_activity(void);
void app_power_process_keys(void);
void app_power_poll(esp_timer_handle_t ui_wakeup);
void app_power_sync_backlight(lv_timer_t *timer);
void app_power_update_battery(lv_timer_t *timer);
