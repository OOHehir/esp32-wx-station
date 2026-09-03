/* Addressable (WS2812-style) status LED driven over RMT.
 *
 * Colour is genuinely useful here: it lets the status patterns in app_led.c say *which*
 * state the device is in from across a room, not just that something is blinking.
 */
#include "led_backend.h"

#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

static const char *TAG = "app_led";

static led_strip_handle_t s_strip;

esp_err_t led_backend_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_APP_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   /* 10 MHz -> 0.1 us, the WS2812 standard */
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },      /* one pixel does not warrant a DMA channel */
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip on GPIO%d: %s", CONFIG_APP_LED_GPIO, esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);
    return ESP_OK;
}

void led_backend_write(bool on, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) {
        return;
    }
    if (!on) {
        led_strip_clear(s_strip);
        return;
    }
    /* Scale to the configured brightness. Devkit WS2812s are painfully bright at full
     * scale and a status light does not need to be. */
    const uint32_t scale = CONFIG_APP_LED_BRIGHTNESS;
    led_strip_set_pixel(s_strip, 0,
                        (uint8_t)((r * scale) / 255),
                        (uint8_t)((g * scale) / 255),
                        (uint8_t)((b * scale) / 255));
    led_strip_refresh(s_strip);
}

bool led_backend_present(void) { return s_strip != NULL; }
