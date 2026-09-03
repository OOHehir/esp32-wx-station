/* app_led -- the single owner of the status LED.
 *
 * Two things want this LED: connectivity status wants to blink it, and the web UI's
 * toggle button wants to hold it. PLAN §7 flags what happens without one owner -- the
 * button looks broken while the status pattern keeps overwriting it -- so there is
 * exactly one writer here and an explicit manual-override state.
 *
 * The LED itself is a per-target detail (PLAN §1a): plain GPIO, addressable, or absent
 * entirely, chosen in sdkconfig.defaults.<target>. Every function below is safe to call
 * when no LED is configured; they simply do nothing.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_LED_STATUS_OFF = 0,   /* dark */
    APP_LED_STATUS_BOOT,      /* starting up, before the network state is known */
    APP_LED_STATUS_CONNECTING,/* slow blink: trying stored credentials */
    APP_LED_STATUS_PORTAL,    /* double blink: provisioning AP is up (readable across a room) */
    APP_LED_STATUS_CONNECTED, /* heartbeat: associated and addressable */
    APP_LED_STATUS_UPDATING,  /* fast strobe: an OTA image is being written */
    APP_LED_STATUS_ERROR,     /* burst: something needs a human */
    APP_LED_STATUS_MAX,
} app_led_status_t;

esp_err_t app_led_init(void);

/* Status is what the firmware wants to say. It is remembered even while a manual
 * override is in force, and resumes when the override is released. */
void             app_led_set_status(app_led_status_t status);
app_led_status_t app_led_get_status(void);
const char      *app_led_status_name(app_led_status_t status);

/* Manual override -- the web UI's toggle. Takes the LED away from the status pattern
 * and holds it until released. */
void app_led_manual_set(bool on);
/* Flip the override, taking the LED from the status pattern if it does not already hold
 * it. Returns the state the LED actually landed in -- not the state it was asked for. */
bool app_led_manual_toggle(void);
void app_led_manual_release(void);
bool app_led_manual_active(void);

/* Whether the LED is lit right now, whoever is driving it. */
bool app_led_is_on(void);

/* False when the build has no LED configured, so callers (the web UI) can hide a
 * control that would do nothing rather than offer a button that lies. */
bool app_led_present(void);

#ifdef __cplusplus
}
#endif
