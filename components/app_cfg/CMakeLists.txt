idf_component_register(
    SRCS "app_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
    PRIV_REQUIRES esp_hw_support mbedtls esp_system)
