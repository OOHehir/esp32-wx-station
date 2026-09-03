/* CONFIG_APP_OTA_ENABLE=n. The API still links so app_core needs no #ifdefs; with no
 * hooks registered, app_httpd reports OTA as unavailable and returns 501. */
#include "app_ota.h"

#include <string.h>

esp_err_t app_ota_init(void)  { return ESP_OK; }
bool app_ota_busy(void)       { return false; }
const char *app_ota_state_name(app_ota_state_t state) { (void)state; return "disabled"; }

esp_err_t app_ota_check_now(bool forced)
{
    (void)forced;
    return ESP_ERR_NOT_SUPPORTED;
}

void app_ota_get_info(app_ota_info_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        out->progress_pct = -1;
        out->last_check_s = -1;
        out->selftest_left_s = -1;
    }
}
