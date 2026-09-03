idf_component_register(
    SRCS "app_net.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_netif
    PRIV_REQUIRES app_cfg esp_wifi esp_timer nvs_flash lwip espressif__mdns)
