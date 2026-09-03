/* No status LED configured. The template must build and run cleanly like this
 * (PLAN §1a) -- a board without an LED is a supported configuration, not a mistake. */
#include "led_backend.h"

esp_err_t led_backend_init(void) { return ESP_OK; }
void led_backend_write(bool on, uint8_t r, uint8_t g, uint8_t b)
{
    (void)on; (void)r; (void)g; (void)b;
}
bool led_backend_present(void) { return false; }
