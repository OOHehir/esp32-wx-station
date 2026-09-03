#include "image_check.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "psa/crypto.h"

static const char *TAG = "app_ota";

/* Big enough that a megabyte is a few hundred reads, small enough to allocate while
 * mbedTLS still holds its own buffers. */
#define CHUNK 4096

esp_err_t image_check_sha256(const esp_partition_t *part, size_t len,
                             const char *expect_hex,
                             char *hex_out, size_t hex_out_len)
{
    if (!part || len == 0 || !hex_out || hex_out_len <= SHA_HEX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    hex_out[0] = '\0';

    if (!sha_hex_valid(expect_hex)) {
        /* Distinguishable from a mismatch on purpose: the caller decides whether a
         * manifest without a usable hash is tolerable (CONFIG_APP_OTA_REQUIRE_SHA256). */
        return ESP_ERR_NOT_FOUND;
    }
    if (len > part->size) {
        ESP_LOGE(TAG, "image claims %u bytes, %s holds %u",
                 (unsigned)len, part->label, (unsigned)part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = malloc(CHUNK);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    /* ESP-IDF v6.0 ships mbedTLS 4: the mbedtls_sha256_* API is gone and hashing goes
     * through PSA (PLAN §7a). Multipart, because the image does not fit in RAM. */
    esp_err_t err = ESP_OK;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_status_t st = psa_crypto_init();
    if (st == PSA_SUCCESS) {
        st = psa_hash_setup(&op, PSA_ALG_SHA_256);
    }
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa hash setup failed (%d)", (int)st);
        free(buf);
        return ESP_FAIL;
    }

    for (size_t off = 0; off < len; ) {
        size_t n = (len - off < CHUNK) ? (len - off) : CHUNK;
        err = esp_partition_read(part, off, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "read %s at 0x%x: %s", part->label, (unsigned)off,
                     esp_err_to_name(err));
            goto out;
        }
        if (psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            err = ESP_FAIL;
            goto out;
        }
        off += n;
    }

    uint8_t digest[32];
    size_t digest_len = 0;
    if (psa_hash_finish(&op, digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
        digest_len != sizeof(digest)) {
        err = ESP_FAIL;
        goto out;
    }

    sha_hex_format(digest, hex_out);
    free(buf);
    return sha_hex_equal(hex_out, expect_hex) ? ESP_OK : ESP_ERR_INVALID_CRC;

out:
    psa_hash_abort(&op);
    free(buf);
    return err;
}
