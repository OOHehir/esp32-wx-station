/* app_cfg -- device identity and NVS-backed configuration.
 *
 * One namespace, string and integer accessors, and a factory reset. Every other
 * component stores its persistent state through here rather than opening NVS itself,
 * so "what does a factory reset actually clear?" has a single answer.
 *
 * Identity (device id, name, SoftAP passphrase) is derived from the station MAC and is
 * therefore stable across reflashes and unique per board -- which is what makes a bench
 * full of identical devkits distinguishable on a network.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Well-known keys. NVS caps key names at 15 characters. */
#define APP_CFG_KEY_WIFI_SSID   "wifi_ssid"
#define APP_CFG_KEY_WIFI_PASS   "wifi_pass"
#define APP_CFG_KEY_BOOT_COUNT  "boot_count"
#define APP_CFG_KEY_OTA_URL     "ota_url"
/* What app_ota remembers across a reboot (PLAN §5). APP_CFG_KEY_OTA_TRY is written
 * before restarting into a new image, so a boot that never reaches our code is still
 * attributable; the OTA_BAD pair is the one quarantined version and its failure count. */
#define APP_CFG_KEY_OTA_TRY     "ota_try"
#define APP_CFG_KEY_OTA_BAD     "ota_bad"
#define APP_CFG_KEY_OTA_BAD_N   "ota_bad_n"

/* Longest device name app_cfg_device_name() can produce, including the terminator. */
#define APP_CFG_DEVICE_NAME_MAX 32
/* SoftAP passphrase length, excluding the terminator. */
#define APP_CFG_AP_PASS_LEN     12

esp_err_t app_cfg_init(void);

/* Returns ESP_ERR_NVS_NOT_FOUND when the key was never set. `out` is always
 * NUL-terminated on success. */
esp_err_t app_cfg_get_str(const char *key, char *out, size_t out_len);
esp_err_t app_cfg_set_str(const char *key, const char *value);
esp_err_t app_cfg_get_u32(const char *key, uint32_t *out);
esp_err_t app_cfg_set_u32(const char *key, uint32_t value);
esp_err_t app_cfg_erase(const char *key);
bool      app_cfg_has(const char *key);

/* Erase every key in the namespace. The caller decides whether to restart -- see
 * app_cfg_factory_reset_and_restart() for the usual behaviour. */
esp_err_t app_cfg_factory_reset(void);
void      app_cfg_factory_reset_and_restart(void);

/* "A1B2C3" -- the last three bytes of the station MAC, uppercase hex. */
const char *app_cfg_device_id(void);
/* "esp-ota-A1B2C3" -- project name plus device id. Used for the mDNS hostname and the
 * SoftAP SSID, so it must stay a valid DNS label: lowercase, no underscores. */
const char *app_cfg_device_name(void);
/* Per-device SoftAP passphrase, derived from the MAC and a build-time salt so it is
 * reproducible from the label without being guessable from the SSID. */
const char *app_cfg_ap_passphrase(void);

#ifdef __cplusplus
}
#endif
