/* Plain GPIO status LED. */
#include "led_backend.h"

#include "driver/gpio.h"
#include "sdkconfig.h"

esp_err_t led_backend_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_APP_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        led_backend_write(false, 0, 0, 0);
    }
    return err;
}

void led_backend_write(bool on, uint8_t r, uint8_t g, uint8_t b)
{
    (void)r; (void)g; (void)b;   /* monochrome: colour carries no information here */
#if CONFIG_APP_LED_ACTIVE_LOW
    gpio_set_level(CONFIG_APP_LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(CONFIG_APP_LED_GPIO, on ? 1 : 0);
#endif
}

bool led_backend_present(void) { return true; }
