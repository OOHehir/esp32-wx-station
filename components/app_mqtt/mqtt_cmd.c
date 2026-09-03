#include "mqtt_cmd.h"

#include <stdio.h>
#include <string.h>

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

const char *mqtt_cmd_name(mqtt_cmd_t cmd)
{
    switch (cmd) {
    case MQTT_CMD_NONE:     return "none";
    case MQTT_CMD_CHECK:    return "check";
    case MQTT_CMD_RESTART:  return "restart";
    case MQTT_CMD_LED_ON:   return "led-on";
    case MQTT_CMD_LED_OFF:  return "led-off";
    case MQTT_CMD_LED_AUTO: return "led-auto";
    case MQTT_CMD_STATUS:   return "status";
    case MQTT_CMD_REFRESH:  return "refresh";
    case MQTT_CMD__LAST:    break;
    }
    return "unknown";
}

mqtt_cmd_t mqtt_cmd_parse(const char *payload, size_t len)
{
    if (!payload || len == 0) {
        return MQTT_CMD_NONE;
    }

    /* Trim, without ever reading payload[len]. */
    size_t start = 0, end = len;
    while (start < end && is_space(payload[start])) { start++; }
    while (end > start && is_space(payload[end - 1])) { end--; }

    const size_t n = end - start;
    static const struct { const char *word; mqtt_cmd_t cmd; } TABLE[] = {
        { "check",    MQTT_CMD_CHECK    },
        { "restart",  MQTT_CMD_RESTART  },
        { "reboot",   MQTT_CMD_RESTART  },   /* the word half of people reach for first */
        { "led-on",   MQTT_CMD_LED_ON   },
        { "led-off",  MQTT_CMD_LED_OFF  },
        { "led-auto", MQTT_CMD_LED_AUTO },
        { "status",   MQTT_CMD_STATUS   },
        { "refresh",  MQTT_CMD_REFRESH  },
        { "redraw",   MQTT_CMD_REFRESH  },   /* as with reboot, the other obvious word */
    };

    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        const char *w = TABLE[i].word;
        if (strlen(w) != n) {
            continue;
        }
        size_t k = 0;
        while (k < n && lower(payload[start + k]) == w[k]) { k++; }
        if (k == n) {
            return TABLE[i].cmd;
        }
    }
    return MQTT_CMD_NONE;
}

bool mqtt_topic(char *out, size_t out_len,
                const char *prefix, const char *device, const char *leaf)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (!prefix || !device || !leaf) {
        return false;
    }
    int n = snprintf(out, out_len, "%s/%s/%s", prefix, device, leaf);
    if (n < 0 || (size_t)n >= out_len) {
        /* Truncated. Leaving the partial string would be worse than failing: it is a
         * valid topic name, just not the right one, and nothing downstream would notice. */
        out[0] = '\0';
        return false;
    }
    return true;
}
