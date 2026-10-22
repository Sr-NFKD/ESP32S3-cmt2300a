idf_component_register(SRCS "app.c" "app_power.c" "app_radio.c" "app_console.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bsp rf_ui rf_service debug_ota user_data esp_timer esp_hw_support esp_driver_usb_serial_jtag)
