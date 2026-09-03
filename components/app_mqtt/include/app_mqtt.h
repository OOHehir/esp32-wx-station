/* app_mqtt -- an optional broker link for command and telemetry.
 *
 * PLAN §8, and optional in the strong sense: default off, and nothing in the OTA path
 * depends on it. A device with no broker to talk to works exactly as before; a broker
 * that goes down changes nothing except that the fleet stops reporting. That constraint
 * is what keeps this an extra rather than a second thing that can strand a device.
 *
 * What it buys, and it is mostly one thing: **"check for an update now", to the whole
 * fleet at once.** Without it an update reaches devices on their own jittered six-hour
 * poll, which is correct and unhurried and no help at all when you have just published a
 * fix and want to watch it land. `mosquitto_pub -t esp-ota/all/cmd -m check` is that.
 *
 * Deliberately thin. It publishes what the status page already renders and accepts the
 * commands the status page already offers, because a second control surface that can do
 * *different* things is a second thing to reason about when a device misbehaves.
 *
 * Trust is the same private CA the OTA path uses (PLAN §3), so there is no second anchor
 * to manage. Credentials are a username and password shared across the fleet, which is a
 * real limit: see tools/gen_mqtt_user.sh for why that is acceptable for "check now" and
 * where to go if it stops being.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Safe to call with no network up and with MQTT compiled out. The client connects when
 * the network arrives and reconnects on its own; nothing here blocks. */
esp_err_t app_mqtt_init(void);

/* True while the broker link is up. The status page shows this so that "the fleet went
 * quiet" can be told apart from "this device went quiet". */
bool app_mqtt_connected(void);

/* Publish the status document now, instead of waiting for the interval. Called on
 * interesting transitions -- a state change worth knowing about should not wait 60 s. */
void app_mqtt_publish_status(void);

#ifdef __cplusplus
}
#endif
