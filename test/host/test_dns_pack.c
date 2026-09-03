#include "dns_pack.h"
#include "test.h"

#include <stdint.h>
#include <string.h>

/* 192.168.4.1 in network byte order, which is what the AP netif hands out. */
static const uint8_t AP_IP_BYTES[4] = { 192, 168, 4, 1 };
static uint32_t ap_ip(void)
{
    uint32_t v;
    memcpy(&v, AP_IP_BYTES, 4);
    return v;
}

/* Build "captive.example" as a query for `qtype`. */
static size_t make_query(uint8_t *buf, uint16_t qtype)
{
    static const uint8_t head[] = {
        0xab, 0xcd,             /* id */
        0x01, 0x00,             /* standard query, recursion desired */
        0x00, 0x01,             /* qdcount 1 */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        7, 'c','a','p','t','i','v','e',
        7, 'e','x','a','m','p','l','e',
        0,
    };
    memcpy(buf, head, sizeof(head));
    size_t n = sizeof(head);
    buf[n++] = (uint8_t)(qtype >> 8);
    buf[n++] = (uint8_t)qtype;
    buf[n++] = 0x00;            /* class IN */
    buf[n++] = 0x01;
    return n;
}

void test_dns_pack(void)
{
    uint8_t q[128], r[512];

    SUITE("dns_build_reply -- A query");

    size_t qlen = make_query(q, 1);
    int n = dns_build_reply(q, qlen, r, sizeof(r), ap_ip());

    CHECK_INT(n, (int)qlen + 16);                  /* question echoed + 16-byte answer */
    CHECK(memcmp(r, q, 2) == 0, "transaction id must be echoed");
    CHECK_INT(r[2], 0x81);                         /* QR + RD */
    CHECK_INT(r[3], 0x80);                         /* RA, rcode 0 */
    CHECK_INT((r[4] << 8) | r[5], 1);              /* qdcount */
    CHECK_INT((r[6] << 8) | r[7], 1);              /* ancount */
    CHECK_INT((r[8] << 8) | r[9], 0);              /* nscount */
    CHECK_INT((r[10] << 8) | r[11], 0);            /* arcount */
    CHECK(memcmp(r + DNS_HEADER_LEN, q + DNS_HEADER_LEN, qlen - DNS_HEADER_LEN) == 0,
          "the question section must be echoed verbatim");

    const uint8_t *ans = r + qlen;
    CHECK_INT(ans[0], 0xc0);                       /* compression pointer... */
    CHECK_INT(ans[1], 0x0c);                       /* ...to the question at offset 12 */
    CHECK_INT((ans[2] << 8) | ans[3], 1);          /* type A */
    CHECK_INT((ans[4] << 8) | ans[5], 1);          /* class IN */
    CHECK_INT(ans[9], 60);                         /* TTL: short, the answer is a lie */
    CHECK_INT((ans[10] << 8) | ans[11], 4);        /* rdlength */
    CHECK(memcmp(ans + 12, AP_IP_BYTES, 4) == 0, "the answer must be the AP address");

    SUITE("dns_build_reply -- non-A queries");

    /* A AAAA query answered with an A record is a malformed reply, and phones notice.
     * The correct answer is a well-formed empty NOERROR. */
    qlen = make_query(q, 28);                      /* AAAA */
    n = dns_build_reply(q, qlen, r, sizeof(r), ap_ip());
    CHECK_INT(n, (int)qlen);
    CHECK_INT((r[6] << 8) | r[7], 0);              /* ancount 0 */
    CHECK_INT(r[3] & 0x0f, 0);                     /* rcode NOERROR, not NXDOMAIN */

    qlen = make_query(q, 65);                      /* HTTPS/SVCB, which iOS does send */
    n = dns_build_reply(q, qlen, r, sizeof(r), ap_ip());
    CHECK_INT(n, (int)qlen);
    CHECK_INT((r[6] << 8) | r[7], 0);

    SUITE("dns_build_reply -- malformed input is rejected");

    qlen = make_query(q, 1);

    CHECK_INT(dns_build_reply(q, 5, r, sizeof(r), ap_ip()), -1);        /* short header */
    CHECK_INT(dns_build_reply(NULL, qlen, r, sizeof(r), ap_ip()), -1);
    CHECK_INT(dns_build_reply(q, qlen, NULL, sizeof(r), ap_ip()), -1);

    /* A reply arriving on port 53 must not be answered -- that is how a responder ends
     * up in a loop with another one. */
    uint8_t resp[128];
    size_t rlen = make_query(resp, 1);
    resp[2] |= 0x80;
    CHECK_INT(dns_build_reply(resp, rlen, r, sizeof(r), ap_ip()), -1);

    /* qdcount != 1 */
    uint8_t two[128];
    size_t tlen = make_query(two, 1);
    two[5] = 2;
    CHECK_INT(dns_build_reply(two, tlen, r, sizeof(r), ap_ip()), -1);

    /* A compression pointer inside a question is malformed, and following it is how a
     * parser gets walked off the end of the buffer.
     *
     * The packet is padded on purpose. A short one is rejected anyway, because 0xc0 read
     * as a label length runs off the end -- which means a short packet does not actually
     * test the pointer check at all. Here the naive "length 192" reading lands back
     * inside the buffer on a zero byte, so the only thing that can reject this is the
     * pointer check itself. */
    uint8_t ptr[256];
    memset(ptr, 'x', sizeof(ptr));
    memcpy(ptr, q, DNS_HEADER_LEN);
    ptr[DNS_HEADER_LEN] = 0xc0;
    ptr[DNS_HEADER_LEN + 1] = 0x0c;
    ptr[DNS_HEADER_LEN + 0xc0 + 1] = 0;    /* where a length-192 label would end */
    CHECK_INT(dns_build_reply(ptr, sizeof(ptr), r, sizeof(r), ap_ip()), -1);

    /* A label length that runs past the end of the packet. */
    uint8_t run[128];
    size_t runlen = make_query(run, 1);
    run[DNS_HEADER_LEN] = 200;
    CHECK_INT(dns_build_reply(run, runlen, r, sizeof(r), ap_ip()), -1);

    /* A name that never terminates. */
    uint8_t noroot[DNS_HEADER_LEN + 4];
    memcpy(noroot, q, sizeof(noroot));
    noroot[DNS_HEADER_LEN] = 3;
    CHECK_INT(dns_build_reply(noroot, sizeof(noroot), r, sizeof(r), ap_ip()), -1);

    /* Truncated before QTYPE/QCLASS. */
    uint8_t cut[128];
    size_t clen = make_query(cut, 1);
    CHECK_INT(dns_build_reply(cut, clen - 3, r, sizeof(r), ap_ip()), -1);

    SUITE("dns_build_reply -- output buffer bounds");

    qlen = make_query(q, 1);
    /* Room for the echoed question but not the answer. */
    CHECK_INT(dns_build_reply(q, qlen, r, qlen + 8, ap_ip()), -1);
    /* Not even room for the question. */
    CHECK_INT(dns_build_reply(q, qlen, r, qlen - 1, ap_ip()), -1);
    /* Exactly enough. */
    CHECK_INT(dns_build_reply(q, qlen, r, qlen + 16, ap_ip()), (int)qlen + 16);
}
