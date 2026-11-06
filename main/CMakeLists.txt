idf_component_register(SRCS "main.c" "sequence.c" "config_json.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_wifi esp_netif esp_event esp_http_server nvs_flash esp_timer
    EMBED_TXTFILES "index.html")
