idf_component_register(SRCS "rf_service.c" INCLUDE_DIRS "include"
    REQUIRES rf_protocol
    PRIV_REQUIRES bsp user_data esp_driver_rmt esp_timer json)
