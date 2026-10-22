idf_component_register(SRCS "bsp.c" "bsp_buttons.c" "bsp_backlight.c" "bsp_display.c" "bsp_radio.c" "bsp_battery.c" "bsp_sleep.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_spi esp_lcd esp_timer esp_adc esp_hw_support lvgl__lvgl)
