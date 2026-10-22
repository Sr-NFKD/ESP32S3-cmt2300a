#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t bsp_battery_init(void);
// Voltage and rounded mean of the same 16 uncalibrated ADC samples.
esp_err_t bsp_battery_read_mv(uint16_t *millivolts, uint16_t *raw_average);
