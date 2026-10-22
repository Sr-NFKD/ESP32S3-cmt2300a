#include "bsp_sleep.h"
#include "bsp_buttons.h"

#include "bsp_board.h"
#include "driver/rtc_io.h"
#include "esp_check.h"
#include "esp_sleep.h"

static const gpio_num_t wake_pins[] = {
    BSP_BUTTON_UP, BSP_BUTTON_RIGHT, BSP_BUTTON_LEFT, BSP_BUTTON_PRESS, BSP_BUTTON_DOWN,
};
// Remove drive and pullups before cutting the rail to avoid back-powering peripherals.
static const gpio_num_t peripheral_pins[] = {
    BSP_LCD_LED, BSP_LCD_RST, BSP_LCD_DC, BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS,
    BSP_RADIO_GDO0, BSP_RADIO_CS, BSP_RADIO_SCLK, BSP_RADIO_MOSI, BSP_RADIO_MISO, BSP_RADIO_GDO2,
};

esp_err_t bsp_sleep_boot_restore(void)
{
    // Keep the rail off while releasing deep-sleep holds; bsp_init powers it up.
    ESP_RETURN_ON_ERROR(gpio_set_direction(BSP_POWER_EN, GPIO_MODE_OUTPUT), "sleep", "power direction");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_POWER_EN, 0), "sleep", "power off");
    ESP_RETURN_ON_ERROR(gpio_hold_dis(BSP_POWER_EN), "sleep", "power hold");
    for (unsigned i = 0; i < sizeof(peripheral_pins) / sizeof(peripheral_pins[0]); i++)
        ESP_RETURN_ON_ERROR(gpio_hold_dis(peripheral_pins[i]), "sleep", "peripheral hold");
    gpio_deep_sleep_hold_dis();
    for (unsigned i = 0; i < BSP_KEY_COUNT; i++) {
        ESP_RETURN_ON_ERROR(rtc_gpio_hold_dis(wake_pins[i]), "sleep", "key hold");
        ESP_RETURN_ON_ERROR(rtc_gpio_deinit(wake_pins[i]), "sleep", "key mux");
    }
    return ESP_OK;
}

static esp_err_t enable_keys(void)
{
    ESP_RETURN_ON_ERROR(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL), "sleep", "clear wake sources");
    uint64_t mask = 0;
    for (unsigned i = 0; i < BSP_KEY_COUNT; i++) {
        mask |= 1ULL << wake_pins[i];
        ESP_RETURN_ON_ERROR(rtc_gpio_pullup_en(wake_pins[i]), "sleep", "key pullup");
        ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(wake_pins[i]), "sleep", "key pulldown");
    }
    return esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
}

esp_err_t bsp_light_sleep(uint64_t delay_us)
{
    esp_err_t err = enable_keys();
    if (err == ESP_OK && delay_us) err = esp_sleep_enable_timer_wakeup(delay_us);
    if (err == ESP_OK) err = esp_light_sleep_start();
    ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
    for (unsigned i = 0; i < BSP_KEY_COUNT; i++) {
        ESP_ERROR_CHECK(rtc_gpio_hold_dis(wake_pins[i]));
        ESP_ERROR_CHECK(rtc_gpio_deinit(wake_pins[i]));
    }
    return err;
}

void bsp_deep_sleep(void)
{
    ESP_ERROR_CHECK(enable_keys()); // No timer: only a button can restart the device.
    for (unsigned i = 0; i < sizeof(peripheral_pins) / sizeof(peripheral_pins[0]); i++) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << peripheral_pins[i], .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        ESP_ERROR_CHECK(gpio_hold_en(peripheral_pins[i]));
    }
    ESP_ERROR_CHECK(gpio_set_level(BSP_POWER_EN, 0));
    ESP_ERROR_CHECK(gpio_hold_en(BSP_POWER_EN));
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();
}
