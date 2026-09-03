idf_component_register(
    SRCS "app_core.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_cfg app_diag app_httpd app_led app_mqtt app_net app_ota
                  esp_event esp_netif nvs_flash)
