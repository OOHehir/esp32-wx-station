# No REQUIRES at all: app_gfx touches no ESP-IDF API, which is what lets test/host
# compile it with plain gcc (PLAN 5b).
idf_component_register(
    SRCS "app_gfx.c" "font_big.c" "font_label.c" "font_small.c"
    INCLUDE_DIRS "include")
