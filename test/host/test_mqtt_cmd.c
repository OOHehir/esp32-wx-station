#include "test.h"
#include "mqtt_cmd.h"

void test_mqtt_cmd(void)
{
    SUITE("mqtt_cmd_parse -- the commands");

    CHECK_INT(mqtt_cmd_parse("check", 5), MQTT_CMD_CHECK);
    CHECK_INT(mqtt_cmd_parse("restart", 7), MQTT_CMD_RESTART);
    CHECK_INT(mqtt_cmd_parse("reboot", 6), MQTT_CMD_RESTART);
    CHECK_INT(mqtt_cmd_parse("led-on", 6), MQTT_CMD_LED_ON);
    CHECK_INT(mqtt_cmd_parse("led-off", 7), MQTT_CMD_LED_OFF);
    CHECK_INT(mqtt_cmd_parse("led-auto", 8), MQTT_CMD_LED_AUTO);
    CHECK_INT(mqtt_cmd_parse("status", 6), MQTT_CMD_STATUS);
    CHECK_INT(mqtt_cmd_parse("refresh", 7), MQTT_CMD_REFRESH);
    CHECK_INT(mqtt_cmd_parse("redraw", 6), MQTT_CMD_REFRESH);
    CHECK_INT(mqtt_cmd_parse("REFRESH", 7), MQTT_CMD_REFRESH);

    /* Shells, dashboards and `echo` all add a newline. Rejecting that would make the
     * obvious command-line invocation silently do nothing. */
    CHECK_INT(mqtt_cmd_parse("check\n", 6), MQTT_CMD_CHECK);
    CHECK_INT(mqtt_cmd_parse("  check  ", 9), MQTT_CMD_CHECK);
    CHECK_INT(mqtt_cmd_parse("check\r\n", 7), MQTT_CMD_CHECK);
    CHECK_INT(mqtt_cmd_parse("CHECK", 5), MQTT_CMD_CHECK);
    CHECK_INT(mqtt_cmd_parse("Restart", 7), MQTT_CMD_RESTART);

    SUITE("mqtt_cmd_parse -- anything else is ignored");

    /* A broker carries traffic nobody vetted. Anything unrecognised must do nothing at
     * all rather than fall through to a default -- and "restart" is the default nobody
     * wants. */
    CHECK_INT(mqtt_cmd_parse("", 0), MQTT_CMD_NONE);
    CHECK_INT(mqtt_cmd_parse(NULL, 5), MQTT_CMD_NONE);
    CHECK_INT(mqtt_cmd_parse("   ", 3), MQTT_CMD_NONE);
    CHECK_INT(mqtt_cmd_parse("checkx", 6), MQTT_CMD_NONE);
    CHECK_INT(mqtt_cmd_parse("chec", 4), MQTT_CMD_NONE);
    CHECK_INT(mqtt_cmd_parse("{\"cmd\":\"check\"}", 15), MQTT_CMD_NONE);
    CHECK_INT(mqtt_cmd_parse("led", 3), MQTT_CMD_NONE);

    /* A prefix of a command is not the command: a length-only comparison would accept
     * "check" from "checkmate" and a strcmp on an unterminated buffer would read past
     * the end of it. */
    CHECK_INT(mqtt_cmd_parse("checkmate", 9), MQTT_CMD_NONE);

    SUITE("mqtt_cmd_parse -- the payload is not a C string");

    /* MQTT payloads are counted, not terminated. Parsing must respect `len` and never
     * touch payload[len] -- this is the check the address sanitiser earns its keep on. */
    {
        char buf[5] = { 'c', 'h', 'e', 'c', 'k' };   /* deliberately unterminated */
        CHECK_INT(mqtt_cmd_parse(buf, 5), MQTT_CMD_CHECK);
    }
    {
        /* A longer buffer where only the first 5 bytes are the command. */
        const char *p = "checkrestart";
        CHECK_INT(mqtt_cmd_parse(p, 5), MQTT_CMD_CHECK);
        CHECK_INT(mqtt_cmd_parse(p, 12), MQTT_CMD_NONE);
    }

    for (int c = MQTT_CMD_NONE; c < MQTT_CMD__LAST; c++) {
        CHECK(mqtt_cmd_name((mqtt_cmd_t)c)[0] != '\0', "command %d has no name", c);
    }

    SUITE("mqtt_topic");

    char t[64];
    CHECK(mqtt_topic(t, sizeof(t), "esp-ota", "A1B2C3", "cmd"), "should fit");
    CHECK_STR(t, "esp-ota/A1B2C3/cmd");
    CHECK(mqtt_topic(t, sizeof(t), "esp-ota", "A1B2C3", "status"), "should fit");
    CHECK_STR(t, "esp-ota/A1B2C3/status");

    /* Truncation must fail loudly rather than produce a shorter topic that is perfectly
     * valid and completely wrong -- a device subscribed to it looks offline forever. */
    char small[10];
    CHECK(!mqtt_topic(small, sizeof(small), "esp-ota", "A1B2C3", "cmd"), "must refuse to truncate");
    CHECK_STR(small, "");

    char exact[19];   /* "esp-ota/A1B2C3/cmd" is 18 chars + NUL */
    CHECK(mqtt_topic(exact, sizeof(exact), "esp-ota", "A1B2C3", "cmd"), "exact fit is a fit");
    CHECK_STR(exact, "esp-ota/A1B2C3/cmd");

    char one_short[18];
    CHECK(!mqtt_topic(one_short, sizeof(one_short), "esp-ota", "A1B2C3", "cmd"),
          "one byte short is not a fit");

    CHECK(!mqtt_topic(t, sizeof(t), NULL, "A1B2C3", "cmd"), "NULL prefix");
    CHECK(!mqtt_topic(t, sizeof(t), "esp-ota", NULL, "cmd"), "NULL device");
    CHECK(!mqtt_topic(t, sizeof(t), "esp-ota", "A1B2C3", NULL), "NULL leaf");
    CHECK(!mqtt_topic(NULL, 10, "esp-ota", "A1B2C3", "cmd"), "NULL buffer");
    CHECK(!mqtt_topic(t, 0, "esp-ota", "A1B2C3", "cmd"), "zero-length buffer");
}
