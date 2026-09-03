#include "ota_record.h"

#include <string.h>

#include "app_cfg.h"
#include "esp_log.h"
#include "rollback_policy.h"

static const char *TAG = "app_ota";

static void load_str(const char *key, char *out, size_t out_len)
{
    if (app_cfg_get_str(key, out, out_len) != ESP_OK) {
        out[0] = '\0';
    }
}

void ota_record_load(ota_record_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    load_str(APP_CFG_KEY_OTA_TRY, out->attempted, sizeof(out->attempted));
    load_str(APP_CFG_KEY_OTA_BAD, out->failed, sizeof(out->failed));

    uint32_t n = 0;
    if (app_cfg_get_u32(APP_CFG_KEY_OTA_BAD_N, &n) == ESP_OK) {
        out->failed_count = (unsigned)n;
    }
    /* A count without a version, or a version without a count, is a half-written record
     * -- treat it as no record rather than as a quarantine nobody can explain. */
    if (out->failed[0] == '\0' || out->failed_count == 0) {
        out->failed[0] = '\0';
        out->failed_count = 0;
    }
}

void ota_record_set_attempted(const char *version)
{
    if (version && version[0]) {
        app_cfg_set_str(APP_CFG_KEY_OTA_TRY, version);
    }
}

void ota_record_clear_attempted(void)
{
    app_cfg_erase(APP_CFG_KEY_OTA_TRY);
}

unsigned ota_record_note_failure(const char *version)
{
    ota_record_t rec;
    ota_record_load(&rec);

    unsigned n = rollback_next_count(rec.failed, rec.failed_count, version);
    if (n == 0) {
        /* Nothing to attribute the failure to. Still drop the attempt so the next boot
         * does not keep re-deciding this. */
        ota_record_clear_attempted();
        return 0;
    }

    app_cfg_set_str(APP_CFG_KEY_OTA_BAD, version);
    app_cfg_set_u32(APP_CFG_KEY_OTA_BAD_N, (uint32_t)n);
    ota_record_clear_attempted();

    ESP_LOGW(TAG, "recorded failure #%u for %s", n, version);
    return n;
}

void ota_record_clear_failure(const char *version)
{
    if (!version || !version[0]) {
        return;
    }
    ota_record_t rec;
    ota_record_load(&rec);
    if (rec.failed[0] && strcmp(rec.failed, version) == 0) {
        app_cfg_erase(APP_CFG_KEY_OTA_BAD);
        app_cfg_erase(APP_CFG_KEY_OTA_BAD_N);
        ESP_LOGI(TAG, "%s ran successfully -- quarantine lifted", version);
    }
}
