/* The little that app_ota has to remember across a reboot.
 *
 * Three NVS keys in app_cfg's namespace, so a factory reset clears them along with
 * everything else and "what does a factory reset actually clear?" keeps a single answer:
 *
 *   ota_try    the version written to the passive slot, stored *before* the restart
 *   ota_bad    the one version known to have failed here
 *   ota_bad_n  how many times it has failed
 *
 * Writing ota_try before the restart rather than after the new image boots is the whole
 * trick: an image that panics in early startup, or a power cut during its first boot,
 * still leaves a record that the next boot can attribute the failure to.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Version strings are esp_app_desc_t::version, which is 32 bytes; the extra room costs
 * nothing and keeps a truncated read from comparing equal to a real version. */
#define OTA_RECORD_VER_MAX 48

typedef struct {
    char     attempted[OTA_RECORD_VER_MAX];  /* empty when no update is outstanding */
    char     failed[OTA_RECORD_VER_MAX];     /* empty when nothing is quarantined */
    unsigned failed_count;
} ota_record_t;

void ota_record_load(ota_record_t *out);

/* Remember that `version` is about to be booted for the first time. Call it before
 * esp_restart(), never after. */
void ota_record_set_attempted(const char *version);
void ota_record_clear_attempted(void);

/* Count a failure against `version` and drop the outstanding attempt. Returns the new
 * count. */
unsigned ota_record_note_failure(const char *version);

/* Forget a quarantine, because the version in question has now run successfully. Does
 * nothing if the record names some other version. */
void ota_record_clear_failure(const char *version);
