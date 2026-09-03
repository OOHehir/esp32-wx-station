# Fail the build when the selected partition table cannot fit the configured flash size.
#
# The point (PLAN §1a) is that a mismatch is caught at configure time on every target,
# not at the first OTA on a device that is already deployed. ESP-IDF's own check is a
# warning buried in build output; this one stops the build.

# Empty list elements must survive the split below. With CMP0007 unset CMake silently
# drops them, so a row with an implicit (blank) offset comes out one field short and gets
# skipped as unparseable -- which is the one case this script most needs to catch.
cmake_policy(SET CMP0007 NEW)

if(NOT CONFIG_PARTITION_TABLE_CUSTOM)
    return()
endif()

set(_table "${CMAKE_CURRENT_LIST_DIR}/../${CONFIG_PARTITION_TABLE_CUSTOM_FILENAME}")
if(NOT EXISTS "${_table}")
    # Path may be absolute or relative to the project; let ESP-IDF report the miss.
    return()
endif()

# CONFIG_ESPTOOLPY_FLASHSIZE is a string like "4MB".
string(REGEX MATCH "^([0-9]+)MB$" _m "${CONFIG_ESPTOOLPY_FLASHSIZE}")
if(NOT _m)
    message(WARNING "check_partitions: cannot parse flash size '${CONFIG_ESPTOOLPY_FLASHSIZE}'")
    return()
endif()
math(EXPR _flash_bytes "${CMAKE_MATCH_1} * 1024 * 1024")

# Walk the CSV and find the highest offset+size. Comment and blank lines are skipped;
# every partition this template ships gives an explicit offset, so no offset inference
# is needed here.
file(STRINGS "${_table}" _lines)
set(_end 0)
set(_largest_app 0)
foreach(_line ${_lines})
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "" OR _line MATCHES "^#")
        continue()
    endif()
    string(REPLACE " " "" _line "${_line}")
    string(REPLACE "," ";" _f "${_line}")
    list(LENGTH _f _n)
    if(_n LESS 5)
        continue()
    endif()
    list(GET _f 0 _name)
    list(GET _f 1 _type)
    list(GET _f 3 _off)
    list(GET _f 4 _size)
    if(NOT _off MATCHES "^0[xX][0-9a-fA-F]+$" OR NOT _size MATCHES "^0[xX][0-9a-fA-F]+$")
        message(FATAL_ERROR
            "check_partitions: '${_name}' in ${CONFIG_PARTITION_TABLE_CUSTOM_FILENAME} "
            "must give an explicit hex offset and size (got '${_off}','${_size}').")
    endif()
    math(EXPR _this "${_off} + ${_size}")
    if(_this GREATER _end)
        set(_end ${_this})
    endif()
    if(_type STREQUAL "app" AND ${_size} GREATER _largest_app)
        set(_largest_app ${_size})
    endif()
endforeach()

math(EXPR _flash_kb "${_flash_bytes} / 1024")
math(EXPR _end_kb "${_end} / 1024")

if(_end GREATER _flash_bytes)
    message(FATAL_ERROR
        "Partition table ${CONFIG_PARTITION_TABLE_CUSTOM_FILENAME} needs ${_end_kb} KB but "
        "CONFIG_ESPTOOLPY_FLASHSIZE is ${CONFIG_ESPTOOLPY_FLASHSIZE} (${_flash_kb} KB).\n"
        "Pick the matching table in sdkconfig.defaults.${CONFIG_IDF_TARGET}, or correct the "
        "flash size for the board you are actually building for.")
endif()

math(EXPR _app_kb "${_largest_app} / 1024")
message(STATUS "check_partitions: ${CONFIG_PARTITION_TABLE_CUSTOM_FILENAME} fits "
               "${CONFIG_ESPTOOLPY_FLASHSIZE} (${_end_kb}/${_flash_kb} KB used, "
               "app slot ${_app_kb} KB)")
