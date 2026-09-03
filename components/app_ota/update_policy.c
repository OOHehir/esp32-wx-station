#include "update_policy.h"

#include <stdio.h>
#include <string.h>

static bool empty(const char *s) { return s == NULL || s[0] == '\0'; }

/* Local rather than strncasecmp(): that lives in <strings.h> and is POSIX, and this file
 * is deliberately dependency-free so the host tests can compile it as strict C11. */
static bool starts_with_ci(const char *s, const char *prefix)
{
    for (size_t i = 0; prefix[i]; i++) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') { a = (char)(a - 'A' + 'a'); }
        if (b >= 'A' && b <= 'Z') { b = (char)(b - 'A' + 'a'); }
        if (a != b) {
            return false;
        }
    }
    return true;
}

/* vX.Y.Z into three numbers, or false.
 *
 * Strict on purpose: an optional leading `v`, exactly three runs of decimal digits
 * separated by dots, and nothing whatsoever after the third. Every other shape `git
 * describe` emits -- `v0.2.0-3-gab18cdd`, `v0.2.0-dirty`, `7b29c21`, `0.0.0-nogit` --
 * fails here, which is the point: they are not orderable, and a policy that guessed at
 * them would sometimes guess a downgrade and call it an upgrade.
 *
 * Nine digits per component is not a limit anybody will meet; it is what keeps the
 * accumulator from overflowing on a version string invented to make it. */
static bool parse_semver(const char *s, unsigned long out[3])
{
    if (s == NULL) {
        return false;
    }
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        unsigned long v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 9) {
                return false;
            }
            v = v * 10 + (unsigned long)(*s - '0');
            s++;
        }
        out[i] = v;
        if (i < 2) {
            if (*s != '.') {
                return false;
            }
            s++;
        }
    }
    return *s == '\0';
}

