idf_component_register(SRCS "debug_ota.c" INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server app_update
             user_data rf_protocol esp_timer esp_app_format bootloader_support json
    EMBED_TXTFILES "web/index.html")
