#include "app_net.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_cfg.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "sdkconfig.h"

static const char *TAG = "app_net";

ESP_EVENT_DEFINE_BASE(APP_NET_EVENT);

/* ---------------------------------------------------------------- internal types */

typedef enum {
    CMD_STA_DISCONNECTED,
    CMD_GOT_IP,
    CMD_RETRY,
    CMD_FALLBACK_DUE,
    CMD_AP_LINGER_DUE,
    CMD_PROVISION,
    CMD_FORGET,
} cmd_id_t;

typedef struct {
    cmd_id_t id;
    int      reason;             /* CMD_STA_DISCONNECTED */
    char     ssid[33];           /* CMD_PROVISION */
    char     pass[65];
} cmd_t;

/* --------------------------------------------------------------- module state */

static QueueHandle_t     s_cmds;
static SemaphoreHandle_t s_lock;          /* guards s_info */
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_fallback_timer;
static esp_timer_handle_t s_linger_timer;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static app_net_info_t s_info;
static uint32_t s_backoff_s = 1;
static bool s_provisioned;
static bool s_sntp_started;
static bool s_mdns_started;
static bool s_ready;

/* --------------------------------------------------------------- small helpers */

const char *app_net_state_name(app_net_state_t state)
{
    switch (state) {
    case APP_NET_STATE_UNPROVISIONED: return "unprovisioned";
    case APP_NET_STATE_CONNECTING:    return "connecting";
    case APP_NET_STATE_FALLBACK:      return "fallback";
    case APP_NET_STATE_CONNECTED:     return "connected";
    }
    return "unknown";
}

void app_net_get_info(app_net_info_t *out)
{
    if (!out) {
        return;
    }
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_lock);

    /* RSSI is cheap to read and stale the moment it is cached, so read it live rather
     * than keeping a copy that lies. */
    if (out->sta_connected) {
        wifi_ap_record_t rec;
        if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
            out->rssi = rec.rssi;
        }
    }
}

app_net_state_t app_net_state(void)
{
    app_net_state_t s;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s = s_info.state;
    xSemaphoreGive(s_lock);
    return s;
}

bool app_net_time_synced(void)
{
    bool t;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    t = s_info.time_synced;
    xSemaphoreGive(s_lock);
    return t;
}

bool app_net_wait_time_sync(uint32_t timeout_ms)
{
    const uint32_t step_ms = 500;
    uint32_t waited = 0;

    while (!app_net_time_synced()) {
        if (waited >= timeout_ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
    }
    return true;
}

const char *app_net_ap_passphrase(void) { return app_cfg_ap_passphrase(); }

static void set_state(app_net_state_t state)
{
    bool changed;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    changed = (s_info.state != state);
    s_info.state = state;
    xSemaphoreGive(s_lock);

    if (changed) {
        ESP_LOGI(TAG, "state -> %s", app_net_state_name(state));
        esp_event_post(APP_NET_EVENT, APP_NET_EVENT_STATE_CHANGED,
                       &state, sizeof(state), 0);
    }
}

static void post(cmd_id_t id)
{
    cmd_t c = { .id = id };
    if (s_cmds) {
        xQueueSend(s_cmds, &c, 0);
    }
}

static bool have_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (app_cfg_get_str(APP_CFG_KEY_WIFI_SSID, ssid, ssid_len) != ESP_OK || ssid[0] == '\0') {
        return false;
    }
    /* A stored open network has an SSID and no passphrase, which is legitimate. */
    if (app_cfg_get_str(APP_CFG_KEY_WIFI_PASS, pass, pass_len) != ESP_OK) {
        pass[0] = '\0';
    }
    return true;
}

/* ------------------------------------------------------------------ radio control */

static void schedule_retry(void);

