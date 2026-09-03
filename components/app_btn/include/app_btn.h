/* app_btn -- the three panel buttons.
 *
 * The Inky Impression carries four tactile buttons on the Pi header; this project wires
 * three (PLAN 3b). The fourth would land on D0/GPIO2, an ESP32-C3 strapping pin that must
 * be high at reset -- a button to ground is high when released and so works perfectly,
 * until somebody happens to be holding it while plugging the board in, and then the
 * device does not boot with nothing on the console to explain it. Three is enough.
 *
 * Buttons short to ground, so the pins are inputs with internal pull-ups and a press
 * reads low.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_BTN_A = 0,
    APP_BTN_B,
    APP_BTN_C,
    APP_BTN_COUNT,
} app_btn_id_t;

/* Called from the button task when a press completes. Keep it short: it runs on a small
 * stack, and anything slow belongs behind a request to another task -- which is what all
 * three default actions do. */
typedef void (*app_btn_cb_t)(app_btn_id_t id, bool long_press);

/* Start polling. Safe to call with no buttons wired: an unconnected pin is held high by
 * its pull-up, which is exactly what "not pressed" looks like. */
esp_err_t app_btn_start(void);

/* Replace the default action table. Pass NULL to restore the built-in behaviour. */
void app_btn_set_callback(app_btn_cb_t cb);

/* Names for logs and the status page. */
const char *app_btn_name(app_btn_id_t id);

#ifdef __cplusplus
}
#endif
