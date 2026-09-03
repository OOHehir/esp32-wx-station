idf_component_register(
    SRCS "app_display.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_epd app_gfx app_ui app_wx app_cfg app_diag app_net app_httpd esp_timer
                  mbedtls nvs_flash)