static esp_err_t apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    /* Accept anything from open upwards: refusing to join a WEP or WPA-only network
     * turns a legacy access point into an unexplained failure to associate. */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_info.ssid, ssid, sizeof(s_info.ssid));
    xSemaphoreGive(s_lock);

    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static esp_err_t ap_set_active(bool up)
{
    bool already;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    already = s_info.ap_active;
    xSemaphoreGive(s_lock);
    if (already == up) {
        return ESP_OK;
    }

    if (!up) {
        ESP_LOGI(TAG, "provisioning AP down");
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.ap_active = false;
        s_info.ap_ssid[0] = '\0';
        s_info.ap_ip[0] = '\0';
        xSemaphoreGive(s_lock);
        return err;
    }

    const char *ssid = app_cfg_device_name();
    const char *pass = app_net_ap_passphrase();

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(ssid);
    strlcpy((char *)cfg.ap.password, pass, sizeof(cfg.ap.password));
    cfg.ap.channel = CONFIG_APP_NET_AP_CHANNEL;
    cfg.ap.max_connection = CONFIG_APP_NET_AP_MAX_STA;
    /* WPA2, never open. An open AP lets anyone in range hand the device credentials or
     * point it at their own update server (PLAN §4b). */
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raising AP: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(s_ap_netif, &ip);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.ap_active = true;
    strlcpy(s_info.ap_ssid, ssid, sizeof(s_info.ap_ssid));
    esp_ip4addr_ntoa(&ip.ip, s_info.ap_ip, sizeof(s_info.ap_ip));
    xSemaphoreGive(s_lock);

    ESP_LOGW(TAG, "provisioning AP up: SSID \"%s\"  pass \"%s\"  http://%s/",
             ssid, pass, s_info.ap_ip);
    return ESP_OK;
}

/* Every connect attempt goes through here.
 *
 * esp_wifi_connect() failing used to be swallowed, and the result was the worst failure
 * mode this component can have: a device that reports "connecting", never associates,
 * never retries, and says nothing at all. A connect that cannot even be started is still
 * a failed attempt, so it gets logged and re-queued like any other. */
static void try_connect(void)
{
    if (!s_provisioned) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        schedule_retry();
    }
}

static void schedule_retry(void)
{
    /* Exponential backoff with jitter, so a router coming back after a power cut is not
     * met by every board on the bench associating in the same millisecond. */
    uint32_t base_ms = s_backoff_s * 1000;
    uint32_t jitter = esp_random() % (base_ms / 2 + 1);
    uint64_t delay_us = (uint64_t)(base_ms - base_ms / 4 + jitter) * 1000ULL;

    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, delay_us);
    ESP_LOGI(TAG, "retry in %ums (backoff %us)", (unsigned)(delay_us / 1000), (unsigned)s_backoff_s);

    s_backoff_s *= 2;
    if (s_backoff_s > CONFIG_APP_NET_BACKOFF_MAX_S) {
        s_backoff_s = CONFIG_APP_NET_BACKOFF_MAX_S;
    }
}

/* --------------------------------------------------------------- SNTP and mDNS */

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool first = !s_info.time_synced;
    s_info.time_synced = true;
    xSemaphoreGive(s_lock);

    if (first) {
        time_t now = time(NULL);
        struct tm tm_utc;
        char buf[32];
        gmtime_r(&now, &tm_utc);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_utc);
        ESP_LOGI(TAG, "time synced: %s UTC", buf);
        esp_event_post(APP_NET_EVENT, APP_NET_EVENT_TIME_SYNCED, NULL, 0, 0);
    }
}

static void sntp_start_once(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_APP_NET_SNTP_SERVER);
    cfg.sync_cb = on_time_sync;
    cfg.start = false;          /* started explicitly below, once there is an IP */
    cfg.wait_for_sync = false;  /* nothing here blocks on the clock; the callback reports it */
#if CONFIG_APP_NET_SNTP_FROM_DHCP
    /* A LAN with no route to the internet still usually has a router willing to answer
     * NTP, and the DHCP-supplied server is how we find it.
     *
     * num_of_servers counts the *preconfigured* servers -- still one. index_of_first_server
     * moves it to slot 1 so DHCP can fill slot 0, which is why sdkconfig.defaults raises
     * CONFIG_LWIP_SNTP_MAX_SERVERS to 2: the array is exactly that size, and leaving it at
     * the IDF default of 1 puts the static server one past the end.
     */
    cfg.server_from_dhcp = true;
    cfg.index_of_first_server = 1;
    cfg.renew_servers_after_new_IP = true;
    cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
