/* app_core -- bring-up order and the glue between components.
 *
 * main/ holds project-specific code only (PLAN §4), so the startup sequence lives here
 * instead: it is template behaviour, identical in every project built from this repo,
 * and app_main() should not have to be edited to reorder it.
 *
 * This is also the one place that knows how components relate to each other -- that a
 * network state change should change what the LED says, for instance. Keeping that
 * policy here is what lets app_net and app_led stay unaware of one another.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up NVS, the default event loop, esp_netif, and every app_* component in
 * dependency order, then returns. Long-running work happens on the components' own
 * tasks, so the caller's task is free to exit. */
esp_err_t app_core_start(void);

#ifdef __cplusplus
}
#endif
