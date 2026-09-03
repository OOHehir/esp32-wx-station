#include "app_led.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "led_backend.h"
#include "sdkconfig.h"

static const char *TAG = "app_led";

/* A pattern is 16 slots of APP_LED_TICK_MS, least-significant bit first, repeating.
 * At 100 ms a slot that is a 1.6 s cycle -- long enough for a distinctive rhythm,
 * short enough that a state change is visible almost immediately. */
#define APP_LED_TICK_MS 100
#define PATTERN_SLOTS   16

typedef struct {
    const char *name;
    uint16_t    pattern;
    uint8_t     r, g, b;
} led_state_desc_t;

/* Patterns are chosen to be distinguishable by rhythm alone, because a plain-GPIO
 * board has no colour to fall back on. */
static const led_state_desc_t STATES[APP_LED_STATUS_MAX] = {
    [APP_LED_STATUS_OFF]        = { "off",        0x0000,   0,   0,   0 },
    [APP_LED_STATUS_BOOT]       = { "boot",       0x000f, 255, 255, 255 },  /* 400 ms on, 1.2 s off */
    [APP_LED_STATUS_CONNECTING] = { "connecting", 0x00ff,   0,   0, 255 },  /* even 800/800 blink */
    [APP_LED_STATUS_PORTAL]     = { "portal",     0x0005, 255, 140,   0 },  /* blink-blink-pause */
    [APP_LED_STATUS_CONNECTED]  = { "connected",  0x0001,   0, 255,   0 },  /* single heartbeat */
    [APP_LED_STATUS_UPDATING]   = { "updating",   0x5555,   0, 255, 255 },  /* fast strobe */
    [APP_LED_STATUS_ERROR]      = { "error",      0x0055, 255,   0,   0 },  /* four fast, then pause */
};

/* All mutable state lives behind this mutex; the pattern task and the HTTP handlers
 * are the two writers and they run on different tasks. */
static SemaphoreHandle_t s_lock;
static app_led_status_t  s_status = APP_LED_STATUS_BOOT;
static bool s_manual_active;
static bool s_manual_on;
static bool s_physical_on;
static bool s_ready;
static uint8_t s_slot;

/* Drive the LED to whatever the current state says. Call with s_lock held: the pattern
 * task and the HTTP handlers both reach the backend, and led_strip is not thread-safe.
 * Writing inside the lock also means s_physical_on can never disagree with the hardware,
 * which is what /api/led reports back. */
static void apply_locked(void)
{
    const led_state_desc_t *desc = &STATES[s_status];
    bool on;
    uint8_t r, g, b;

    if (s_manual_active) {
        /* The override holds the LED steady and, deliberately, in a neutral colour:
         * a manually-lit LED should not be mistakable for a status pattern. */
        on = s_manual_on;
        r = g = b = 255;
    } else {
        on = (desc->pattern >> s_slot) & 1;
        r = desc->r; g = desc->g; b = desc->b;
    }

    s_physical_on = on;
    led_backend_write(on, r, g, b);
}

static void led_task(void *arg)
{
    (void)arg;

    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        apply_locked();
        s_slot = (s_slot + 1) % PATTERN_SLOTS;
        xSemaphoreGive(s_lock);

        vTaskDelay(pdMS_TO_TICKS(APP_LED_TICK_MS));
    }
}

esp_err_t app_led_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = led_backend_init();
    if (err != ESP_OK) {
        /* A broken or absent LED must not stop the device booting -- it is a status
         * indicator, not a function. Say so loudly and carry on. */
        ESP_LOGW(TAG, "no status LED: %s", esp_err_to_name(err));
    }

    if (!led_backend_present()) {
        ESP_LOGI(TAG, "no status LED configured for this target");
        s_ready = true;
        return ESP_OK;
    }

    if (xTaskCreate(led_task, "app_led", CONFIG_APP_LED_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "status LED on GPIO%d", CONFIG_APP_LED_GPIO);
    return ESP_OK;
}

void app_led_set_status(app_led_status_t status)
{
    if (status >= APP_LED_STATUS_MAX || !s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = (s_status != status);
    s_status = status;
    xSemaphoreGive(s_lock);
    if (changed) {
        ESP_LOGD(TAG, "status -> %s", STATES[status].name);
    }
}

app_led_status_t app_led_get_status(void)
{
    if (!s_lock) {
        return s_status;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    app_led_status_t s = s_status;
    xSemaphoreGive(s_lock);
    return s;
}

const char *app_led_status_name(app_led_status_t status)
{
    if (status >= APP_LED_STATUS_MAX) {
        return "unknown";
    }
    return STATES[status].name;
}

void app_led_manual_set(bool on)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_manual_active = true;
    s_manual_on = on;
    /* Apply now rather than on the next tick. Otherwise the caller reads back the old
     * state -- /api/led would report the LED it is about to set, not the one it set. */
    apply_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "manual override: %s", on ? "on" : "off");
}

bool app_led_manual_toggle(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Toggle against the override, not against the pin. While a pattern is running the
     * pin's instantaneous state is arbitrary -- sampling it means the first press does
     * something different depending on where in the blink it landed, which is exactly
     * how a working button comes to look broken. Taking the LED always lights it. */
    bool next = s_manual_active ? !s_manual_on : true;
    s_manual_active = true;
    s_manual_on = next;
    apply_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "manual override: %s", next ? "on" : "off");
    return next;
}

void app_led_manual_release(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_manual_active = false;
    apply_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "manual override released");
}

bool app_led_manual_active(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool a = s_manual_active;
    xSemaphoreGive(s_lock);
    return a;
}

bool app_led_is_on(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_physical_on;
    xSemaphoreGive(s_lock);
    return on;
}

bool app_led_present(void) { return led_backend_present(); }