#endif
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        /* Best effort by design: a device on an isolated LAN may simply never get the
         * time. Say so, because the consequence lands later as a TLS handshake failure
         * that looks like a network fault (PLAN §7). */
        ESP_LOGW(TAG, "sntp init: %s -- TLS will fail until the clock is set",
                 esp_err_to_name(err));
        return;
    }
    esp_netif_sntp_start();
    s_sntp_started = true;
}

static void mdns_start_once(void)
{
#if CONFIG_APP_NET_MDNS_ENABLE
    if (s_mdns_started) {
        return;
    }
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns init: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(app_cfg_device_name());
    mdns_instance_name_set(app_cfg_device_name());
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_mdns_started = true;
    ESP_LOGI(TAG, "mdns: http://%s.local/", app_cfg_device_name());
#endif
}

/* ------------------------------------------------------------------ IDF events */

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        /* Only chase a network we were actually told about. Connecting unconditionally
         * here is how an "unprovisioned" device silently joins whatever the Wi-Fi driver
         * still had in NVS from previous firmware -- observed on the bench, and a nasty
         * one: the device reports Unprovisioned while sitting on someone's network. */
        try_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = data;
        cmd_t c = { .id = CMD_STA_DISCONNECTED, .reason = e ? e->reason : 0 };
        xQueueSend(s_cmds, &c, 0);
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "portal client " MACSTR " joined", MAC2STR(e->mac));
        break;
    }
    default:
        break;
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const ip_event_got_ip_t *e = data;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_ip4addr_ntoa(&e->ip_info.ip, s_info.ip, sizeof(s_info.ip));
    esp_ip4addr_ntoa(&e->ip_info.gw, s_info.gateway, sizeof(s_info.gateway));
    xSemaphoreGive(s_lock);

    post(CMD_GOT_IP);
}

static void timer_retry(void *arg)    { (void)arg; post(CMD_RETRY); }
static void timer_fallback(void *arg) { (void)arg; post(CMD_FALLBACK_DUE); }
static void timer_linger(void *arg)   { (void)arg; post(CMD_AP_LINGER_DUE); }

/* ------------------------------------------------------------------ state machine */

static app_net_prov_result_t reason_to_result(int reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return APP_NET_PROV_NOT_FOUND;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return APP_NET_PROV_BAD_AUTH;
    default:
        return APP_NET_PROV_FAILED;
    }
}

static void arm_fallback_timer(void)
{
    esp_timer_stop(s_fallback_timer);
    esp_timer_start_once(s_fallback_timer,
                         (uint64_t)CONFIG_APP_NET_FALLBACK_DELAY_S * 1000000ULL);
}

static void handle_disconnected(int reason)
{
    bool was_connected;
    bool prov_pending;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    was_connected = s_info.sta_connected;
    prov_pending = (s_info.prov_result == APP_NET_PROV_PENDING);
    s_info.sta_connected = false;
    s_info.ip[0] = '\0';
    s_info.gateway[0] = '\0';
    s_info.retries++;
    if (prov_pending) {
        s_info.prov_result = reason_to_result(reason);
    }
    uint32_t retries = s_info.retries;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "disconnected (reason %d), attempt %u", reason, (unsigned)retries);

    if (was_connected) {
        esp_event_post(APP_NET_EVENT, APP_NET_EVENT_LOST_IP, NULL, 0, 0);
    }

    /* Only Connected and Connecting demote; Unprovisioned and Fallback already have the
     * AP up and keep retrying underneath it. */
    if (app_net_state() == APP_NET_STATE_CONNECTED) {
        set_state(APP_NET_STATE_CONNECTING);
        arm_fallback_timer();
    }

    schedule_retry();
}

