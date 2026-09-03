# See components/app_led/CMakeLists.txt: SRCS may depend on CONFIG_*, requirements may not.
if(CONFIG_APP_OTA_ENABLE)
    set(srcs "app_ota.c" "manifest.c" "update_policy.c" "rollback_policy.c"
             "ota_record.c" "sha_hex.c" "image_check.c" "selftest.c")
else()
    set(srcs "app_ota_disabled.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES app_cfg app_diag app_httpd app_led app_net app_trust
                  app_update esp_app_format esp_event esp_http_client esp_https_ota
                  esp_partition esp_timer espressif__cjson mbedtls nvs_flash
    )
