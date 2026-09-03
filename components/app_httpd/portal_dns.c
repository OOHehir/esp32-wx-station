/* Captive-portal plumbing: a DNS responder that points every name at the device, and
 * handlers for the probe URLs each OS uses to decide whether it is behind a portal.
 *
 * Get this right and the login sheet pops by itself when a phone joins the AP. Get it
 * wrong and the user has to somehow know to browse to 192.168.4.1 (PLAN §4b). The
 * probes differ per OS and change between releases, so browsing to the AP address
 * directly is kept working as the fallback-to-the-fallback.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_net.h"
#include "esp_log.h"
#include "dns_pack.h"
#include "httpd_internal.h"
#include "lwip/inet.h"
#include "sdkconfig.h"

static const char *TAG = "app_httpd";

#define DNS_PORT 53
#define DNS_BUF  512

static TaskHandle_t s_dns_task;
static volatile bool s_dns_run;

/* --------------------------------------------------------------- HTTP probe side */

static esp_err_t captive_redirect(httpd_req_t *req)
{
    app_net_info_t net;
    app_net_get_info(&net);

    if (!net.ap_active) {
        /* On a real network an unknown path is simply not found. Redirecting there
         * would make the device look like a portal to everything on the LAN. */
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "not found\n");
    }

    char location[64];
    snprintf(location, sizeof(location), "http://%s/", net.ap_ip);

    /* 302 rather than the "success" body each OS is looking for: that mismatch is
     * precisely the signal that opens the captive-portal sheet. */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, "<html><body>Redirecting to the setup page.</body></html>");
}

void portal_register_probes(httpd_handle_t server)
{
    /* The URLs iOS, Android, Windows and Firefox probe. The wildcard is registered last
     * so it only sees paths no real handler claimed. */
    static const char *const probes[] = {
        "/generate_204",           /* Android */
        "/gen_204",
        "/hotspot-detect.html",    /* iOS, macOS */
        "/library/test/success.html",
        "/ncsi.txt",               /* Windows */
        "/connecttest.txt",
        "/redirect",
        "/canonical.html",         /* Firefox */
        "/success.txt",
        "/*",                      /* the fallback-to-the-fallback */
    };

    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        httpd_uri_t uri = {
            .uri = probes[i], .method = HTTP_GET, .handler = captive_redirect,
        };
        esp_err_t err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "register probe %s: %s", probes[i], esp_err_to_name(err));
        }
    }
}

/* ----------------------------------------------------------------- DNS responder */

static void dns_task(void *arg)
{
    (void)arg;
    int sock = -1;
    uint8_t *rx = malloc(DNS_BUF);
    uint8_t *tx = malloc(DNS_BUF);

    if (!rx || !tx) {
        ESP_LOGE(TAG, "captive DNS: out of memory");
        free(rx); free(tx);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_dns_run) {
        app_net_info_t net;
        app_net_get_info(&net);

        /* The socket exists only while the AP does, and binds only to the AP address,
         * so this never answers for clients on the real network. */
        if (!net.ap_active || net.ap_ip[0] == '\0') {
            if (sock >= 0) {
                close(sock);
                sock = -1;
                ESP_LOGI(TAG, "captive DNS stopped");
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_port = htons(DNS_PORT),
                .sin_addr.s_addr = inet_addr(net.ap_ip),
            };
            if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                ESP_LOGW(TAG, "captive DNS bind %s:53 failed", net.ap_ip);
                close(sock);
                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ESP_LOGI(TAG, "captive DNS answering on %s:53", net.ap_ip);
        }

        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, rx, DNS_BUF, 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;   /* the 1 s timeout, which is also how ap_active gets re-checked */
        }

        int reply = dns_build_reply(rx, (size_t)n, tx, DNS_BUF, inet_addr(net.ap_ip));
        if (reply > 0) {
            sendto(sock, tx, reply, 0, (struct sockaddr *)&from, from_len);
        }
    }

    if (sock >= 0) {
        close(sock);
    }
    free(rx);
    free(tx);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t portal_dns_start(void)
{
    if (s_dns_task) {
        return ESP_OK;
    }
    s_dns_run = true;
    if (xTaskCreate(dns_task, "portal_dns", 3072, NULL, 4, &s_dns_task) != pdPASS) {
        s_dns_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void portal_dns_stop(void) { s_dns_run = false; }