/* -1, 0 or 1, comparing major then minor then patch. */
static int semver_cmp(const unsigned long a[3], const unsigned long b[3])
{
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

const char *update_verdict_name(update_verdict_t v)
{
    switch (v) {
    case UPDATE_APPLY:         return "apply";
    case UPDATE_UP_TO_DATE:    return "up_to_date";
    case UPDATE_WRONG_PROJECT: return "wrong_project";
    case UPDATE_WRONG_CHIP:    return "wrong_chip";
    case UPDATE_INSECURE_URL:  return "insecure_url";
    case UPDATE_MALFORMED:     return "malformed";
    case UPDATE_QUARANTINED:   return "quarantined";
    case UPDATE_NOT_NEWER:     return "not_newer";
    case UPDATE_UNORDERABLE:   return "unorderable";
    }
    return "unknown";
}

update_verdict_t update_decide(const update_offer_t *offer, const update_device_t *dev,
                               char *reason, size_t reason_len)
{
    /* Write something into `reason` on every path, so a caller that only logs it can
     * never print an uninitialised buffer. */
    if (reason && reason_len) {
        reason[0] = '\0';
    }
#define SAY(...) do { if (reason && reason_len) snprintf(reason, reason_len, __VA_ARGS__); } while (0)

    if (!offer || !dev ||
        empty(offer->project) || empty(offer->chip) || empty(offer->version) ||
        empty(offer->url) ||
        empty(dev->project) || empty(dev->chip) || empty(dev->version)) {
        SAY("manifest is missing a required field");
        return UPDATE_MALFORMED;
    }

    /* Flashing an image built for another project bricks the device, and one NAS feeds a
     * mixed fleet from one directory tree -- a publish into the wrong path is an ordinary
     * mistake, not an attack. Check before trusting the layout. */
    if (strcmp(offer->project, dev->project) != 0) {
        SAY("manifest is for project \"%s\", this is \"%s\"", offer->project, dev->project);
        return UPDATE_WRONG_PROJECT;
    }
    if (strcmp(offer->chip, dev->chip) != 0) {
        SAY("manifest is for %s, this is %s", offer->chip, dev->chip);
        return UPDATE_WRONG_CHIP;
    }

    /* The manifest chooses the URL, so the manifest could quietly move the download to
     * plain HTTP. Refuse: the CA pinning that makes this whole design safe only applies
     * to a TLS connection. Case-insensitive, because a scheme is. */
    if (!starts_with_ci(offer->url, "https://")) {
        SAY("manifest url is not https");
        return UPDATE_INSECURE_URL;
    }

    /* Identical strings first, before any parsing. Two versions that are the same string
     * are the same build whether or not either is orderable, and a device running an
     * untagged image should say "up to date" rather than "I cannot compare these". */
    if (strcmp(offer->version, dev->version) == 0) {
        SAY("already running %s", dev->version);
        return UPDATE_UP_TO_DATE;
    }

    /* Then: strictly newer, or nothing.
     *
     * An automatic poll installs an image only when it can prove the offer sorts above
     * what is running. That is what lets a release be published and left alone -- a stale
     * or mistaken artefact on the server cannot walk a fleet backwards months later, and
     * nobody has to remember to go and delete the old one.
     *
     * A forced check is a person asking, and gets the same latitude here that it already
     * has over the quarantine: without it, a device flashed with an untagged build would
     * be reachable only by cable. */
    /* Set by any branch that has already written a more specific reason than the plain
     * "old -> new" below. Without it, the last SAY wins and every "installing anyway,
     * you asked" is overwritten before the caller ever reads it -- which was already
     * true of the forced-quarantine message, and is exactly the line somebody needs when
     * they are asking why a fleet moved backwards. */
    bool noted = false;

    unsigned long mine[3], theirs[3];
    const bool mine_ok = parse_semver(dev->version, mine);
    const bool theirs_ok = parse_semver(offer->version, theirs);

    if (!mine_ok || !theirs_ok) {
        if (!dev->forced) {
            /* Name the unorderable side. "cannot compare" sends somebody to read both
             * strings; naming the one at fault says what to do about it. */
            if (!mine_ok) {
                SAY("this device runs %s, which is not vX.Y.Z -- reflash to update",
                    dev->version);
            } else {
                SAY("manifest offers %s, which is not vX.Y.Z", offer->version);
            }
            return UPDATE_UNORDERABLE;
        }
        SAY("%s -> %s, unorderable -- installing anyway, you asked",
            dev->version, offer->version);
        noted = true;
    } else {
        const int order = semver_cmp(theirs, mine);

        /* The same release spelled differently -- "0.2.0" against "v0.2.0" -- is not an
         * update, and the string comparison above cannot see that. Reported as up to
         * date rather than as a refusal, because nothing is wrong: a forced check gets
         * the same answer, since reinstalling the version already running achieves
         * nothing but a panel refresh and a reboot. */
        if (order == 0) {
            SAY("already running %s", dev->version);
            return UPDATE_UP_TO_DATE;
        }
        if (order < 0) {
            if (!dev->forced) {
                SAY("%s is not newer than %s", offer->version, dev->version);
                return UPDATE_NOT_NEWER;
            }
            SAY("%s -> %s is a downgrade -- installing anyway, you asked",
                dev->version, offer->version);
            noted = true;
        }
    }

    /* Checked last, and only against a version we are not already running: a device that
     * somehow ended up running a quarantined build should report "up to date", not
     * accuse itself.
     *
     * Without this the poll is a loop -- download 900 KB, boot, fail the self-test, roll
     * back, and do it all again six hours later, for as long as the broken image stays
     * published. Two attempts is enough to distinguish a bad image from a bad moment. */
    if (dev->max_failures > 0 && !empty(dev->failed_version) &&
        strcmp(offer->version, dev->failed_version) == 0 &&
        dev->failed_count >= dev->max_failures) {
        if (dev->forced) {
            SAY("%s failed %u times -- retrying anyway, you asked",
                offer->version, dev->failed_count);
            noted = true;
        } else {
            SAY("%s failed to run here %u times -- not retrying automatically",
                offer->version, dev->failed_count);
            return UPDATE_QUARANTINED;
        }
    }

    if (!noted) {
        SAY("%s -> %s", dev->version, offer->version);
    }
    return UPDATE_APPLY;
#undef SAY
}
