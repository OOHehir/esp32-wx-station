# See the note in components/app_led/CMakeLists.txt: SRCS may depend on CONFIG_*,
# requirements may not.
if(CONFIG_APP_HTTPD_ENABLE)
    set(srcs "app_httpd.c" "api.c" "json_util.c")
    if(CONFIG_APP_HTTPD_CAPTIVE_PORTAL)
        list(APPEND srcs "portal_dns.c" "dns_pack.c")
    endif()
    set(embed "www/index.html")
else()
    # The public API still exists and still links; it just does nothing, so callers do
    # not need their own #ifdefs to compile the web UI out.
    set(srcs "app_httpd_disabled.c")
    set(embed "")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES app_cfg app_diag app_led app_net esp_http_server esp_netif
                  esp_system esp_timer espressif__cjson lwip
    EMBED_FILES ${embed})
