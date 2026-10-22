#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
typedef enum {
    BSP_KEY_UP = 0, BSP_KEY_RIGHT, BSP_KEY_LEFT, BSP_KEY_PRESS, BSP_KEY_DOWN,
    BSP_KEY_COUNT
} bsp_key_t;

// Interrupt-driven debounce task; owner is notified when an event is queued.
esp_err_t bsp_buttons_start(TaskHandle_t owner);
// Consume in the LVGL owner task, before handling LVGL timers.
bool bsp_buttons_take_event(uint32_t *mask);
uint32_t bsp_buttons_pressed(void); // Bit (1 << bsp_key_t), debounced.
uint32_t bsp_buttons_raw(void);
void bsp_buttons_suspend(void);
void bsp_buttons_resume(void);
