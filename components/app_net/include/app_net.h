/* app_net -- getting and staying on a network.
 *
 * PLAN §4b: commissioning is a first-class state machine, not something bolted on at
 * the end. Four states, and the one that matters most is Fallback:
 *
 *   Unprovisioned  no credentials in NVS      -> raise the AP immediately
 *   Connecting     credentials present        -> STA connect with backoff
 *   Fallback       no association after N s   -> raise the AP *without giving up on STA*
 *   Connected      association + DHCP lease   -> tear the AP down, carry on
 *
 * The AP runs in WIFI_MODE_APSTA and the STA retries continue underneath it. A device
 * that flips to AP mode and stops retrying gets stranded by a router reboot, and
 * stranded means somebody walks to it.
 *
 * This component owns the Wi-Fi driver, SNTP and mDNS. It does not own any UI: the
 * portal is served by app_httpd, which drives commissioning through app_net_scan() and
 * app_net_provision() below.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(APP_NET_EVENT);

typedef enum {
    APP_NET_EVENT_STATE_CHANGED,  /* data: app_net_state_t */
    APP_NET_EVENT_GOT_IP,         /* data: app_net_info_t   */
    APP_NET_EVENT_LOST_IP,
    APP_NET_EVENT_TIME_SYNCED,
} app_net_event_id_t;

typedef enum {
    APP_NET_STATE_UNPROVISIONED = 0,
    APP_NET_STATE_CONNECTING,
    APP_NET_STATE_FALLBACK,
    APP_NET_STATE_CONNECTED,
} app_net_state_t;

/* Result of the last commissioning attempt, so the portal can report *why* a join
 * failed rather than leaving the user staring at a form. Dropping the AP the instant
 * credentials are saved is the classic provisioning bug (PLAN §4b). */
typedef enum {
    APP_NET_PROV_IDLE = 0,
    APP_NET_PROV_PENDING,
    APP_NET_PROV_OK,
    APP_NET_PROV_BAD_AUTH,
    APP_NET_PROV_NOT_FOUND,
    APP_NET_PROV_FAILED,
} app_net_prov_result_t;

typedef struct {
    app_net_state_t state;
    bool     sta_connected;      /* associated *and* holding a lease */
    bool     ap_active;
    char     ssid[33];           /* the network we are on or trying */
    char     ip[16];
    char     gateway[16];
    char     ap_ssid[33];
    char     ap_ip[16];
    int8_t   rssi;
    uint32_t retries;            /* consecutive failed STA attempts */
    bool     time_synced;
    app_net_prov_result_t prov_result;
} app_net_info_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    bool    open;                /* no passphrase needed */
} app_net_ap_t;

/* Requires nvs_flash, the default event loop and esp_netif to be up already --
 * app_core does that. Returns as soon as the state machine is running; it does not
 * block waiting for a network. */
esp_err_t app_net_init(void);

void            app_net_get_info(app_net_info_t *out);
app_net_state_t app_net_state(void);
const char     *app_net_state_name(app_net_state_t state);
bool            app_net_time_synced(void);

/* Block up to timeout_ms for the clock to be set, returning what it ended up as.
 *
 * SNTP is best-effort and its timing varies wildly -- observed anywhere from 7 s to 90 s
 * after the lease, depending on whether the DHCP-supplied server answers. Anything that
 * needs a valid clock should wait a little rather than fail the moment it finds one
 * missing. Call from a task, never from an event handler or an HTTP handler. */
bool            app_net_wait_time_sync(uint32_t timeout_ms);

/* Store credentials and try them now. Returns immediately; watch prov_result in
 * app_net_get_info() for the outcome. */
esp_err_t app_net_provision(const char *ssid, const char *password);

/* Clear stored credentials and drop back to Unprovisioned. The recovery path for a
 * mis-provisioned board. */
esp_err_t app_net_forget(void);

/* Blocking active scan. Safe to call while connected, at the cost of a brief pause in
 * traffic. Results are sorted strongest-first with duplicate SSIDs collapsed. */
esp_err_t app_net_scan(app_net_ap_t *out, size_t max, size_t *count);

/* The passphrase for the provisioning AP -- printed at boot and, ideally, on the label. */
const char *app_net_ap_passphrase(void);

#ifdef __cplusplus
}
#endif
