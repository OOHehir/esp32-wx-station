/* app_httpd -- the device's status page, control surface and provisioning portal.
 *
 * PLAN §4a: one page, so that "what is this board running, and is there an update?" is
 * answerable from a phone on the same Wi-Fi with no serial cable and no laptop
 * toolchain. PLAN §4b reuses the same component, not a second web stack, for the
 * commissioning portal -- it is one extra view, not another server.
 *
 * The HTML is a single embedded asset with no CDN and no build step, because these
 * devices routinely sit on a network with no route to the internet and anything fetched
 * from outside renders as a broken page. State arrives as JSON, so curl drives exactly
 * the same surface as the browser.
 *
 * Deliberately no authentication and no TLS (PLAN §4a): anyone who can reach the board
 * can reboot it. That is a reasonable trade on a lab network and a poor one anywhere
 * else, which is why the whole component sits behind CONFIG_APP_HTTPD_ENABLE.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The update controls call through this table rather than against app_ota directly, so
 * app_httpd does not depend on it: unset, the page reports OTA as unavailable and
 * /api/ota/check returns 501, which is what a build with CONFIG_APP_OTA_ENABLE=n gets.
 */
typedef struct {
    /* Queue a manifest check on the OTA task and return immediately. Never download
     * inside an HTTP handler -- it blocks the server for minutes and times out the
     * browser (PLAN §4a).
     *
     * `forced` says a human asked, which is what allows a retry of a version the device
     * has quarantined after repeated failures. */
    esp_err_t (*check)(bool forced);
    /* True while an image is being written, so /api/restart can refuse with 409 rather
     * than let a stray click abandon a half-written slot. */
    bool (*busy)(void);
    /* Write the body of the "ota" object of /api/status (no braces) into buf. Returns
     * the number of bytes written. */
    int (*status_json)(char *buf, size_t len);
} app_httpd_ota_hooks_t;

/* The panel controls call through this table for the same reason the update controls do:
 * app_httpd is inherited template code, and a status page that named app_display directly
 * would drag this project's components into it and grow the diff against upstream.
 * Unset, /api/refresh returns 501 and /api/status carries no "panel" object. */
typedef struct {
    /* Ask for a redraw and return immediately. Never refresh inside an HTTP handler: a
     * panel refresh is 26 seconds, which blocks the server and times out the browser.
     *
     * Returns 0 when queued, or the seconds still to wait when the panel's own rate limit
     * refused it. The handler reports that number rather than pretending success. */
    uint32_t (*refresh)(void);
    /* Write the body of the "panel" object of /api/status (no braces) into buf. Returns
     * the number of bytes written. */
    int (*status_json)(char *buf, size_t len);
    /* Validate and store a location. ESP_ERR_INVALID_ARG when a field is refused, with
     * nothing stored. Strings, because they are decimal degrees with a sign. */
    esp_err_t (*set_location)(const char *lat, const char *lon, const char *place);
} app_httpd_panel_hooks_t;

/* The same arrangement again, for the buttons. A separate table rather than a field on
 * the panel one because app_btn already depends on app_display -- routing its status
 * through app_display would make that a cycle. */
typedef struct {
    /* Write the body of the "buttons" object of /api/status (no braces). */
    int (*status_json)(char *buf, size_t len);
} app_httpd_button_hooks_t;

esp_err_t app_httpd_start(void);
esp_err_t app_httpd_stop(void);
bool      app_httpd_running(void);

/* Pass NULL to unregister. Safe to call before or after app_httpd_start(). */
void app_httpd_set_ota_hooks(const app_httpd_ota_hooks_t *hooks);

/* Pass NULL to unregister. Safe to call before or after app_httpd_start(). */
void app_httpd_set_panel_hooks(const app_httpd_panel_hooks_t *hooks);

/* Pass NULL to unregister. Safe to call before or after app_httpd_start(). */
void app_httpd_set_button_hooks(const app_httpd_button_hooks_t *hooks);

#ifdef __cplusplus
}
#endif
