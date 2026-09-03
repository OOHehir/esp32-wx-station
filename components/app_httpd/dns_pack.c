#include "dns_pack.h"

#include <string.h>

/* Local byte-order helpers rather than <arpa/inet.h> or lwIP's: this file has to build
 * both on the device and in the host test, and the wire format is fixed anyway. */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline void     wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

#define OFF_FLAGS    2
#define OFF_QDCOUNT  4
#define OFF_ANCOUNT  6
#define OFF_NSCOUNT  8
#define OFF_ARCOUNT 10

#define QTYPE_A 1

int dns_build_reply(const uint8_t *query, size_t query_len,
                    uint8_t *out, size_t out_len, uint32_t ip_be)
{
    if (!query || !out || query_len < DNS_HEADER_LEN || query_len > out_len) {
        return -1;
    }
    /* A response arriving on port 53 is not a question; ignore it rather than reply. */
    if (query[OFF_FLAGS] & 0x80) {
        return -1;
    }
    /* Nobody sends multi-question queries. Do not pretend to handle them. */
    if (rd16(query + OFF_QDCOUNT) != 1) {
        return -1;
    }

    /* Walk the QNAME labels to find where the question ends. */
    size_t p = DNS_HEADER_LEN;
    while (p < query_len && query[p] != 0) {
        if ((query[p] & 0xc0) != 0) {
            return -1;   /* a compression pointer in a question is malformed */
        }
        p += (size_t)query[p] + 1;
    }
    if (p >= query_len) {
        return -1;       /* ran off the end without finding the root label */
    }
    p += 1;              /* the root label itself */
    if (p + 4 > query_len) {
        return -1;       /* no room for QTYPE + QCLASS */
    }
    uint16_t qtype = rd16(query + p);
    p += 4;

    memcpy(out, query, p);
    wr16(out + OFF_FLAGS, 0x8180);   /* response, recursion available, no error */
    wr16(out + OFF_ANCOUNT, 0);
    wr16(out + OFF_NSCOUNT, 0);
    wr16(out + OFF_ARCOUNT, 0);

    if (qtype != QTYPE_A) {
        return (int)p;
    }

    static const uint8_t answer_head[] = {
        0xc0, 0x0c,             /* name: pointer back to the question */
        0x00, 0x01,             /* type A */
        0x00, 0x01,             /* class IN */
        0x00, 0x00, 0x00, 0x3c, /* TTL 60s -- short, because this answer is a lie that
                                 * should expire quickly once the device leaves AP mode */
        0x00, 0x04,             /* RDLENGTH */
    };
    if (p + sizeof(answer_head) + 4 > out_len) {
        return -1;
    }
    memcpy(out + p, answer_head, sizeof(answer_head));
    p += sizeof(answer_head);
    memcpy(out + p, &ip_be, 4);
    p += 4;

    wr16(out + OFF_ANCOUNT, 1);
    return (int)p;
}
