#include "app_cfg.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_cfg";

#define NAMESPACE CONFIG_APP_CFG_NAMESPACE

static char s_device_id[7];
static char s_device_name[APP_CFG_DEVICE_NAME_MAX];
static char s_ap_pass[APP_CFG_AP_PASS_LEN + 1];
static bool s_ready;

/* Passphrase alphabet: no 0/O/1/I/l, because these get read off a label and typed into
 * a phone. 32 symbols, so each character consumes exactly 5 bits of the digest. */
static const char PASS_ALPHABET[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ#";
_Static_assert(sizeof(PASS_ALPHABET) - 1 == 32,
               "the alphabet must hold exactly 32 symbols for the 5-bit mask below");

static void derive_identity(void)
{
    uint8_t mac[6] = {0};
    /* The station MAC is the one printed on Espressif modules, so it is the one a human
     * can check against the label. */
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac: %s -- identity will not be unique", esp_err_to_name(err));
    }

    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X", mac[3], mac[4], mac[5]);

    /* Lowercase: this becomes an mDNS hostname, and mixed-case DNS labels are a source
     * of resolver-specific surprises. */
    snprintf(s_device_name, sizeof(s_device_name), "%s-%02x%02x%02x",
             CONFIG_APP_PROJECT_NAME_SLUG, mac[3], mac[4], mac[5]);

    if (strlen(CONFIG_APP_CFG_AP_PASS_FIXED) > 0) {
        strlcpy(s_ap_pass, CONFIG_APP_CFG_AP_PASS_FIXED, sizeof(s_ap_pass));
        return;
    }

    /* ESP-IDF v6.0 ships mbedTLS 4, where the mbedtls_sha256_* API is gone and hashing
     * goes through PSA. This is one of the v6 breaking changes PLAN §1 pinned for. */
    uint8_t salted[sizeof(mac) + 32];
    size_t salt_len = strlen(CONFIG_APP_CFG_AP_PASS_SALT);
    if (salt_len > sizeof(salted) - sizeof(mac)) {
        salt_len = sizeof(salted) - sizeof(mac);
    }
    memcpy(salted, CONFIG_APP_CFG_AP_PASS_SALT, salt_len);
    memcpy(salted + salt_len, mac, sizeof(mac));

    uint8_t digest[32] = {0};
    size_t digest_len = 0;
    psa_status_t st = psa_crypto_init();
    if (st == PSA_SUCCESS) {
        st = psa_hash_compute(PSA_ALG_SHA_256, salted, salt_len + sizeof(mac),
                              digest, sizeof(digest), &digest_len);
    }
    if (st != PSA_SUCCESS || digest_len < APP_CFG_AP_PASS_LEN) {
        /* Falling back to the raw MAC would make the passphrase derivable from the SSID,
         * which is the one property this is here to avoid. Refuse instead, and let the
         * fixed-passphrase Kconfig option be the deliberate way to get a known one. */
        ESP_LOGE(TAG, "psa hash failed (%d) -- no SoftAP passphrase derived", (int)st);
        s_ap_pass[0] = '\0';
        return;
    }

    for (int i = 0; i < APP_CFG_AP_PASS_LEN; i++) {
        s_ap_pass[i] = PASS_ALPHABET[digest[i] & 0x1f];
    }
    s_ap_pass[APP_CFG_AP_PASS_LEN] = '\0';
}

esp_err_t app_cfg_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    derive_identity();
    s_ready = true;
    ESP_LOGI(TAG, "device %s (id %s)", s_device_name, s_device_id);
    return ESP_OK;
}

const char *app_cfg_device_id(void)     { return s_device_id; }
const char *app_cfg_device_name(void)   { return s_device_name; }
const char *app_cfg_ap_passphrase(void) { return s_ap_pass; }

esp_err_t app_cfg_get_str(const char *key, char *out, size_t out_len)
{
    if (!key || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* An untouched namespace does not exist yet; that is "not found", not a fault. */
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NVS_NOT_FOUND : err;
    }
    size_t len = out_len;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return err;
}

esp_err_t app_cfg_set_str(const char *key, const char *value)
{
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t app_cfg_get_u32(const char *key, uint32_t *out)
{
    if (!key || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NVS_NOT_FOUND : err;
    }
    err = nvs_get_u32(h, key, out);
    nvs_close(h);
    return err;
}

esp_err_t app_cfg_set_u32(const char *key, uint32_t value)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t app_cfg_erase(const char *key)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;   /* erasing what was never there is a success, not a fault */
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool app_cfg_has(const char *key)
{
    char probe[2];
    esp_err_t err = app_cfg_get_str(key, probe, sizeof(probe));
    /* A value longer than the probe buffer still proves the key exists. */
    return err == ESP_OK || err == ESP_ERR_NVS_INVALID_LENGTH;
}

esp_err_t app_cfg_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGW(TAG, "factory reset: namespace '%s' erased", NAMESPACE);
    return err;
}

void app_cfg_factory_reset_and_restart(void)
{
    app_cfg_factory_reset();
    ESP_LOGW(TAG, "restarting into unprovisioned state");
    /* Give the log (and any HTTP response already queued) a moment to drain. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
