/* Shared between the app_ota translation units. Not part of the public API. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "app_ota.h"
#include "esp_err.h"

/* <base>/<project>/<chip>/manifest.json -- exactly the layout tools/publish.sh writes. */
esp_err_t ota_build_manifest_url(char *out, size_t out_len);

/* Fetch a manifest into `buf`. Small, bounded, read in one go. */
esp_err_t ota_fetch_manifest(const char *url, char *buf, size_t buf_len);

/* Publish a state and a one-line reason. `fmt` may be NULL. */
void ota_set_state(app_ota_state_t state, const char *fmt, ...);

/* How long the self-test has left, for the status page. -1 when it is not running. */
void ota_set_selftest_left(int32_t seconds);

/* Runs on the OTA task at start-up when the running image is on probation. Returns true
 * if the image proved itself, false if the caller should roll back. Implemented in
 * selftest.c. */
bool ota_selftest_run(void);
