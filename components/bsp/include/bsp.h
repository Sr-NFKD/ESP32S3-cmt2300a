#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Board facade. Individual drivers may include only their peripheral header.
#include "bsp_backlight.h"
#include "bsp_battery.h"
#include "bsp_buttons.h"
#include "bsp_sleep.h"
#include "bsp_display.h"

// Enables shared power, configures buttons, then initializes backlight PWM.
esp_err_t bsp_init(void);
