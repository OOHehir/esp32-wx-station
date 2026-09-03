/* The manifest tools/publish.sh writes:
 *
 *   { "project": "esp-ota", "chip": "esp32s3", "version": "v0.2.0",
 *     "url": "https://nas:8443/fw/esp-ota/esp32s3/v0.2.0/esp-ota.bin",
 *     "sha256": "...", "size": 882672, "mandatory": false }
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char     project[32];
    char     chip[16];
    char     version[48];
    char     url[256];
    char     sha256[65];
    uint32_t size;
    bool     mandatory;
} manifest_t;

/* Parses `json` into `out`. Missing optional fields (sha256, size, mandatory) are left
 * zeroed; missing required ones are left empty for update_decide() to reject, so there
 * is one place that decides what "required" means. */
esp_err_t manifest_parse(const char *json, manifest_t *out);
