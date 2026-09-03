/* app_ota -- pull a new firmware image from the NAS over TLS, prove it works, and keep
 * it. Or put the old one back.
 *
 * PLAN §4. The shape of this is deliberate:
 *
 *   Manifest first. The device fetches a small JSON document and compares versions
 *   before downloading a single byte of image. A 900 KB image over marginal Wi-Fi is not
 *   something to transfer speculatively every six hours.
 *
 *   Advanced API, not the one-shot esp_https_ota() call. Beginning the download gives
 *   access to the new image's header, which is checked before any of it is committed --
 *   flashing an image built for a different project is a brick, and it is cheap to
 *   refuse. The same access is what allows the manifest's sha256 to be checked against
 *   the written slot before the slot is ever marked bootable.
 *
 *   Nothing blocks an HTTP handler. app_ota owns a task; the web UI queues work and
 *   polls for progress (PLAN §4a).
 *
 *   A new image is on probation. It boots in ESP_OTA_IMG_PENDING_VERIFY and is confirmed
 *   only after a self-test proves the device is still reachable and still updatable. If
 *   the self-test fails -- or the image never gets far enough to run one -- the
 *   bootloader puts the previous image back. This is what makes a bad build recoverable
 *   without a cable, and it is the reason the rest exists.
 *
 * Four independent things have to agree before a new image runs for good: the manifest
 * says it is for this project and chip, the image header agrees with the manifest, the
 * bytes in flash hash to what the manifest published, and the image carries a valid
 * signature (when signing is enabled). Then it still has to pass its self-test.
 *
 * The trust anchor is the private CA embedded at build time, not the leaf certificate,
 * so the NAS certificate can be re-issued without reflashing (PLAN §3).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(APP_OTA_EVENT);

typedef enum {
    APP_OTA_EVENT_STATE_CHANGED,
    APP_OTA_EVENT_PROGRESS,
} app_ota_event_id_t;

typedef enum {
    APP_OTA_IDLE = 0,
    APP_OTA_SELFTEST,      /* on probation: proving this image before confirming it */
    APP_OTA_CHECKING,      /* fetching and parsing the manifest */
    APP_OTA_UP_TO_DATE,    /* checked, nothing to do */
    APP_OTA_DOWNLOADING,   /* writing an image into the passive slot */
    APP_OTA_READY,         /* written and verified; restarting */
    APP_OTA_FAILED,
} app_ota_state_t;

typedef struct {
    app_ota_state_t state;
    char     running_version[48];
    char     offered_version[48];   /* what the manifest advertises, when known */
    int      progress_pct;          /* -1 when the server did not give a length */
    uint32_t bytes_written;
    uint32_t bytes_total;
    char     message[112];          /* human-readable reason, especially on failure */
    int64_t  last_check_s;          /* uptime seconds at the last completed check, -1 = never */

    /* Rollback (PLAN §5). `probation` mirrors ESP_OTA_IMG_PENDING_VERIFY: the bootloader
     * will revert on the next reset unless the self-test confirms this image. */
    bool     probation;
    int32_t  selftest_left_s;       /* seconds left before the self-test gives up, -1 when idle */

    /* The one version known to have failed here. Automatic polls skip it once the count
     * reaches CONFIG_APP_OTA_MAX_FAILURES; a manual check still retries it. */
    char     failed_version[48];
    uint32_t failed_count;
} app_ota_info_t;

/* Starts the OTA task and registers the hook table with app_httpd, which is what makes
 * the web UI's "Check for updates" button live. Safe to call with no network up.
 *
 * This is also where the previous boot is accounted for: an image that was installed and
 * did not survive gets its failure counted here, before anything else runs. */
esp_err_t app_ota_init(void);

/* Queue a check. Returns immediately -- watch app_ota_get_info() for the outcome.
 * Returns ESP_ERR_INVALID_STATE if an update is already in flight or the running image
 * is still on probation.
 *
 * `forced` means a person asked, and overrides the quarantine on a version that has
 * already failed here -- a version broken by a bad upload is otherwise a dead end. */
esp_err_t app_ota_check_now(bool forced);

void app_ota_get_info(app_ota_info_t *out);

/* True while an image is being written. app_httpd refuses a restart on this. */
bool app_ota_busy(void);

const char *app_ota_state_name(app_ota_state_t state);

#ifdef __cplusplus
}
#endif
