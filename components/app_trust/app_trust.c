#include "app_trust.h"

#include "sdkconfig.h"

#if CONFIG_APP_TRUST_PRIVATE_CA
extern const char ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
#endif

/* The bundle header is included whenever the bundle is compiled in, which is no longer
 * only in the CERT_BUNDLE build: the weather client selects MBEDTLS_CERTIFICATE_BUNDLE
 * for its own anchor while the OTA anchor stays a private CA (PLAN 1a). */
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

const char *app_trust_ca_pem(void)
{
#if CONFIG_APP_TRUST_PRIVATE_CA
    return ca_cert_pem_start;
#else
    return NULL;
#endif
}

app_trust_bundle_attach_fn app_trust_bundle_attach(void)
{
#if CONFIG_APP_TRUST_PRIVATE_CA
    return NULL;
#else
    return esp_crt_bundle_attach;
#endif
}

const char *app_trust_name(void)
{
#if CONFIG_APP_TRUST_PRIVATE_CA
    return "the embedded private CA";
#else
    return "the IDF root certificate bundle";
#endif
}

app_trust_bundle_attach_fn app_trust_public_bundle_attach(void)
{
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    return esp_crt_bundle_attach;
#else
    /* No bundle in this build. Returning NULL is the honest answer and the caller is
     * required to treat it as fatal: the failure mode of the alternative is a device that
     * silently accepts any certificate on the one connection that leaves the LAN. */
    return NULL;
#endif
}

const char *app_trust_public_name(void)
{
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    return "the IDF root certificate bundle";
#else
    return "nothing -- no bundle in this build";
#endif
}
