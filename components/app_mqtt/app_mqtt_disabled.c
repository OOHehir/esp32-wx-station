/* CONFIG_APP_MQTT_ENABLE=n, which is the default. The API still links so app_core needs
 * no #ifdefs and nothing else in the template has to know whether a broker exists. */
#include "app_mqtt.h"

esp_err_t app_mqtt_init(void)        { return ESP_OK; }
bool app_mqtt_connected(void)        { return false; }
void app_mqtt_publish_status(void)   { }
