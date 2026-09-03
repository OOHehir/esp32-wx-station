idf_component_register(
    SRCS "app_diag.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_cfg app_update espcoredump esp_app_format esp_hw_support
                  esp_partition esp_system esp_timer)
