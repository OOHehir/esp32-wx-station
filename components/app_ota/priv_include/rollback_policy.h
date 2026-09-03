/* What a boot means, given what the last update tried to install.
 *
 * PLAN §4 makes rollback the whole point of the OTA design, and the bookkeeping around
 * it is exactly the kind of logic that fails silently: count a failure twice and a good
 * version gets quarantined for no reason; count it never and a device re-downloads a
 * broken image every six hours forever. So the decision is a pure function here, tested
 * on the host, and app_ota only supplies the facts.
 *
 * The facts are three:
 *
 *   attempted   the version app_ota wrote to the passive slot and rebooted into, as
 *               recorded in NVS *before* the restart -- so it survives a power cut
 *               during the very first boot of the new image
 *   running     the version actually executing now
 *   pending     whether the bootloader left this image in ESP_OTA_IMG_PENDING_VERIFY
 *
 * A reverted boot is the interesting one, and it covers both ways an image can fail:
 * the self-test rejected it, or it never got far enough to run one and the bootloader
 * reverted on its own (panic, watchdog, brownout). Either way what booted is not what
 * was installed, and that is all this needs to know.
 */
#pragma once

#include <stdbool.h>

typedef enum {
    ROLLBACK_BOOT_NORMAL = 0,   /* nothing was pending; an ordinary boot */
    ROLLBACK_BOOT_PROBATION,    /* on probation -- the self-test decides whether it stays */
    ROLLBACK_BOOT_REVERTED,     /* the installed image is not what is running: count a failure */
    ROLLBACK_BOOT_CONFIRMED,    /* the installed image is running and already valid */
} rollback_boot_t;

/* `attempted` may be NULL or empty, meaning no update is outstanding. */
rollback_boot_t rollback_on_boot(const char *attempted, const char *running, bool pending);

const char *rollback_boot_name(rollback_boot_t b);

/* The failure count to store after `failed` has just failed, given the record already
 * held (`record`/`count`). A different version resets the tally to 1 rather than
 * inheriting someone else's: the counter means "this version failed n times", and the
 * single record slot is reused. */
unsigned rollback_next_count(const char *record, unsigned count, const char *failed);
