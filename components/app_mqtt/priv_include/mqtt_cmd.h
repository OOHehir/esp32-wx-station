/* Commands arriving on the device's MQTT command topic, and the topics themselves.
 *
 * Free of ESP-IDF so the host tests can hold it to account. Two things live here and
 * both fail quietly if they are wrong: a parser that accepts the wrong thing reboots a
 * device somebody was not asking to reboot, and a topic builder that truncates puts a
 * device on a topic nobody is listening to -- which looks exactly like a device that is
 * offline.
 *
 * Payloads are bare words, not JSON. An MQTT command topic is not a place where a
 * structured document earns its complexity, and `mosquitto_pub -t .../cmd -m check` is
 * something you can type from memory at a terminal.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MQTT_CMD_NONE = 0,      /* not a command we know -- ignored, deliberately silently */
    MQTT_CMD_CHECK,         /* check for a firmware update now */
    MQTT_CMD_RESTART,
    MQTT_CMD_LED_ON,
    MQTT_CMD_LED_OFF,
    MQTT_CMD_LED_AUTO,      /* release the manual hold, back to the status pattern */
    MQTT_CMD_STATUS,        /* publish status now, rather than waiting for the interval */
    MQTT_CMD_REFRESH,       /* redraw the panel now, rather than waiting for the tick */

    /* Not a command. Keeps the name-coverage test honest without anybody having to
     * remember to widen its loop when a command is added -- which is exactly what was
     * forgotten when MQTT_CMD_REFRESH went in. */
    MQTT_CMD__LAST,
} mqtt_cmd_t;

/* `payload` need not be NUL-terminated -- MQTT payloads are counted, not terminated, and
 * treating one as a C string is how a parser reads past the end of a buffer. Surrounding
 * whitespace is tolerated because shells and dashboards add it; anything else is
 * MQTT_CMD_NONE. Matching is case-insensitive. */
mqtt_cmd_t mqtt_cmd_parse(const char *payload, size_t len);

const char *mqtt_cmd_name(mqtt_cmd_t cmd);

/* Build "<prefix>/<device>/<leaf>" into `out`. Returns false and writes an empty string
 * if it would not fit, so a caller cannot accidentally subscribe to a truncated topic. */
bool mqtt_topic(char *out, size_t out_len,
                const char *prefix, const char *device, const char *leaf);