static void handle_got_ip(void)
{
    bool ap_up;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.sta_connected = true;
    s_info.retries = 0;
    if (s_info.prov_result == APP_NET_PROV_PENDING) {
        s_info.prov_result = APP_NET_PROV_OK;
    }
    ap_up = s_info.ap_active;
    char ip[16];
    strlcpy(ip, s_info.ip, sizeof(ip));
    xSemaphoreGive(s_lock);

    s_backoff_s = 1;
    esp_timer_stop(s_retry_timer);
    esp_timer_stop(s_fallback_timer);

    /* Take the SSID from the association rather than from what we asked for: they can
     * differ, and the status page should show where the device actually is. */
    wifi_ap_record_t rec;
    if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_info.ssid, (const char *)rec.ssid, sizeof(s_info.ssid));
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG, "connected, ip %s", ip);
    set_state(APP_NET_STATE_CONNECTED);

    sntp_start_once();
    mdns_start_once();

    app_net_info_t snapshot;
    app_net_get_info(&snapshot);
    esp_event_post(APP_NET_EVENT, APP_NET_EVENT_GOT_IP, &snapshot, sizeof(snapshot), 0);

    if (ap_up) {
        /* Linger, so whoever just typed a passphrase into the portal sees that it
         * worked before their phone loses the AP. */
        esp_timer_stop(s_linger_timer);
        if (CONFIG_APP_NET_AP_LINGER_S > 0) {
            ESP_LOGI(TAG, "AP stays up %ds so the portal can report success",
                     CONFIG_APP_NET_AP_LINGER_S);
            esp_timer_start_once(s_linger_timer,
                                 (uint64_t)CONFIG_APP_NET_AP_LINGER_S * 1000000ULL);
        } else {
            post(CMD_AP_LINGER_DUE);
        }
    }
}

static void handle_provision(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "provisioning: joining \"%s\"", ssid);

    app_cfg_set_str(APP_CFG_KEY_WIFI_SSID, ssid);
    app_cfg_set_str(APP_CFG_KEY_WIFI_PASS, pass);
    s_provisioned = true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.prov_result = APP_NET_PROV_PENDING;
    s_info.retries = 0;
    xSemaphoreGive(s_lock);

    s_backoff_s = 1;
    esp_timer_stop(s_retry_timer);

    esp_err_t err = apply_sta_config(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "applying credentials: %s", esp_err_to_name(err));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.prov_result = APP_NET_PROV_FAILED;
        xSemaphoreGive(s_lock);
        return;
    }
    esp_wifi_disconnect();
    try_connect();

    /* Stay in whatever state we are in -- the AP must remain up until the result is
     * known, and that is exactly what Fallback and Unprovisioned already mean. */
    if (app_net_state() == APP_NET_STATE_CONNECTED) {
        set_state(APP_NET_STATE_CONNECTING);
    }
}

static void handle_forget(void)
{
    ESP_LOGW(TAG, "credentials cleared");
    app_cfg_erase(APP_CFG_KEY_WIFI_SSID);
    app_cfg_erase(APP_CFG_KEY_WIFI_PASS);
    s_provisioned = false;

    /* Clear the driver's copy too, or a later esp_wifi_connect() would still have
     * somewhere to go. */
    wifi_config_t blank = { 0 };
    esp_wifi_set_config(WIFI_IF_STA, &blank);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.ssid[0] = '\0';
    s_info.prov_result = APP_NET_PROV_IDLE;
    s_info.sta_connected = false;
    xSemaphoreGive(s_lock);

    esp_timer_stop(s_retry_timer);
    esp_timer_stop(s_fallback_timer);
    esp_timer_stop(s_linger_timer);
    esp_wifi_disconnect();

    set_state(APP_NET_STATE_UNPROVISIONED);
    ap_set_active(true);
}

