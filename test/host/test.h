/* A test harness small enough that it needs no explanation and no dependency.
 *
 * These tests run on the build machine with plain gcc -- no ESP-IDF, no toolchain, no
 * hardware. That is the whole point: the units under test are the ones whose bugs are
 * silent on a device (a phone that never shows the setup page, an SSID that breaks the
 * status page), and catching them here costs a second rather than a flash cycle.
 */
#pragma once

#include <stdio.h>
#include <string.h>

extern int tests_run, tests_failed;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        tests_run++;                                                          \
        if (!(cond)) {                                                        \
            tests_failed++;                                                   \
            printf("  FAIL  %s:%d  ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

#define CHECK_STR(got, want)                                                  \
    CHECK(strcmp((got), (want)) == 0, "got \"%s\", want \"%s\"", (got), (want))

#define CHECK_INT(got, want)                                                  \
    do {                                                                      \
        long g_ = (long)(got), w_ = (long)(want);                             \
        CHECK(g_ == w_, "got %ld, want %ld", g_, w_);                         \
    } while (0)

#define SUITE(name) printf("%s\n", name)
