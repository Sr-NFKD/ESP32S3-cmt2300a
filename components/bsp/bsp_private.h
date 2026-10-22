#pragma once
#include "esp_err.h"

// Internal startup steps, sequenced only by bsp_init().
esp_err_t bsp_buttons_init(void);
esp_err_t bsp_backlight_init(void);
