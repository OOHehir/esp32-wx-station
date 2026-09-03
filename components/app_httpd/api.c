/* The JSON control surface behind the status page.
 *
 * Everything the page can do, curl can do identically -- which is what makes this
 * usable on the bench and in CI, not just from a phone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_cfg.h"
#include "app_diag.h"
#include "app_led.h"
#include "app_net.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "httpd_internal.h"
#include "json_util.h"
#include "sdkconfig.h"

static const char *TAG = "app_httpd";

#define BODY_MAX   512
#define STATUS_MAX 1792

/* ------------------------------------------------------------------- utilities */

esp_err_t httpd_send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

esp_err_t httpd_send_error(httpd_req_t *req, const char *status, const char *message)
{
    char esc[160], body[224];
    json_escape(message, esc, sizeof(esc));
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", esc);
    httpd_resp_set_status(req, status);
    return httpd_send_json(req, body);
}

esp_err_t httpd_read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len >= buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += n;
    }
    buf[got] = '\0';
    return ESP_OK;
}

/* Restart from a timer rather than from the handler, so the response actually reaches
 * the browser before the socket dies with the device. */
static void restart_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "restarting on request from the web UI");
    esp_restart();
}

static void restart_after(uint32_t ms)
{
    static esp_timer_handle_t timer;
    if (!timer) {
        const esp_timer_create_args_t args = { .callback = restart_cb, .name = "httpd_restart" };
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_start_once(timer, (uint64_t)ms * 1000ULL);
}

static const char *prov_result_name(app_net_prov_result_t r)
{
    switch (r) {
    case APP_NET_PROV_IDLE:      return "idle";
    case APP_NET_PROV_PENDING:   return "pending";
    case APP_NET_PROV_OK:        return "ok";
    case APP_NET_PROV_BAD_AUTH:  return "bad_password";
    case APP_NET_PROV_NOT_FOUND: return "not_found";
    case APP_NET_PROV_FAILED:    return "failed";
    }
    return "unknown";
}

/* ---------------------------------------------------------------- GET /api/status */

static esp_err_t status_get(httpd_req_t *req)
{
    app_net_info_t net;
    app_net_get_info(&net);

    char ssid[80], ap_ssid[80];
    json_escape(net.ssid, ssid, sizeof(ssid));
    json_escape(net.ap_ssid, ap_ssid, sizeof(ap_ssid));

    char *buf = malloc(STATUS_MAX);
    if (!buf) {
        return httpd_send_error(req, "503 Service Unavailable", "out of memory");
    }

    int n = snprintf(buf, STATUS_MAX,
        "{"
          "\"device\":{\"name\":\"%s\",\"id\":\"%s\",\"chip\":\"%s\"},"
          "\"firmware\":{\"version\":\"%s\",\"built\":\"%s\",\"idf\":\"%s\","
                        "\"partition\":\"%s\",\"pending_verify\":%s},"
          "\"runtime\":{\"uptime_s\":%llu,\"boot_count\":%u,\"reset_reason\":\"%s\","
                       "\"heap_free\":%u,\"heap_min\":%u},"
          "\"net\":{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"gateway\":\"%s\","
                   "\"rssi\":%d,\"retries\":%u,\"time_synced\":%s,"
                   "\"ap_active\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\","
                   "\"prov_result\":\"%s\"},"
          "\"led\":{\"present\":%s,\"on\":%s,\"status\":\"%s\",\"manual\":%s}",
        app_cfg_device_name(), app_cfg_device_id(), app_diag_chip_model(),
        app_diag_app_version(), app_diag_build_timestamp(), app_diag_idf_version(),
        app_diag_running_partition(), app_diag_pending_verify() ? "true" : "false",
        (unsigned long long)app_diag_uptime_s(), (unsigned)app_diag_boot_count(),
        app_diag_reset_reason(), (unsigned)app_diag_free_heap(),
        (unsigned)app_diag_min_free_heap(),
        app_net_state_name(net.state), ssid, net.ip, net.gateway,
        net.rssi, (unsigned)net.retries, net.time_synced ? "true" : "false",
        net.ap_active ? "true" : "false", ap_ssid, net.ap_ip,
        prov_result_name(net.prov_result),
        app_led_present() ? "true" : "false", app_led_is_on() ? "true" : "false",
        app_led_status_name(app_led_get_status()),
        app_led_manual_active() ? "true" : "false");

    /* app_ota fills the rest of the "ota" object in -- state, progress, probation and
     * the quarantine record. With no hooks registered it carries only availability, and
     * the page hides the update controls accordingly. */
    /* Whole and self-contained, so it can go before "ota" -- which is deliberately left
     * open by the format above for app_ota to finish, and must therefore stay last.
     * Appending anything between the two nests it inside "ota" instead, which is exactly
     * what the first version of this did. */
    const app_httpd_panel_hooks_t *panel = app_httpd_panel();
    if (panel && panel->status_json && n > 0 && n < STATUS_MAX) {
        n += snprintf(buf + n, STATUS_MAX - n, ",\"panel\":{");
        if (n > 0 && n < STATUS_MAX) {
            n += panel->status_json(buf + n, STATUS_MAX - n);
        }
        if (n > 0 && n < STATUS_MAX) {
            n += snprintf(buf + n, STATUS_MAX - n, "}");
        }
    }

    const app_httpd_button_hooks_t *btn = app_httpd_buttons();
    if (btn && btn->status_json && n > 0 && n < STATUS_MAX) {
        n += snprintf(buf + n, STATUS_MAX - n, ",\"buttons\":{");
        if (n > 0 && n < STATUS_MAX) {
            n += btn->status_json(buf + n, STATUS_MAX - n);
        }
        if (n > 0 && n < STATUS_MAX) {
            n += snprintf(buf + n, STATUS_MAX - n, "}");
        }
    }

    if (n > 0 && n < STATUS_MAX) {
        n += snprintf(buf + n, STATUS_MAX - n, ",\"ota\":{\"available\":%s",
                      app_httpd_ota() ? "true" : "false");
    }

    const app_httpd_ota_hooks_t *ota = app_httpd_ota();
    if (ota && ota->status_json && n > 0 && n < STATUS_MAX) {
        n += ota->status_json(buf + n, STATUS_MAX - n);
    }
    if (n > 0 && n < STATUS_MAX - 3) {
        snprintf(buf + n, STATUS_MAX - n, "}}");
    } else {
        free(buf);
        return httpd_send_error(req, "500 Internal Server Error", "status buffer overflow");
    }

    esp_err_t err = httpd_send_json(req, buf);
    free(buf);
    return err;
}

/* -------------------------------------------------------------- POST /api/restart */

static esp_err_t restart_post(httpd_req_t *req)
{
    const app_httpd_ota_hooks_t *ota = app_httpd_ota();
    if (ota && ota->busy && ota->busy()) {
        /* Refusing here is the point: a stray click must not abandon a half-written
         * slot (PLAN §4a). */
        return httpd_send_error(req, "409 Conflict",
                                "an update is being written -- restart refused");
    }
    restart_after(500);
    return httpd_send_json(req, "{\"ok\":true,\"restarting_in_ms\":500}");
}

/* ------------------------------------------------------------------ POST /api/led */

static esp_err_t led_post(httpd_req_t *req)
{
    if (!app_led_present()) {
        return httpd_send_error(req, "501 Not Implemented",
                                "this build has no status LED configured");
    }

    char body[BODY_MAX] = "";
    if (req->content_len > 0 && httpd_read_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_send_error(req, "400 Bad Request", "unreadable body");
    }

    bool want_auto = false;
    bool have_explicit = false;
    bool want_on = false;

    if (body[0]) {
        cJSON *root = cJSON_Parse(body);
        if (!root) {
            return httpd_send_error(req, "400 Bad Request", "body is not JSON");
        }
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        if (cJSON_IsString(mode) && strcmp(mode->valuestring, "auto") == 0) {
            want_auto = true;
        }
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(root, "on");
        if (cJSON_IsBool(on)) {
            have_explicit = true;
            want_on = cJSON_IsTrue(on);
        }
        cJSON_Delete(root);
    }

    if (want_auto) {
        app_led_manual_release();
    } else if (have_explicit) {
        app_led_manual_set(want_on);
    } else {
        /* No body, or no usable field: treat it as the toggle the button wants. */
        app_led_manual_toggle();
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"on\":%s,\"manual\":%s,\"status\":\"%s\"}",
             app_led_is_on() ? "true" : "false",
             app_led_manual_active() ? "true" : "false",
             app_led_status_name(app_led_get_status()));
    return httpd_send_json(req, resp);
}

/* ------------------------------------------------------------------ GET /api/scan */

static esp_err_t scan_get(httpd_req_t *req)
{
    const size_t max = CONFIG_APP_HTTPD_SCAN_MAX;
    app_net_ap_t *aps = calloc(max, sizeof(*aps));
    if (!aps) {
        return httpd_send_error(req, "503 Service Unavailable", "out of memory");
    }

    size_t count = 0;
    esp_err_t err = app_net_scan(aps, max, &count);
    if (err != ESP_OK) {
        free(aps);
        return httpd_send_error(req, "503 Service Unavailable", esp_err_to_name(err));
    }

    /* One entry is 44 bytes of literal, an SSID that escapes to at most 79, an rssi of
      * up to 4 and a channel of up to 3: 130, not 128. A 32-character SSID of control
      * bytes is what reaches the top of that range, and a neighbour chooses its own
      * SSID. 160 leaves the arithmetic room to be wrong again. */
    size_t cap = 32 + count * 160;
    char *buf = malloc(cap);
    if (!buf) {
        free(aps);
        return httpd_send_error(req, "503 Service Unavailable", "out of memory");
    }

    /* Guarded the way status_get is: snprintf returns what it *would* have written, so
      * an unchecked `n` walks past the buffer and turns `cap - n` into a huge size_t. */
    int n = snprintf(buf, cap, "{\"networks\":[");
    for (size_t i = 0; i < count && n > 0 && (size_t)n < cap; i++) {
        char esc[80];
        json_escape(aps[i].ssid, esc, sizeof(esc));
        n += snprintf(buf + n, cap - n,
                      "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"open\":%s}",
                      i ? "," : "", esc, aps[i].rssi, aps[i].channel,
                      aps[i].open ? "true" : "false");
    }
    if (n <= 0 || (size_t)n >= cap - 3) {
        /* Nothing partial: a truncated document is invalid JSON, and the page would
         * report a parse error rather than an empty scan. */
        n = snprintf(buf, cap, "{\"networks\":[");
    }
    snprintf(buf + n, cap - n, "]}");

    free(aps);
    esp_err_t send = httpd_send_json(req, buf);
    free(buf);
    return send;
}

/* ------------------------------------------------------------- POST /api/provision */

static esp_err_t provision_post(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (httpd_read_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_send_error(req, "400 Bad Request", "unreadable or oversized body");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return httpd_send_error(req, "400 Bad Request", "body is not JSON");
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return httpd_send_error(req, "400 Bad Request", "ssid is required");
    }

    esp_err_t err = app_net_provision(ssid->valuestring,
                                      cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return httpd_send_error(req, "400 Bad Request", esp_err_to_name(err));
    }
    /* Deliberately does not wait for the join. The page polls /api/status and reports
     * the outcome while the AP is still up. */
    return httpd_send_json(req, "{\"ok\":true,\"result\":\"pending\"}");
}

/* ---------------------------------------------------------------- POST /api/forget */

static esp_err_t forget_post(httpd_req_t *req)
{
    esp_err_t err = app_net_forget();
    if (err != ESP_OK) {
        return httpd_send_error(req, "503 Service Unavailable", esp_err_to_name(err));
    }
    return httpd_send_json(req, "{\"ok\":true}");
}

/* --------------------------------------------------------- POST /api/factory-reset */

static esp_err_t factory_reset_post(httpd_req_t *req)
{
    esp_err_t err = app_cfg_factory_reset();
    if (err != ESP_OK) {
        return httpd_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    restart_after(500);
    return httpd_send_json(req, "{\"ok\":true,\"restarting_in_ms\":500}");
}

/* Ask app_display for a redraw. Returns at once: the refresh itself is 26 seconds, and
 * an HTTP handler that waited for it would block the server and time out the browser. The
 * frame hash then makes a redundant request free -- a repeated click redraws nothing if
 * the picture has not changed. */
static esp_err_t refresh_post(httpd_req_t *req)
{
    const app_httpd_panel_hooks_t *panel = app_httpd_panel();
    if (!panel || !panel->refresh) {
        return httpd_send_error(req, "501 Not Implemented", "no panel in this build");
    }
    uint32_t wait_s = panel->refresh();
    if (wait_s > 0) {
        /* 429 with the wait, rather than a cheerful 200 that did nothing. E Ink advise
         * against refreshing this panel more often than every three minutes (PLAN 3e),
         * and a UI that hides the refusal teaches people to click harder. */
        char body[96];
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"queued\":false,\"retry_after_s\":%u}", (unsigned)wait_s);
        httpd_resp_set_status(req, "429 Too Many Requests");
        return httpd_send_json(req, body);
    }
    return httpd_send_json(req, "{\"ok\":true,\"queued\":true}");
}

/* Set where the forecast is fetched for. Validated inside app_wx rather than here: the
 * check that matters is that a coordinate is a number and nothing else, since it is
 * interpolated into an HTTPS query, and that belongs next to the code that builds the
 * URL rather than next to the code that parses the form. */
static esp_err_t location_post(httpd_req_t *req)
{
    const app_httpd_panel_hooks_t *panel = app_httpd_panel();
    if (!panel || !panel->set_location) {
        return httpd_send_error(req, "501 Not Implemented", "no panel in this build");
    }

    char body[BODY_MAX];
    if (httpd_read_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_send_error(req, "400 Bad Request", "unreadable or oversized body");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return httpd_send_error(req, "400 Bad Request", "body is not JSON");
    }
    const cJSON *lat   = cJSON_GetObjectItemCaseSensitive(root, "latitude");
    const cJSON *lon   = cJSON_GetObjectItemCaseSensitive(root, "longitude");
    const cJSON *place = cJSON_GetObjectItemCaseSensitive(root, "place");
    if (!cJSON_IsString(lat) || !cJSON_IsString(lon) || !cJSON_IsString(place)) {
        cJSON_Delete(root);
        return httpd_send_error(req, "400 Bad Request",
                                "latitude, longitude and place are all required, as strings");
    }

    esp_err_t err = panel->set_location(lat->valuestring, lon->valuestring,
                                        place->valuestring);
    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_send_error(req, "400 Bad Request",
                                "latitude must be -90..90, longitude -180..180, both plain "
                                "decimals; place is 1-31 printable ASCII characters");
    }
    if (err != ESP_OK) {
        return httpd_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    return httpd_send_json(req, "{\"ok\":true,\"applies\":\"at the next fetch\"}");
}

/* ----------------------------------------------------------------- crash dumps */

static esp_err_t coredump_get(httpd_req_t *req)
{
    app_diag_coredump_t cd;
    app_diag_coredump_info(&cd);

    if (!cd.present) {
        return httpd_send_json(req, "{\"present\":false}");
    }

    /* The task name and the panic reason come from the crashed image, not from us, so
     * they are escaped like any other untrusted text on this page. */
    char task[64], reason[192];
    json_escape(cd.task, task, sizeof(task));
    json_escape(cd.reason, reason, sizeof(reason));

    char body[400];
    snprintf(body, sizeof(body),
             "{\"present\":true,\"size\":%u,\"task\":\"%s\",\"pc\":\"0x%08x\","
             "\"reason\":\"%s\",\"elf_sha256\":\"%s\",\"downloadable\":%s}",
             (unsigned)cd.size, task, (unsigned)cd.pc, reason, cd.elf_sha256,
#if CONFIG_APP_HTTPD_COREDUMP_DOWNLOAD
             "true"
#else
             "false"
#endif
             );
    return httpd_send_json(req, body);
}

#if CONFIG_APP_HTTPD_COREDUMP_DOWNLOAD
/* Chunked, because 64 KB will not sit in a response buffer and holding it in RAM during
 * an OTA is exactly the competition PLAN §7 warns about. */
static esp_err_t coredump_bin_get(httpd_req_t *req)
{
    app_diag_coredump_t cd;
    app_diag_coredump_info(&cd);
    if (!cd.present) {
        return httpd_send_error(req, "404 Not Found", "no crash dump stored");
    }

    char *buf = malloc(1024);
    if (!buf) {
        return httpd_send_error(req, "503 Service Unavailable", "out of memory");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < cd.size; ) {
        size_t n = app_diag_coredump_read(off, buf, 1024);
        if (n == 0) {
            break;
        }
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) {
            free(buf);
            return err;           /* client hung up; do not send the terminating chunk */
        }
        off += n;
    }
    free(buf);
    return httpd_resp_send_chunk(req, NULL, 0);
}
#endif