static void net_task(void *arg)
{
    (void)arg;
    cmd_t c;

    for (;;) {
        if (xQueueReceive(s_cmds, &c, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (c.id) {
        case CMD_STA_DISCONNECTED:
            handle_disconnected(c.reason);
            break;
        case CMD_GOT_IP:
            handle_got_ip();
            break;
        case CMD_RETRY:
            try_connect();
            break;
        case CMD_FALLBACK_DUE:
            if (app_net_state() == APP_NET_STATE_CONNECTING) {
                ESP_LOGW(TAG, "no association after %ds -- raising the provisioning AP "
                              "(station retries continue underneath it)",
                         CONFIG_APP_NET_FALLBACK_DELAY_S);
                set_state(APP_NET_STATE_FALLBACK);
                ap_set_active(true);
            }
            break;
        case CMD_AP_LINGER_DUE:
            if (app_net_state() == APP_NET_STATE_CONNECTED) {
                ap_set_active(false);
            }
            break;
        case CMD_PROVISION:
            handle_provision(c.ssid, c.pass);
            break;
        case CMD_FORGET:
            handle_forget();
            break;
        }
    }
}

/* ------------------------------------------------------------------- public API */

esp_err_t app_net_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    s_cmds = xQueueCreate(8, sizeof(cmd_t));
    if (!s_lock || !s_cmds) {
        return ESP_ERR_NO_MEM;
    }

    /* Both interfaces exist from the start. Creating the AP netif lazily would mean
     * doing it from the fallback path, which is the one path that must not fail. */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) {
        return ESP_FAIL;
    }
    esp_netif_set_hostname(s_sta_netif, app_cfg_device_name());

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    /* app_cfg owns credentials, so the Wi-Fi driver must not keep its own copy in NVS.
     * Left enabled, the driver loads whatever a previous firmware stored and the device
     * joins a network nobody in this build ever configured. */
    wcfg.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event, NULL, NULL));

    const esp_timer_create_args_t retry_args =
        { .callback = timer_retry, .name = "net_retry" };
    const esp_timer_create_args_t fallback_args =
        { .callback = timer_fallback, .name = "net_fallback" };
    const esp_timer_create_args_t linger_args =
        { .callback = timer_linger, .name = "net_linger" };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));
    ESP_ERROR_CHECK(esp_timer_create(&fallback_args, &s_fallback_timer));
    ESP_ERROR_CHECK(esp_timer_create(&linger_args, &s_linger_timer));

    if (xTaskCreate(net_task, "app_net", CONFIG_APP_NET_TASK_STACK, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    char ssid[33], pass[65];
    bool provisioned = have_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    s_provisioned = provisioned;

    /* Mode first, then config. esp_wifi_set_config() rejects an interface that is not
     * enabled in the current mode, and the mode after esp_wifi_init() is NULL -- so
     * configuring the station before setting the mode fails, and the device then sits in
     * "connecting" forever with no SSID to connect to. */
    if (provisioned) {
        ESP_LOGI(TAG, "stored credentials for \"%s\"", ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(apply_sta_config(ssid, pass));
        set_state(APP_NET_STATE_CONNECTING);
    } else {
        ESP_LOGW(TAG, "no stored credentials -- starting in provisioning mode");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        set_state(APP_NET_STATE_UNPROVISIONED);
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    if (provisioned) {
        /* Only a provisioned device waits: an unprovisioned one has nothing to wait for. */
        arm_fallback_timer();
    } else {
        ap_set_active(true);
    }

    s_ready = true;
    return ESP_OK;
}

esp_err_t app_net_provision(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password && strlen(password) > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    cmd_t c = { .id = CMD_PROVISION };
    strlcpy(c.ssid, ssid, sizeof(c.ssid));
    strlcpy(c.pass, password ? password : "", sizeof(c.pass));
    return xQueueSend(s_cmds, &c, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_net_forget(void)
{
    cmd_t c = { .id = CMD_FORGET };
    return xQueueSend(s_cmds, &c, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_net_scan(app_net_ap_t *out, size_t max, size_t *count)
{
    if (!out || !count || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    wifi_scan_config_t scan = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return ESP_OK;
    }

    wifi_ap_record_t *recs = calloc(found, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&found, recs);
    if (err != ESP_OK) {
        free(recs);
        return err;
    }

    /* IDF returns records strongest-first. Collapse duplicate SSIDs -- a mesh or a
     * dual-band router otherwise fills the whole list with one network, and the first
     * copy seen is the strongest one. */
    for (uint16_t i = 0; i < found && *count < max; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < *count; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        strlcpy(out[*count].ssid, ssid, sizeof(out[*count].ssid));
        out[*count].rssi = recs[i].rssi;
        out[*count].channel = recs[i].primary;
        out[*count].open = (recs[i].authmode == WIFI_AUTH_OPEN);
        (*count)++;
    }

    free(recs);
    return ESP_OK;
}
