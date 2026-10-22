#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "rf_protocol.h"

// Shared by the ESP32 firmware and Windows preview. Call from the LVGL owner.
void rf_ui_create(void);
// Local USB debug command; invoke only in the LVGL owner task.
bool rf_ui_remote_receive(uint32_t frequency_hz, bool start);
void rf_ui_set_receive_options(uint8_t options);
uint8_t rf_ui_get_receive_options(void);
uint32_t rf_ui_receive_frequency(void);
// Pass a measured battery voltage in mV; zero displays unknown (--.--V).
void rf_ui_set_battery_mv(uint16_t millivolts);
// Uncalibrated ADC mean; a negative value displays unknown.
void rf_ui_set_battery_raw(int raw);
// Same bit order as the BSP: up, right, left, center, down. Rising edges flip pages.
void rf_ui_set_keys(uint32_t mask);
// Preview pointer context action, in logical display coordinates.
void rf_ui_signal_context_at(int32_t x, int32_t y);
// Brightness selected in Settings, 1..100 percent in steps of 1; default 70.
uint8_t rf_ui_get_brightness(void);
void rf_ui_set_brightness(uint8_t value);
// Minutes: 0 disables that stage; deep delay starts on entry to light sleep.
void rf_ui_set_sleep_minutes(uint8_t light, uint8_t deep);
void rf_ui_get_sleep_minutes(uint8_t *light, uint8_t *deep);
typedef enum { RF_DEBUG_OFF, RF_DEBUG_STARTING, RF_DEBUG_READY, RF_DEBUG_STOPPING, RF_DEBUG_ERROR } rf_debug_state_t;
bool rf_ui_take_debug_request(void);
void rf_ui_set_debug_state(rf_debug_state_t state);

bool rf_ui_take_radio_action(rf_command_t *command);
void rf_ui_update_radio(const rf_status_t *status);
void rf_ui_radio_error(const char *message);