static esp_err_t coredump_erase_post(httpd_req_t *req)
{
    /* The partition holds exactly one dump and the *first* crash after an erase is the
     * one kept, so erasing is how you arm it for the next one -- not just tidying up. */
    esp_err_t err = app_diag_coredump_erase();
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return httpd_send_error(req, "501 Not Implemented",
                                "crash dumps are not built into this firmware");
    }
    if (err != ESP_OK) {
        return httpd_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    return httpd_send_json(req, "{\"ok\":true}");
}

/* -------------------------------------------------------------- POST /api/ota/check */

static esp_err_t ota_check_post(httpd_req_t *req)
{
    const app_httpd_ota_hooks_t *ota = app_httpd_ota();
    if (!ota || !ota->check) {
        return httpd_send_error(req, "501 Not Implemented",
                                "OTA is not built into this firmware yet");
    }

    /* Anything arriving here came from a person -- the page's button, or a curl someone
     * typed -- so it is a forced check by default and retries a quarantined version.
     * {"forced":false} asks for the automatic poll's behaviour instead, which is how the
     * quarantine can be exercised without waiting six hours for it. */
    bool forced = true;
    char body[BODY_MAX] = "";
    if (req->content_len > 0) {
        if (httpd_read_body(req, body, sizeof(body)) != ESP_OK) {
            return httpd_send_error(req, "400 Bad Request", "unreadable or oversized body");
        }
        cJSON *root = cJSON_Parse(body);
        if (!root) {
            return httpd_send_error(req, "400 Bad Request", "body is not JSON");
        }
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, "forced");
        if (cJSON_IsBool(f)) {
            forced = cJSON_IsTrue(f);
        }
        cJSON_Delete(root);
    }

    esp_err_t err = ota->check(forced);
    if (err == ESP_ERR_NOT_FINISHED) {
        /* The running image has not been confirmed yet, so ESP-IDF would refuse to move
         * the boot partition anyway. Say that, rather than letting it fail a megabyte in. */
        return httpd_send_error(req, "409 Conflict",
                                "this image is still on probation -- its self-test has to "
                                "finish before another update can be installed");
    }
    if (err != ESP_OK) {
        return httpd_send_error(req, "503 Service Unavailable", esp_err_to_name(err));
    }
    return httpd_send_json(req, forced ? "{\"ok\":true,\"result\":\"queued\",\"forced\":true}"
                                       : "{\"ok\":true,\"result\":\"queued\",\"forced\":false}");
}

/* ------------------------------------------------------------------ registration */

void app_httpd_register_api(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/status",        .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/scan",          .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/restart",       .method = HTTP_POST, .handler = restart_post },
        { .uri = "/api/led",           .method = HTTP_POST, .handler = led_post },
        { .uri = "/api/provision",     .method = HTTP_POST, .handler = provision_post },
        { .uri = "/api/forget",        .method = HTTP_POST, .handler = forget_post },
        { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_post },
        { .uri = "/api/ota/check",     .method = HTTP_POST, .handler = ota_check_post },
        { .uri = "/api/refresh",       .method = HTTP_POST, .handler = refresh_post },
        { .uri = "/api/location",      .method = HTTP_POST, .handler = location_post },
        { .uri = "/api/coredump",      .method = HTTP_GET,  .handler = coredump_get },
        { .uri = "/api/coredump/erase",.method = HTTP_POST, .handler = coredump_erase_post },
#if CONFIG_APP_HTTPD_COREDUMP_DOWNLOAD
        { .uri = "/api/coredump.bin",  .method = HTTP_GET,  .handler = coredump_bin_get },
#endif
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s: %s", uris[i].uri, esp_err_to_name(err));
        }
    }
}
