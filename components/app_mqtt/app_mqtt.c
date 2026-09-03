#include "app_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "app_cfg.h"
#include "app_diag.h"
#include "app_display.h"
#include "app_led.h"
#include "app_net.h"
#include "app_ota.h"
#include "app_trust.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "mqtt_cmd.h"
#include "sdkconfig.h"

static const char *TAG = "app_mqtt";

#define TOPIC_MAX  96
#define STATUS_MAX 512

static esp_mqtt_client_handle_t s_client;
static bool tls_in_use;
static esp_timer_handle_t s_status_timer;
static volatile bool s_connected;

static char s_topic_status[TOPIC_MAX];
static char s_topic_online[TOPIC_MAX];
static char s_topic_cmd[TOPIC_MAX];
static char s_topic_all[TOPIC_MAX];

bool app_mqtt_connected(void) { return s_connected; }

/* ------------------------------------------------------------------- publishing */

/* The same facts the status page shows. Deliberately not a second, richer view: two
 * control surfaces that disagree about a device is worse than one that is terse. */
void app_mqtt_publish_status(void)
{
    if (!s_client || !s_connected) {
        return;
    }

    app_net_info_t net;
    app_net_get_info(&net);
    app_ota_info_t ota;
    app_ota_get_info(&ota);
    app_diag_coredump_t cd;
    app_diag_coredump_info(&cd);

    char body[STATUS_MAX];
    int n = snprintf(body, sizeof(body),
        "{\"device\":\"%s\",\"version\":\"%s\",\"partition\":\"%s\","
        "\"uptime_s\":%llu,\"boot_count\":%u,\"reset_reason\":\"%s\","
        "\"heap_free\":%u,\"rssi\":%d,\"ip\":\"%s\","
        "\"ota_state\":\"%s\",\"probation\":%s,\"crash_dump\":%s}",
        app_cfg_device_name(), app_diag_app_version(), app_diag_running_partition(),
        (unsigned long long)app_diag_uptime_s(), (unsigned)app_diag_boot_count(),
        app_diag_reset_reason(), (unsigned)app_diag_free_heap(), net.rssi, net.ip,
        app_ota_state_name(ota.state), ota.probation ? "true" : "false",
        cd.present ? "true" : "false");

    if (n <= 0 || (size_t)n >= sizeof(body)) {
        ESP_LOGW(TAG, "status document did not fit; not publishing a truncated one");
        return;
    }
    /* Retained: a dashboard that connects a minute from now should see the fleet, not an
     * empty topic tree waiting for the next heartbeat. */
    esp_mqtt_client_publish(s_client, s_topic_status, body, n, 0, 1);
}

static void status_timer_fired(void *arg)
{
    (void)arg;
    app_mqtt_publish_status();
}

/* ------------------------------------------------------------------- commands */

static void handle_command(const char *data, int len)
{
    mqtt_cmd_t cmd = mqtt_cmd_parse(data, (size_t)(len > 0 ? len : 0));
    if (cmd == MQTT_CMD_NONE) {
        /* Silently. A broker carries other people's traffic and a device that complains
         * about every message it does not understand is a device that fills a log. */
        ESP_LOGD(TAG, "ignoring unrecognised command (%d bytes)", len);
        return;
    }
    ESP_LOGI(TAG, "command: %s", mqtt_cmd_name(cmd));

    switch (cmd) {
    case MQTT_CMD_CHECK:
        /* forced=false: a fleet-wide push is automation, not a human looking at one
         * device, so it must honour the quarantine on a version that has already failed
         * here. Otherwise one broken release plus one broadcast re-downloads it onto
         * every device that already rejected it twice. */
        if (app_ota_check_now(false) != ESP_OK) {
            ESP_LOGW(TAG, "check refused -- busy, or this image is still on probation");
        }
        break;
    case MQTT_CMD_RESTART:
#if CONFIG_APP_MQTT_ALLOW_RESTART
        if (app_ota_busy()) {
            /* Same refusal the web UI makes: a stray broadcast must not abandon a
             * half-written slot (PLAN §4a). */
            ESP_LOGW(TAG, "restart refused -- an update is being written");
            break;
        }
        ESP_LOGW(TAG, "restarting on request from the broker");
        app_mqtt_publish_status();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
#else
        ESP_LOGW(TAG, "restart refused -- CONFIG_APP_MQTT_ALLOW_RESTART is off");
#endif
        break;
    case MQTT_CMD_LED_ON:   app_led_manual_set(true);  break;
    case MQTT_CMD_LED_OFF:  app_led_manual_set(false); break;
    case MQTT_CMD_LED_AUTO: app_led_manual_release();  break;
    case MQTT_CMD_STATUS:   break;   /* the publish below covers it */
    case MQTT_CMD_REFRESH:
        /* Asks; does not draw. app_display owns the panel, and a refresh takes 26
         * seconds -- doing it on the MQTT task would block the broker link for the whole
         * refresh and drop whatever else arrived meanwhile. It also collapses naturally:
         * a fleet-wide broadcast that arrives twice redraws once, and the frame hash then
         * makes it free if nothing changed. */
        {
            uint32_t wait_s = app_display_request_refresh();
            if (wait_s > 0) {
                ESP_LOGW(TAG, "refresh refused -- %u s until the panel may refresh again",
                         (unsigned)wait_s);
            }
        }
        break;
    case MQTT_CMD_NONE:     break;
    case MQTT_CMD__LAST:    break;
    }

    /* Answer every accepted command with the resulting state, so a fleet-wide push has
     * an observable outcome per device rather than silence. */
    app_mqtt_publish_status();
}

