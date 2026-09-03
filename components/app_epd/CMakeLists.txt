idf_component_register(
    SRCS "app_epd.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_driver_gpio esp_driver_spi esp_timer)
