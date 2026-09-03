#include "app_btn.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_APP_BTN_ENABLE

#include "app_display.h"
#include "app_httpd.h"
#include "app_ota.h"

static const char *TAG = "app_btn";

#define POLL_MS 20

static const int PINS[APP_BTN_COUNT] = {
    [APP_BTN_A] = CONFIG_APP_BTN_A_GPIO,
    [APP_BTN_B] = CONFIG_APP_BTN_B_GPIO,
    [APP_BTN_C] = CONFIG_APP_BTN_C_GPIO,
};

static app_btn_cb_t s_cb;

/* Counted so a press can be checked after the fact.
 *
 * Watching the log requires somebody to be pressing at the moment somebody else is
 * capturing, and on a board being brought up remotely those are different people in
 * different rooms. `edges` is the one that matters: it counts raw level changes, so a
 * wire that is connected but whose press never survives debounce still shows a number,
 * and a wire that is not connected at all shows a zero that cannot be argued with. */
static uint32_t s_short[APP_BTN_COUNT];
static uint32_t s_long[APP_BTN_COUNT];
static uint32_t s_edges[APP_BTN_COUNT];

static int button_status_json(char *buf, size_t len)
{
    return snprintf(buf, len,
        "\"a\":{\"level\":%d,\"edges\":%u,\"short\":%u,\"long\":%u},"
        "\"b\":{\"level\":%d,\"edges\":%u,\"short\":%u,\"long\":%u},"
        "\"c\":{\"level\":%d,\"edges\":%u,\"short\":%u,\"long\":%u}",
        gpio_get_level(PINS[APP_BTN_A]), (unsigned)s_edges[APP_BTN_A],
        (unsigned)s_short[APP_BTN_A], (unsigned)s_long[APP_BTN_A],
        gpio_get_level(PINS[APP_BTN_B]), (unsigned)s_edges[APP_BTN_B],
        (unsigned)s_short[APP_BTN_B], (unsigned)s_long[APP_BTN_B],
        gpio_get_level(PINS[APP_BTN_C]), (unsigned)s_edges[APP_BTN_C],
        (unsigned)s_short[APP_BTN_C], (unsigned)s_long[APP_BTN_C]);
}

static const app_httpd_button_hooks_t BUTTON_HOOKS = {
    .status_json = button_status_json,
};

const char *app_btn_name(app_btn_id_t id)
{
    switch (id) {
    case APP_BTN_A:     return "A";
    case APP_BTN_B:     return "B";
    case APP_BTN_C:     return "C";
    case APP_BTN_COUNT: break;
    }
    return "?";
}

/* What the buttons do by default.
 *
 * PLAN 3b assigns the pins and stops there, so these are this project's choice rather
 * than the plan's, and they are deliberately the three things somebody standing in front
 * of a panel actually wants: make it current, make it check for firmware, and make it
 * repaint when the glass and the software disagree.
 *
 * None of them does the slow thing itself. A refresh is 26 seconds and an update is
 * minutes; both are requests to a task that can afford to wait, so a press returns at
 * once and the button never feels dead.
 */
static void default_action(app_btn_id_t id, bool long_press)
{
    switch (id) {
    case APP_BTN_A: {
        /* Refresh now, subject to the panel's own three-minute limit. The refusal is
         * logged with the wait rather than swallowed, for the reason PLAN 3e gives: a
         * button that silently does nothing reads as broken hardware. */
        uint32_t wait_s = app_display_request_refresh();
        if (wait_s > 0) {
            ESP_LOGW(TAG, "A: refresh refused, %u s to wait", (unsigned)wait_s);
        } else {
            ESP_LOGI(TAG, "A: refresh requested");
        }
        break;
    }

    case APP_BTN_B:
        /* Check for firmware. forced=true because a finger on the button *is* a human
         * asking, which is exactly the case that may retry a version the device has
         * quarantined -- the opposite of the fleet-wide MQTT push, which must not. */
        ESP_LOGI(TAG, "B: checking for an update");
        if (app_ota_check_now(true) != ESP_OK) {
            ESP_LOGW(TAG, "B: check refused -- busy, or this image is on probation");
        }
        break;

    case APP_BTN_C:
        /* Long press only, and it forces a repaint past the frame hash.
         *
         * The hash records what this firmware believes is on the glass, and the two can
         * genuinely diverge -- a refresh interrupted by a power cut, or a panel that was
         * cleaned by hand. There is otherwise no way to say "I am looking at it, and it
         * is wrong". Long press because 26 seconds of panel wear should not be one
         * accidental brush away. */
        if (!long_press) {
            ESP_LOGI(TAG, "C: hold it to force a repaint");
            break;
        }
        ESP_LOGI(TAG, "C: forcing a repaint");
        app_display_force_repaint();
        break;

    case APP_BTN_COUNT:
        break;
    }
}

