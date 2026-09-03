#include "rollback_policy.h"

#include <string.h>

static bool empty(const char *s) { return s == NULL || s[0] == '\0'; }

const char *rollback_boot_name(rollback_boot_t b)
{
    switch (b) {
    case ROLLBACK_BOOT_NORMAL:    return "normal";
    case ROLLBACK_BOOT_PROBATION: return "probation";
    case ROLLBACK_BOOT_REVERTED:  return "reverted";
    case ROLLBACK_BOOT_CONFIRMED: return "confirmed";
    }
    return "unknown";
}

rollback_boot_t rollback_on_boot(const char *attempted, const char *running, bool pending)
{
    /* Probation is decided by the bootloader, not by our record, so it is read first.
     * A device flashed over serial into a slot the bootloader marked pending has no
     * attempted-version record at all, and still needs its self-test to run. */
    if (pending) {
        return ROLLBACK_BOOT_PROBATION;
    }
    if (empty(attempted) || empty(running)) {
        return ROLLBACK_BOOT_NORMAL;
    }
    /* An outstanding attempt, not pending, and what is running is not it: the bootloader
     * has already put the old image back. */
    if (strcmp(attempted, running) != 0) {
        return ROLLBACK_BOOT_REVERTED;
    }
    /* Running the version we installed, and no longer on probation -- a previous boot's
     * self-test already passed. Nothing to do but tidy the record away. */
    return ROLLBACK_BOOT_CONFIRMED;
}

unsigned rollback_next_count(const char *record, unsigned count, const char *failed)
{
    if (empty(failed)) {
        return 0;
    }
    if (empty(record) || strcmp(record, failed) != 0) {
        return 1;
    }
    /* Saturate rather than wrap. A counter that rolls over to zero would silently lift
     * the quarantine on the one image that has failed most. */
    return (count >= (unsigned)-1) ? count : count + 1;
}
