#include "bsp_backlight.h"
#include "bsp_board.h"
#include "bsp_private.h"
#include "driver/ledc.h"
#include "esp_check.h"

static const char *TAG = "bsp_backlight";
static bool initialized;

esp_err_t bsp_backlight_set(uint8_t percent)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    if (percent > 100) return ESP_ERR_INVALID_ARG;
    // Inverted output: UI 1..100 maps to pin HIGH duty 100%..50%.
    // LEDC counts the active-low portion (0..1024 of 2048 ticks).
    // Zero remains the internal backlight-off command.
    uint32_t duty = percent ? (1024U * (percent - 1U) + 49U) / 99U : 0;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t bsp_backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_11_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = BSP_BACKLIGHT_HZ,
        .clk_cfg = LEDC_USE_APB_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "PWM timer");
    const ledc_channel_config_t channel = {
        .gpio_num = BSP_LCD_LED, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0,
        .duty = 0, .hpoint = 0,
        .flags.output_invert = 1,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "PWM channel");
    initialized = true;
    return ESP_OK;
}
