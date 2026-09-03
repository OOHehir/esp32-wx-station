/* Project entry point.
 *
 * Everything reusable lives in components/ -- that is the actual deliverable of this
 * template (PLAN §4). Bring-up order is app_core's business, so this file stays empty
 * enough that a new project can add its own code here without first having to
 * understand, or accidentally reorder, the startup sequence.
 */
#include "app_core.h"
#include "app_btn.h"
#include "app_display.h"
#include "app_wx.h"
#include "esp_err.h"

void app_main(void)
{
    ESP_ERROR_CHECK(app_core_start());
    ESP_ERROR_CHECK(app_wx_start());
    ESP_ERROR_CHECK(app_display_start());
    ESP_ERROR_CHECK(app_btn_start());

    /* Both calls return as soon as their components' tasks are running, so returning
     * from app_main() is fine and the idle task reclaims this stack. */
}
