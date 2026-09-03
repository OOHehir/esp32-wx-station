/* Shared between the app_httpd translation units. Not part of the public API. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "app_httpd.h"
#include "esp_err.h"
#include "esp_http_server.h"

/* Registered by app_httpd.c, implemented in api.c. */
void app_httpd_register_api(httpd_handle_t server);

/* The OTA hook table, or NULL. Read-only for api.c. */
const app_httpd_ota_hooks_t *app_httpd_ota(void);
const app_httpd_panel_hooks_t *app_httpd_panel(void);
const app_httpd_button_hooks_t *app_httpd_buttons(void);

/* Send a JSON body with the headers every response here needs. */
esp_err_t httpd_send_json(httpd_req_t *req, const char *json);
esp_err_t httpd_send_error(httpd_req_t *req, const char *status, const char *message);

/* Read a request body into a NUL-terminated buffer. Returns ESP_ERR_INVALID_SIZE when
 * the body is larger than the buffer -- a control surface has no business accepting an
 * unbounded POST. */
esp_err_t httpd_read_body(httpd_req_t *req, char *buf, size_t buf_len);

#if CONFIG_APP_HTTPD_CAPTIVE_PORTAL
esp_err_t portal_dns_start(void);
void      portal_dns_stop(void);
void      portal_register_probes(httpd_handle_t server);
#endif
