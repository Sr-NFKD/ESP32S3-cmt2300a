#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#include "lvgl.h"
// LVGL is owned by the calling task; UI calls and timer handling stay in it.
esp_err_t bsp_display_init(void);
lv_display_t *bsp_display_get(void);
esp_err_t bsp_display_sleep(bool sleeping);
lv_indev_t *bsp_keypad_get(void);
