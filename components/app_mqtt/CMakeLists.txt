# See components/app_led/CMakeLists.txt: SRCS may depend on CONFIG_*, requirements may not.
if(CONFIG_APP_MQTT_ENABLE)
    set(srcs "app_mqtt.c" "mqtt_cmd.c")
else()
    set(srcs "app_mqtt_disabled.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES app_cfg app_diag app_display app_led app_net app_ota app_trust
                  esp_event esp_timer espressif__mqtt)
