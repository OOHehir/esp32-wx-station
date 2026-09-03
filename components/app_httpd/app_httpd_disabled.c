/* CONFIG_APP_HTTPD_ENABLE=n.
 *
 * The web UI is an unauthenticated control surface (PLAN §7), so compiling it out has to
 * be a real option. Keeping the symbols means app_core and any project code call the
 * same API either way, with no #ifdefs of their own.
 */
#include "app_httpd.h"

esp_err_t app_httpd_start(void) { return ESP_OK; }
esp_err_t app_httpd_stop(void)  { return ESP_OK; }
bool app_httpd_running(void)    { return false; }
void app_httpd_set_ota_hooks(const app_httpd_ota_hooks_t *hooks) { (void)hooks; }
