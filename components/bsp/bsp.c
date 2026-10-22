#include "bsp.h"
#include "bsp_board.h"
#include "bsp_private.h"
#include "esp_check.h"

static const char *TAG = "bsp";
static bool initialized;

esp_err_t bsp_init(void)
{
    if (initialized) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(bsp_sleep_boot_restore(), TAG, "restore sleep pins");
    const gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << BSP_POWER_EN) | (1ULL << BSP_LCD_LED) |
                        (1ULL << BSP_LCD_CS) | (1ULL << BSP_LCD_RST) |
                        (1ULL << BSP_RADIO_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "outputs");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_LED, 1), TAG, "backlight off");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_CS, 1), TAG, "LCD deselect");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_RADIO_CS, 1), TAG, "radio deselect");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_RST, 0), TAG, "hold LCD reset");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_POWER_EN, 1), TAG, "power enable");
    vTaskDelay(pdMS_TO_TICKS(BSP_POWER_SETTLE_MS));

    ESP_RETURN_ON_ERROR(bsp_buttons_init(), TAG, "buttons");
    ESP_RETURN_ON_ERROR(bsp_backlight_init(), TAG, "backlight");
    initialized = true;
    return ESP_OK;
}

