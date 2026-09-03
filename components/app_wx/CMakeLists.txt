# wx_series.c and wx_parse.c are both pure: no ESP-IDF, and tested on the host against a
# recorded fixture (test/host/test_wx_parse.c). app_wx.c is the half that cannot be --
# a socket, a TLS handshake and an NVS blob -- and it is deliberately thin for that
# reason.
#
# APP_WX_HAS_FETCH gates the ESP-IDF half of app_wx.h, so the same header stays
# compilable with plain gcc for app_ui and the host tests.
idf_component_register(
    SRCS "wx_series.c" "wx_parse.c" "wx_location.c" "app_wx.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_http_client
    PRIV_REQUIRES espressif__cjson app_trust app_cfg nvs_flash esp_timer)

target_compile_definitions(${COMPONENT_LIB} PUBLIC APP_WX_HAS_FETCH)
