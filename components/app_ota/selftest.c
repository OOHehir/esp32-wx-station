/* The self-test a new image has to pass before the rollback is cancelled.
 *
 * PLAN §4: "the new image boots in PENDING_VERIFY and only calls
 * esp_ota_mark_app_valid_cancel_rollback() after a self-test passes (network up, NAS
 * reachable, N minutes of uptime, no panic). Otherwise the bootloader reverts."
 *
 * Three of those four are checked here. The fourth -- no panic -- needs no code: an
 * image that panics or trips a watchdog while in PENDING_VERIFY is reverted by the
 * bootloader on the way back up, before any of this runs. That is why the state exists,
 * and why the only failure this file has to handle is the quiet one: an image that runs
 * happily and simply cannot be reached or updated any more.
 *
 * Which is the criterion that actually matters. Confirming an image is a promise that a
 * fix can still reach the device; the server probe is the only check that tests that
 * promise directly, and everything else here is a precondition for it.
 */
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_diag.h"
#include "app_net.h"
#include "esp_log.h"
#include "ota_internal.h"
#include "sdkconfig.h"

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE

static const char *TAG = "app_ota";

#define POLL_MS       2000
#define MANIFEST_MAX  1024

#if CONFIG_APP_OTA_SELFTEST_REQUIRE_SERVER
/* Fetch the manifest and throw the body away. What is being tested is the whole path --
 * association, DHCP, DNS, the clock, the embedded CA, the server -- not its content. A
 * device that can do this can be updated, which is the property being confirmed. */
static bool server_reachable(void)
{
    char url[256];
    if (ota_build_manifest_url(url, sizeof(url)) != ESP_OK) {
        ESP_LOGE(TAG, "self-test: manifest url too long");
        return false;
    }

    char *body = malloc(MANIFEST_MAX);
    if (!body) {
        ESP_LOGW(TAG, "self-test: out of memory for the server probe");
        return false;
    }
    esp_err_t err = ota_fetch_manifest(url, body, MANIFEST_MAX);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "self-test: %s unreachable: %s", url, esp_err_to_name(err));
        return false;
    }
    return true;
}
#endif

bool ota_selftest_run(void)
{
    const uint64_t started = app_diag_uptime_s();
    const uint64_t deadline = started + CONFIG_APP_OTA_SELFTEST_TIMEOUT_S;
#if CONFIG_APP_OTA_SELFTEST_REQUIRE_SERVER
    uint64_t next_probe = 0;
#endif

    ota_set_state(APP_OTA_SELFTEST, "on probation -- proving this image");

    for (;;) {
        const uint64_t now = app_diag_uptime_s();
        ota_set_selftest_left(now < deadline ? (int32_t)(deadline - now) : 0);

        /* Uptime first: it is the cheapest, and it is the one criterion that cannot be
         * satisfied early no matter how well everything else goes. */
        if (now < (uint64_t)CONFIG_APP_OTA_SELFTEST_MIN_UPTIME_S) {
            goto wait;
        }
        if (app_net_state() != APP_NET_STATE_CONNECTED) {
            goto wait;
        }
#if CONFIG_APP_OTA_REQUIRE_TIME_SYNC
        if (!app_net_time_synced()) {
            goto wait;   /* TLS cannot check a certificate on a device that thinks it is 1970 */
        }
#endif

#if CONFIG_APP_OTA_SELFTEST_REQUIRE_SERVER
        if (now >= next_probe) {
            ota_set_state(APP_OTA_SELFTEST, "checking that the update server is reachable");
            if (server_reachable()) {
                ESP_LOGI(TAG, "self-test passed after %llus",
                         (unsigned long long)(app_diag_uptime_s() - started));
                return true;
            }
            /* Back off rather than hammering a server that is merely rebooting. */
            next_probe = app_diag_uptime_s() + CONFIG_APP_OTA_SELFTEST_RETRY_S;
            ota_set_state(APP_OTA_SELFTEST, "update server unreachable -- retrying");
        }
#else
        ESP_LOGI(TAG, "self-test passed after %llus (server probe disabled)",
                 (unsigned long long)(now - started));
        return true;
#endif

    wait:
        if (app_diag_uptime_s() >= deadline) {
            ESP_LOGE(TAG, "self-test did not pass within %ds",
                     CONFIG_APP_OTA_SELFTEST_TIMEOUT_S);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

#endif /* CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE */
