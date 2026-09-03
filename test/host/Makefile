# Host tests. No ESP-IDF, no toolchain, no hardware:
#
#     make -C test/host
#
# Only units that are pure C and whose bugs are silent on a device live here. The Wi-Fi
# state machine and the NVS wrappers are deliberately absent -- testing them would mean
# mocking most of ESP-IDF, and the mocks would be what got tested.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
CPPFLAGS = -I. -I../../components/app_httpd/priv_include \
           -I../../components/app_ota/priv_include \
           -I../../components/app_mqtt/priv_include \
           -I../../components/app_gfx/include \
           -I../../components/app_wx/include \
           -I../../components/app_wx/priv_include \
           -I../../managed_components/espressif__cjson/cJSON \
           -I../../components/app_ui/include
LDFLAGS  = -fsanitize=address,undefined

UNITS = ../../components/app_httpd/json_util.c \
        ../../components/app_httpd/dns_pack.c \
        ../../components/app_ota/update_policy.c \
        ../../components/app_ota/rollback_policy.c \
        ../../components/app_ota/sha_hex.c \
        ../../components/app_mqtt/mqtt_cmd.c \
        ../../components/app_gfx/app_gfx.c \
        ../../components/app_gfx/font_big.c \
        ../../components/app_gfx/font_label.c \
        ../../components/app_gfx/font_small.c \
        ../../components/app_wx/wx_series.c \
        ../../components/app_wx/wx_parse.c \
        ../../components/app_wx/wx_location.c \
        ../../managed_components/espressif__cjson/cJSON/cJSON.c \
        ../../components/app_ui/app_ui.c
TESTS = main.c test_json_util.c test_dns_pack.c test_update_policy.c \
        test_rollback_policy.c test_sha_hex.c test_mqtt_cmd.c \
        test_app_gfx.c test_app_ui.c test_wx_parse.c

BIN = run_tests

.PHONY: all check clean preview
all: check

check: $(BIN)
	./$(BIN)

$(BIN): $(UNITS) $(TESTS) test.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(UNITS) $(TESTS) $(LDFLAGS)

# Renders every app_ui mode to /tmp so the layout can be looked at rather than reasoned
# about. See the comment at the top of render_ui.c.
PREVIEW_UNITS = ../../components/app_ui/app_ui.c ../../components/app_gfx/app_gfx.c \
                ../../components/app_gfx/font_big.c ../../components/app_gfx/font_label.c \
                ../../components/app_gfx/font_small.c ../../components/app_wx/wx_series.c

preview: render_ui.c $(PREVIEW_UNITS)
	$(CC) -std=c11 -Wall -O1 $(CPPFLAGS) -o /tmp/wx-render $^
	@for m in ok stale none prov; do \
	    /tmp/wx-render /tmp/wx-$$m.ppm $$m; \
	    echo "  /tmp/wx-$$m.ppm"; \
	done

clean:
	rm -rf $(BIN) $(BIN).dSYM
