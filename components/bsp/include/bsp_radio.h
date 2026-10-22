#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct { uint8_t partnum; uint8_t version; } bsp_radio_info_t;

// Initializes SPI and resets the CC1101 into IDLE; does not start RX/TX.
esp_err_t bsp_radio_init(bsp_radio_info_t *info);
esp_err_t bsp_radio_strobe(uint8_t command, uint8_t *status);
esp_err_t bsp_radio_read_reg(uint8_t address, uint8_t *value);
esp_err_t bsp_radio_write_reg(uint8_t address, uint8_t value);
// Config-register bursts or FIFO/PATABLE accesses; maximum 64 bytes.
esp_err_t bsp_radio_read_burst(uint8_t address, uint8_t *data, size_t length);
esp_err_t bsp_radio_write_burst(uint8_t address, const uint8_t *data, size_t length);
