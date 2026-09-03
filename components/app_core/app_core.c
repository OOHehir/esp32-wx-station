#include "app_core.h"

#include "app_cfg.h"
#include "app_diag.h"
#include "app_httpd.h"
#include "app_led.h"
#include "app_mqtt.h"
#include "app_net.h"
#include "app_ota.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "app_core";

/* Network state decides what the LED says. app_net does not know a LED exists and
 * app_led does not know what a network is; this handler is the only thing that knows
 * both, which is what keeps either replaceable. */
static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;

    if (id != APP_NET_EVENT_STATE_CHANGED) {
        return;
    }
    switch (app_net_state()) {
    case APP_NET_STATE_UNPROVISIONED:
    case APP_NET_STATE_FALLBACK:
        app_led_set_status(APP_LED_STATUS_PORTAL);
        break;
    case APP_NET_STATE_CONNECTING:
        app_led_set_status(APP_LED_STATUS_CONNECTING);
        break;
    case APP_NET_STATE_CONNECTED:
        app_led_set_status(APP_LED_STATUS_CONNECTED);
        break;
    }
}

static esp_err_t nvs_bring_up(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* A partition-table change or a version bump left NVS unreadable. Erasing loses
         * stored credentials, which drops the device into provisioning mode -- recoverable
         * from the portal, unlike a boot loop. */
        ESP_LOGW(TAG, "nvs unusable (%s) -- erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t app_core_start(void)
{
    /* Order matters and is the reason this function exists:
     *   NVS      -- app_cfg and the Wi-Fi driver both read it
     *   event    -- everything below posts to the default loop
     *   netif    -- app_net creates interfaces on it
     *   app_cfg  -- derives the identity that app_net and app_httpd both use
     *   app_diag -- counts this boot, which needs app_cfg
     *   app_led  -- up before app_net so the very first state change is visible
     *   app_net  -- starts the radio; returns without waiting for a network
     *   app_httpd-- reachable on the fallback AP, so it must not wait for a network either
     *   app_ota  -- registers its hooks with app_httpd, which must exist first
     *   app_mqtt -- last, and optional: it reports what every component above knows, so
     *               it has to come after all of them. Compiled out by default.
     */
    ESP_ERROR_CHECK(nvs_bring_up());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(app_cfg_init());
    ESP_ERROR_CHECK(app_diag_init());
    app_diag_log_banner();

    ESP_ERROR_CHECK(app_led_init());
    app_led_set_status(APP_LED_STATUS_BOOT);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        APP_NET_EVENT, ESP_EVENT_ANY_ID, on_net_event, NULL, NULL));

    ESP_ERROR_CHECK(app_net_init());

#if CONFIG_APP_HTTPD_ENABLE
    /* Started before any network exists on purpose: on the fallback AP it is the only
     * way back in, and a server that waits for a station connection would never come up
     * on a device that cannot associate. */
    ESP_ERROR_CHECK(app_httpd_start());
#else
    ESP_LOGW(TAG, "device web UI compiled out (CONFIG_APP_HTTPD_ENABLE=n)");
#endif

    /* Last, because registering the OTA hooks is what makes app_httpd's update controls
     * live -- and app_httpd has to be up to receive them. Nothing here waits for a
     * network; the OTA task blocks on its own queue until something asks it to check. */
    ESP_ERROR_CHECK(app_ota_init());

    /* Last, because it reports what everything above knows. Not ESP_ERROR_CHECK: an
     * abort here is a boot loop, and a wrong broker URI must cost MQTT, not the device. */
    esp_err_t mqtt_err = app_mqtt_init();
    if (mqtt_err != ESP_OK) {
        ESP_LOGE(TAG, "app_mqtt failed to start (%s) -- carrying on without it",
                 esp_err_to_name(mqtt_err));
    }

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}
