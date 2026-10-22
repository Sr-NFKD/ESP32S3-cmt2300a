#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// UI 1..100 -> GPIO high-duty 100%..50% (active-low backlight); 0 = off.
esp_err_t bsp_backlight_set(uint8_t percent);
