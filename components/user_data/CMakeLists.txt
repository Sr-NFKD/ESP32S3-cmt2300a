idf_component_register(SRCS "user_data.c" INCLUDE_DIRS "include"
    REQUIRES json PRIV_REQUIRES nvs_flash rf_protocol)
