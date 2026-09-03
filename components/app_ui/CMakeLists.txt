# No I/O, so no ESP-IDF requirements beyond the two components whose headers it uses.
idf_component_register(
    SRCS "app_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES app_gfx app_wx)
