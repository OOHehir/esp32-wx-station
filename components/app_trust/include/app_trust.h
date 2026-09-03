/* app_trust -- what this device believes about who it is talking to.
 *
 * PLAN §3 calls the trust anchor "the actual design fork", and it is the one decision
 * here that cannot be changed after devices ship: a device trusts what its firmware says
 * it trusts. That makes it a project-level choice rather than any one component's
 * business, which is why it lives in a component of its own with nothing else in it.
 *
 * It started inside app_ota, because OTA was the only thing making TLS connections. The
 * moment a second component needed the same anchor (app_mqtt, PLAN §8) that stopped
 * being tenable -- and ESP-IDF made the point concrete by refusing to build: EMBED_TXTFILES
 * names its generated stub after the file's basename, so two components embedding the
 * same certificate collide in ninja. The fix and the correct design turned out to be the
 * same thing.
 *
 * One anchor, one place, and no way for two call sites to quietly differ about it.
 *
 * PLAN 1a amends that for this project, and is worth reading before adding a third: the
 * OTA server is a NAS on the LAN holding a certificate from a CA made here, and
 * api.open-meteo.com is on the public internet holding one from Let's Encrypt. No single
 * anchor covers both. So there are two, each named for what it is for, and neither is
 * unverified -- the alternative considered and rejected was disabling verification on the
 * weather connection, which is the one connection that actually leaves the house.
 *
 * The private anchor below is untouched by that decision, deliberately: it is the choice
 * that cannot be revisited once devices ship.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The private CA embedded at build time, NUL-terminated for mbedTLS's PEM parser.
 * NULL when this build trusts the public root bundle instead. */
const char *app_trust_ca_pem(void);

/* The bundle attach function, for config structs that take one. NULL when this build
 * pins a private CA; exactly one of the two is ever non-NULL. */
typedef esp_err_t (*app_trust_bundle_attach_fn)(void *conf);
app_trust_bundle_attach_fn app_trust_bundle_attach(void);

/* For logs and the status page -- "the embedded private CA" or "the IDF root certificate
 * bundle". Worth showing: a TLS failure looks like a network fault either way, and the
 * first useful question is which anchor the device is actually using. */
const char *app_trust_name(void);

/* --- The public anchor, for connections that leave the LAN (PLAN 1a) ---------------- */

/* The ESP-IDF root certificate bundle, for servers this project does not issue
 * certificates for. Non-NULL only in a build that has the bundle compiled in; a caller
 * that gets NULL must fail the connection rather than proceed without verification.
 *
 * Deliberately separate from app_trust_bundle_attach() above, which answers a different
 * question -- that one is "which single anchor did this build choose", and returns NULL
 * in the private-CA build that is the default here. Reusing it for the weather client
 * would make the weather connection unverifiable in exactly the configuration everything
 * else ships in. */
app_trust_bundle_attach_fn app_trust_public_bundle_attach(void);

/* Name of the public anchor, for logs. */
const char *app_trust_public_name(void);

#ifdef __cplusplus
}
#endif
