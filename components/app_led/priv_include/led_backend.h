/* The hardware half of app_led.
 *
 * Exactly one backend_*.c is compiled per build (see CMakeLists.txt). Keeping the
 * choice in the build rather than in #ifdefs means app_led.c contains no per-target
 * code at all, which is the rule PLAN §1a sets for the whole template.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t led_backend_init(void);

/* Drive the LED. A monochrome backend ignores the colour and looks only at `on`; an
 * addressable one uses all four arguments. */
void led_backend_write(bool on, uint8_t r, uint8_t g, uint8_t b);

/* False when the build has no LED hardware configured. */
bool led_backend_present(void);
