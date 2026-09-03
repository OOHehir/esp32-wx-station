# esp-ota — reusable ESP-IDF project template with OTA.
#
# Chip-agnostic by construction: nothing here names a target. Choose one with
#   idf.py set-target esp32s3 && idf.py build
# and per-target settings come from sdkconfig.defaults.<target>.

cmake_minimum_required(VERSION 3.16)

# PROJECT_VER comes from git tags, so the version a device reports over OTA is the
# version in the repository. `--dirty` is deliberate: an image built from a dirty tree
# should be visibly different from the tag it claims.
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE APP_GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT APP_GIT_VERSION)
    set(APP_GIT_VERSION "0.0.0-nogit")
endif()
set(PROJECT_VER "${APP_GIT_VERSION}")

# execute_process runs at *configure* time, so without this a commit or a new tag does
# not reach the image -- `idf.py build` reuses whatever version CMake last computed. A
# stale version in an OTA manifest is worse than a wrong one: the device compares it and
# concludes it is already up to date.
#
# Watching these files makes a commit, a tag or a `git add` trigger a reconfigure. It
# does not catch an unstaged edit flipping the `-dirty` suffix; tools/publish.sh warns
# about a dirty version separately, which is where that matters.
# refs/tags is watched as a directory: a new or moved lightweight tag creates a loose
# ref file in there and touches the directory mtime, which packed-refs alone misses --
# and a tag is exactly what turns a build into a release.
foreach(git_file HEAD index packed-refs refs/tags)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git/${git_file}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${CMAKE_CURRENT_LIST_DIR}/.git/${git_file}")
    endif()
endforeach()

# sdkconfig.local -- this working tree's own settings, and the place for anything that
# must not be committed. Broker credentials are the case that prompted it: they are
# site-specific like the OTA base URL, but unlike a URL they are a secret, and a template
# meant to be copied must not carry one developer's password in its git history.
#
# ESP-IDF loads "<entry>.<target>" for every entry in SDKCONFIG_DEFAULTS, so this layers
# exactly like the tracked files and gives sdkconfig.local.<target> for free:
#
#   sdkconfig.defaults -> sdkconfig.defaults.<target> -> sdkconfig.local -> sdkconfig.local.<target>
#
# Later wins. An explicit -DSDKCONFIG_DEFAULTS on the command line is left alone, so the
# bench and CI scripts that pass their own list keep full control.
if(NOT SDKCONFIG_DEFAULTS AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.local")
    message(STATUS "sdkconfig.local found -- layering it over sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32-wx-station)

# CONFIG_* only exist once project() has processed sdkconfig, so these have to come last.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/check_partitions.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/check_signing_key.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/check_server_cert.cmake)
