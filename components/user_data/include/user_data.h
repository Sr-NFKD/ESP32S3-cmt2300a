#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t user_data_init(void);
// Caller owns the returned array. Empty on first boot; never creates demo signals.
esp_err_t user_signals_read(cJSON **signals);
// Saves a new signal with name, key_code, optional RF metadata; returns an allocated ID.
// Keys are hexadecimal strings (up to 128 digits); metadata survives web edits.
esp_err_t user_signal_save(cJSON *signal, uint32_t *id);
esp_err_t user_signal_edit(uint32_t id, const char *name, const char *key_code);
// Observed 433 MHz RAW framing: expose the final byte without assigning meanings.
// Returns -1 for unrecognized waveforms. Does not alter persisted records.
int user_signal_button_code(const cJSON *record);
esp_err_t user_signal_delete(uint32_t id);
uint8_t user_brightness_load(void);
esp_err_t user_brightness_save(uint8_t value);
void user_sleep_load(uint8_t *light, uint8_t *deep);
esp_err_t user_sleep_save(uint8_t light, uint8_t deep);
uint8_t user_receive_options_load(void); // bit 0: auto-save, bit 1: receive at boot
esp_err_t user_receive_options_save(uint8_t options);
esp_err_t user_upgrade_stage(uint8_t partition, bool retain, bool reset);
esp_err_t user_upgrade_finish(uint8_t running_partition, bool verified);

unsigned user_signals_revision(void);
