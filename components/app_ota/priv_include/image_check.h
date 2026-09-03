/* Hash what actually landed in the passive slot, and compare it with the manifest.
 *
 * PLAN §4: where this happens is the point -- *after* the last byte is written and
 * *before* esp_https_ota_finish(), which is the call that marks the slot bootable. A
 * hash checked after that would need undoing; checked here, a mismatch is just an abort
 * and the device carries on running what it was running.
 *
 * Reading the flash back rather than hashing the TLS stream is deliberate too -- it is
 * the bytes in the slot that will be executed, not the bytes that came off the socket,
 * and esp_https_ota gives no hook to see the stream anyway (ESP_HTTPS_OTA_WRITE_FLASH
 * carries a length, not the data).
 *
 * This is integrity, not authenticity: the hash and the image come from the same server,
 * so it catches a truncated upload or a corrupted transfer, not a hostile one. Image
 * signing is what covers that, and it is a separate, independent check.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_partition.h"
#include "sha_hex.h"

/* Hashes the first `len` bytes of `part`. `hex_out` receives the digest as text even on
 * mismatch, so the log and the status page can show what was actually there. */
esp_err_t image_check_sha256(const esp_partition_t *part, size_t len,
                             const char *expect_hex,
                             char *hex_out, size_t hex_out_len);
