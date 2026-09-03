#include "test.h"
#include "rollback_policy.h"

#define OLD "v0.2.0"
#define NEW "v0.3.0"

void test_rollback_policy(void)
{
    SUITE("rollback_on_boot -- an ordinary boot");

    /* Nothing outstanding, nothing pending: the overwhelmingly common case, and it must
     * cost the device nothing and record nothing. */
    CHECK_INT(rollback_on_boot(NULL, OLD, false), ROLLBACK_BOOT_NORMAL);
    CHECK_INT(rollback_on_boot("",   OLD, false), ROLLBACK_BOOT_NORMAL);

    SUITE("rollback_on_boot -- the first boot of a new image");

    /* Installed and now running, still on probation: the self-test decides. */
    CHECK_INT(rollback_on_boot(NEW, NEW, true), ROLLBACK_BOOT_PROBATION);

    /* Probation is the bootloader's verdict, not ours. A device flashed over serial into
     * a slot the bootloader left pending has no record at all and still needs its
     * self-test to run -- treating that as "nothing to do" would leave an image that
     * silently reverts on the next reset. */
    CHECK_INT(rollback_on_boot(NULL, NEW, true), ROLLBACK_BOOT_PROBATION);
    CHECK_INT(rollback_on_boot("",   NEW, true), ROLLBACK_BOOT_PROBATION);

    SUITE("rollback_on_boot -- it did not survive");

    /* We installed NEW and OLD is running: the bootloader has already put it back. This
     * covers both ways it can happen -- the self-test rejected it, or it panicked before
     * one could run -- because the evidence is the same either way. */
    CHECK_INT(rollback_on_boot(NEW, OLD, false), ROLLBACK_BOOT_REVERTED);

    SUITE("rollback_on_boot -- it survived an earlier boot");

    /* Running what we installed, no longer pending: some earlier boot's self-test passed.
     * Nothing to do but tidy the record away -- counting a failure here would quarantine
     * a version that is demonstrably fine, and returning PROBATION would re-run a
     * self-test that can no longer confirm anything. */
    CHECK_INT(rollback_on_boot(NEW, NEW, false), ROLLBACK_BOOT_CONFIRMED);

    SUITE("rollback_on_boot -- degenerate inputs");

    /* No running version to compare against: say nothing rather than guess. */
    CHECK_INT(rollback_on_boot(NEW, NULL, false), ROLLBACK_BOOT_NORMAL);
    CHECK_INT(rollback_on_boot(NEW, "",   false), ROLLBACK_BOOT_NORMAL);
    /* ... but a pending image is pending regardless of what we can name. */
    CHECK_INT(rollback_on_boot(NEW, NULL, true), ROLLBACK_BOOT_PROBATION);

    rollback_boot_t all[] = { ROLLBACK_BOOT_NORMAL, ROLLBACK_BOOT_PROBATION,
                              ROLLBACK_BOOT_REVERTED, ROLLBACK_BOOT_CONFIRMED };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        CHECK(rollback_boot_name(all[i])[0] != '\0', "boot %d has no name", (int)all[i]);
    }

    SUITE("rollback_next_count -- the failure tally");

    /* Nothing recorded yet: the first failure is failure one, not failure zero. A tally
     * that starts at 0 never reaches a threshold of 2 in two attempts. */
    CHECK_INT(rollback_next_count(NULL, 0, NEW), 1);
    CHECK_INT(rollback_next_count("",   0, NEW), 1);

    CHECK_INT(rollback_next_count(NEW, 1, NEW), 2);
    CHECK_INT(rollback_next_count(NEW, 2, NEW), 3);

    /* A different version resets the tally. The single record slot is reused, and the
     * count means "this version failed n times" -- inheriting someone else's total would
     * quarantine a fresh version on its first attempt. */
    CHECK_INT(rollback_next_count(OLD, 5, NEW), 1);
    CHECK_INT(rollback_next_count("v0.3.0-1-gdeadbee", 5, NEW), 1);

    /* Nothing to attribute the failure to. */
    CHECK_INT(rollback_next_count(NEW, 1, NULL), 0);
    CHECK_INT(rollback_next_count(NEW, 1, ""),   0);

    /* Saturate rather than wrap: a counter that rolls over to zero would silently lift
     * the quarantine on the one image that has failed most. */
    CHECK_INT(rollback_next_count(NEW, (unsigned)-1, NEW), (long)(unsigned)-1);
}
