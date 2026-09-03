/* Minimal DNS reply construction for the captive portal.
 *
 * Deliberately free of ESP-IDF and lwIP so it can be compiled and tested on the host
 * (see test/host/). This is packet parsing driven by bytes off the network: the failure
 * mode of a bug here is a phone that silently never shows the setup page, which is
 * exactly the kind of thing that is miserable to debug on hardware and trivial to
 * catch with a test.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Build a reply to `query` that answers any A query with `ip_be` (4 bytes in network
 * order, as in_addr_t holds them).
 *
 * Returns the reply length, or -1 if the query is malformed or does not fit. A query
 * for anything other than an A record gets a well-formed empty NOERROR answer, not an
 * A record: answering a AAAA query with an A record is a malformed reply, and phones
 * notice.
 */
int dns_build_reply(const uint8_t *query, size_t query_len,
                    uint8_t *out, size_t out_len, uint32_t ip_be);

/* Exposed for tests. */
#define DNS_HEADER_LEN 12
