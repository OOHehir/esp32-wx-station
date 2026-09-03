#include "app_httpd.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "httpd_internal.h"
#include "sdkconfig.h"

static const char *TAG = "app_httpd";

static httpd_handle_t s_server;
static const app_httpd_ota_hooks_t *s_ota_hooks;
static const app_httpd_panel_hooks_t *s_panel_hooks;
static const app_httpd_button_hooks_t *s_button_hooks;

const app_httpd_ota_hooks_t *app_httpd_ota(void) { return s_ota_hooks; }
const app_httpd_panel_hooks_t *app_httpd_panel(void) { return s_panel_hooks; }
const app_httpd_button_hooks_t *app_httpd_buttons(void) { return s_button_hooks; }

void app_httpd_set_ota_hooks(const app_httpd_ota_hooks_t *hooks)
{
    s_ota_hooks = hooks;
    ESP_LOGI(TAG, "OTA hooks %s", hooks ? "registered" : "cleared");
}

void app_httpd_set_panel_hooks(const app_httpd_panel_hooks_t *hooks)
{
    s_panel_hooks = hooks;
    ESP_LOGI(TAG, "panel hooks %s", hooks ? "registered" : "cleared");
}

void app_httpd_set_button_hooks(const app_httpd_button_hooks_t *hooks)
{
    s_button_hooks = hooks;
    ESP_LOGI(TAG, "button hooks %s", hooks ? "registered" : "cleared");
}

/* The page itself: one embedded file, no CDN, no build step. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* No caching: the page is tiny and a stale copy after an OTA is confusing in
     * exactly the moment you most want to trust what you are reading. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

esp_err_t app_httpd_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_APP_HTTPD_PORT;
    cfg.max_open_sockets = CONFIG_APP_HTTPD_MAX_SOCKETS;
    cfg.stack_size = CONFIG_APP_HTTPD_STACK;
    cfg.lru_purge_enable = true;      /* a phone that wandered off must not hold a slot */
    /* One for "/", thirteen in api.c, ten captive-portal probes: 24, and the count is
     * checked below rather than kept in somebody's head.
     *
     * At 20 this silently lost the last handlers registered, which were the portal's --
     * including the "slash-star" catch-all that answers a phone probing for a portal by
     * any URL nobody anticipated. The only symptom was one warning line at boot
     * ("register probe ... HANDLERS_FULL") and a setup flow that worked on
     * some phones and not others. */
    cfg.max_uri_handlers = 28;
    /* Registered handlers are matched first; the wildcard below only sees what is left,
     * which is what makes the captive-portal redirect possible without shadowing /api. */
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = index_get,
    };
    httpd_register_uri_handler(s_server, &index_uri);

    app_httpd_register_api(s_server);

#if CONFIG_APP_HTTPD_CAPTIVE_PORTAL
    portal_register_probes(s_server);
    portal_dns_start();
#endif

    ESP_LOGI(TAG, "listening on port %d", CONFIG_APP_HTTPD_PORT);
    return ESP_OK;
}

esp_err_t app_httpd_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }
#if CONFIG_APP_HTTPD_CAPTIVE_PORTAL
    portal_dns_stop();
#endif
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}

bool app_httpd_running(void) { return s_server != NULL; }
