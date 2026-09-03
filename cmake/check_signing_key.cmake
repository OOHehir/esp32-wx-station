# Fail the configure step, with the command that fixes it, when signed images are
# enabled but the signing key is not there.
#
# Without this the build dies deep inside esptool with a path it was handed and no
# suggestion of where that path was supposed to come from -- and the answer ("run this
# one script") is not guessable. Same reasoning as cmake/check_partitions.cmake: a
# configuration mistake should stop the build and say what to do about it.

if(NOT CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES)
    return()
endif()

set(_key "${CONFIG_SECURE_BOOT_SIGNING_KEY}")
if(NOT IS_ABSOLUTE "${_key}")
    # ESP-IDF evaluates the path relative to the project directory.
    set(_key "${CMAKE_CURRENT_LIST_DIR}/../${_key}")
endif()

if(EXISTS "${_key}")
    message(STATUS "check_signing_key: signing with ${CONFIG_SECURE_BOOT_SIGNING_KEY}")
    return()
endif()

message(FATAL_ERROR
    "Signed images are enabled but the signing key is missing:\n"
    "    ${CONFIG_SECURE_BOOT_SIGNING_KEY}\n\n"
    "Generate it once, then build again:\n"
    "    ./tools/gen_signing_key.sh\n\n"
    "The key is never committed -- it is the one secret that makes an OTA image "
    "trustworthy to devices already in the field. To build without signing instead, "
    "set CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=n, and understand that a device "
    "then installs whatever the file server offers it.")
