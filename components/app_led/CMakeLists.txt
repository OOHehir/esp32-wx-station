# Only the backend the target selected is compiled -- a board with no RMT never compiles
# addressable-LED code. SRCS may be chosen by CONFIG_*, but REQUIRES may NOT: ESP-IDF
# evaluates this file once for the dependency graph before Kconfig has been read, and a
# CONFIG-guarded requirement silently resolves to nothing there.
set(srcs "app_led.c")
if(CONFIG_APP_LED_TYPE_GPIO)
    list(APPEND srcs "backend_gpio.c")
elseif(CONFIG_APP_LED_TYPE_ADDRESSABLE)
    list(APPEND srcs "backend_addressable.c")
else()
    list(APPEND srcs "backend_none.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES esp_driver_gpio espressif__led_strip)
