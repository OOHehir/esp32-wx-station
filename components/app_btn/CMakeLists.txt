idf_component_register(
    SRCS "app_btn.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_display app_ota app_httpd esp_driver_gpio esp_timer esp_event)
