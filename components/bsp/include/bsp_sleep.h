#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t bsp_sleep_boot_restore(void);
// Called with display/RF quiesced and button task suspended. Zero = no timer.
esp_err_t bsp_light_sleep(uint64_t delay_us);
void bsp_deep_sleep(void);