/* Report the raw pin levels whenever they change.
 *
 * Bring-up evidence, and the same reasoning as app_epd's BUSY diagnostics: when a button
 * does nothing, the first question is whether the pin moved at all, and "no press was
 * detected" cannot distinguish a wire that is not there from logic that is wrong. A
 * coalesced snapshot rather than an edge log, so switch bounce does not fill the console.
 */
static void log_levels_if_changed(void)
{
    static int last[APP_BTN_COUNT] = { -1, -1, -1 };
    int now[APP_BTN_COUNT];
    bool changed = false;

    for (int i = 0; i < APP_BTN_COUNT; i++) {
        now[i] = gpio_get_level(PINS[i]);
        if (now[i] != last[i]) {
            changed = true;
            if (last[i] >= 0) {
                s_edges[i]++;   /* not the first snapshot, so this is a real transition */
            }
        }
    }
    if (!changed) {
        return;
    }
    for (int i = 0; i < APP_BTN_COUNT; i++) {
        last[i] = now[i];
    }
    ESP_LOGI(TAG, "pins: A(GPIO%d)=%d B(GPIO%d)=%d C(GPIO%d)=%d  (0 = pressed)",
             PINS[APP_BTN_A], now[APP_BTN_A], PINS[APP_BTN_B], now[APP_BTN_B],
             PINS[APP_BTN_C], now[APP_BTN_C]);
}

/* Announce the press, then run the action. One line per press, at the moment the press
 * is recognised, so "did the button work" is answered by the log rather than by whether
 * the action it triggered happened to have a visible effect. */
static void fire(app_btn_id_t id, bool long_press)
{
    ESP_LOGI(TAG, "button %s pressed (%s)", app_btn_name(id),
             long_press ? "long" : "short");

    app_btn_cb_t cb = s_cb;
    (cb ? cb : default_action)(id, long_press);
}

static void btn_task(void *arg)
{
    (void)arg;

    /* Debounce state per button, in poll ticks rather than milliseconds so the arithmetic
     * stays integer and the comparison is against a count the loop already keeps. */
    int      held_ticks[APP_BTN_COUNT] = { 0 };
    bool     reported[APP_BTN_COUNT]   = { false };
    const int debounce_ticks = CONFIG_APP_BTN_DEBOUNCE_MS / POLL_MS;
    const int long_ticks     = CONFIG_APP_BTN_LONG_PRESS_MS / POLL_MS;

    for (;;) {
        log_levels_if_changed();

        for (int i = 0; i < APP_BTN_COUNT; i++) {
            /* Pull-up, button to ground: low is pressed. */
            const bool down = gpio_get_level(PINS[i]) == 0;

            if (!down) {
                /* Released. A press is reported on release for a short press, so that a
                 * hold can still become a long press without also firing the short one
                 * on the way through. */
                if (held_ticks[i] > debounce_ticks && !reported[i]) {
                    s_short[i]++;
                    fire((app_btn_id_t)i, false);
                }
                held_ticks[i] = 0;
                reported[i] = false;
                continue;
            }

            held_ticks[i]++;

            /* A long press fires the moment the threshold is crossed, while the finger is
             * still down. Waiting for release would make a deliberate hold feel like
             * nothing happened for a second and a half. */
            if (!reported[i] && long_ticks > 0 && held_ticks[i] >= long_ticks) {
                reported[i] = true;
                s_long[i]++;
                fire((app_btn_id_t)i, true);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void app_btn_set_callback(app_btn_cb_t cb)
{
    s_cb = cb;
}

esp_err_t app_btn_start(void)
{
    for (int i = 0; i < APP_BTN_COUNT; i++) {
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << PINS[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&c);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "button %s on GPIO%d: %s", app_btn_name((app_btn_id_t)i),
                     PINS[i], esp_err_to_name(err));
            return err;
        }
    }

    /* 3072 is generous for a poll loop, and it is the callbacks that set the floor: they
     * run on this stack, and the default ones reach into app_ota. */
    if (xTaskCreate(btn_task, "app_btn", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    app_httpd_set_button_hooks(&BUTTON_HOOKS);

    ESP_LOGI(TAG, "A=GPIO%d B=GPIO%d C=GPIO%d, %d ms debounce, %d ms long press",
             PINS[APP_BTN_A], PINS[APP_BTN_B], PINS[APP_BTN_C],
             CONFIG_APP_BTN_DEBOUNCE_MS, CONFIG_APP_BTN_LONG_PRESS_MS);
    return ESP_OK;
}

#else /* !CONFIG_APP_BTN_ENABLE */

esp_err_t app_btn_start(void) { return ESP_OK; }
void app_btn_set_callback(app_btn_cb_t cb) { (void)cb; }
const char *app_btn_name(app_btn_id_t id) { (void)id; return "?"; }

#endif