/* ------------------------------------------------------------------- plumbing */

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        /* Naming the trust anchor on a plaintext link would be worse than saying
         * nothing: it reads as reassurance for a connection that has none. */
        ESP_LOGI(TAG, "connected to %s (%s)", CONFIG_APP_MQTT_BROKER_URI,
                 tls_in_use ? app_trust_name() : "UNENCRYPTED -- no server authentication");
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        esp_mqtt_client_subscribe(s_client, s_topic_all, 1);
        /* Retained, to replace the last will the broker published while we were away. */
        esp_mqtt_client_publish(s_client, s_topic_online, "1", 1, 1, 1);
        app_mqtt_publish_status();
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected; the client will keep retrying");
        break;

    case MQTT_EVENT_DATA:
        handle_command(e->data, e->data_len);
        break;

    case MQTT_EVENT_ERROR:
        /* Worth a line rather than silence: a certificate or credential problem here
         * looks identical to "the broker is down" from the outside. */
        if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "transport error: esp-tls 0x%x, sock errno %d",
                     e->error_handle->esp_tls_last_esp_err, e->error_handle->esp_transport_sock_errno);
        } else if (e->error_handle) {
            ESP_LOGW(TAG, "connect refused, code %d", e->error_handle->connect_return_code);
        }
        break;

    default:
        break;
    }
}

esp_err_t app_mqtt_init(void)
{
    const char *prefix = CONFIG_APP_MQTT_TOPIC_PREFIX;
    const char *id = app_cfg_device_id();

    if (!mqtt_topic(s_topic_status, sizeof(s_topic_status), prefix, id, "status") ||
        !mqtt_topic(s_topic_online, sizeof(s_topic_online), prefix, id, "online") ||
        !mqtt_topic(s_topic_cmd,    sizeof(s_topic_cmd),    prefix, id, "cmd") ||
        !mqtt_topic(s_topic_all,    sizeof(s_topic_all),    prefix, "all", "cmd")) {
        /* A truncated topic is a valid topic nobody is listening to, which presents as a
         * device that never responds. Refuse to start instead. */
        ESP_LOGE(TAG, "topic prefix \"%s\" is too long", prefix);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Said out loud, once, at boot. A device quietly shipping its credentials in the
     * clear is exactly the kind of thing that is obvious in the config and invisible
     * afterwards. */
    tls_in_use = (strncmp(CONFIG_APP_MQTT_BROKER_URI, "mqtts://", 8) == 0);
    if (!tls_in_use) {
        ESP_LOGW(TAG, "broker URI is not mqtts:// -- credentials and commands will cross "
                      "the network unencrypted");
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_APP_MQTT_BROKER_URI,
        .credentials.username = CONFIG_APP_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_APP_MQTT_PASSWORD,
        .credentials.client_id = app_cfg_device_name(),
        .session.keepalive = CONFIG_APP_MQTT_KEEPALIVE_S,
        /* The last will is what makes a dashboard honest: without it a device that loses
         * power stays "online" forever, because the last thing it ever said was that it
         * was fine. */
        .session.last_will = {
            .topic = s_topic_online,
            .msg = "0",
            .msg_len = 1,
            .qos = 1,
            .retain = 1,
        },
    };
    /* TLS only: esp-mqtt rejects verification settings on a plaintext transport and
     * esp_mqtt_client_init() then returns NULL, which looks like nothing at all. */
    if (tls_in_use) {
        cfg.broker.verification.certificate = app_trust_ca_pem();
        cfg.broker.verification.crt_bundle_attach = app_trust_bundle_attach();
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        /* Almost always the configuration rather than the broker -- the client has not
         * tried to connect yet. esp-mqtt logs the specific complaint just above this. */
        ESP_LOGE(TAG, "could not create the MQTT client; check CONFIG_APP_MQTT_BROKER_URI");
        return ESP_FAIL;
    }
    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                  on_mqtt_event, NULL);
    if (err != ESP_OK) {
        return err;
    }

    const esp_timer_create_args_t args = {
        .callback = status_timer_fired, .name = "mqtt_status",
    };
    if ((err = esp_timer_create(&args, &s_status_timer)) != ESP_OK) {
        return err;
    }
    if ((err = esp_timer_start_periodic(
            s_status_timer,
            (uint64_t)CONFIG_APP_MQTT_STATUS_INTERVAL_S * 1000000ULL)) != ESP_OK) {
        return err;
    }

    /* Started immediately, with no network: esp-mqtt retries on its own, and waiting for
     * a network here would mean a second place that has to know about connectivity. */
    err = esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "%s; topics %s/{status,online,cmd} and %s",
             err == ESP_OK ? "started" : "failed to start", prefix, s_topic_all);
    return err;
}
