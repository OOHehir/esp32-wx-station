#include "json_util.h"
#include "test.h"

void test_json_util(void)
{
    char out[64];

    SUITE("json_escape");

    json_escape("plain", out, sizeof(out));
    CHECK_STR(out, "plain");

    /* The case that matters: an SSID is whatever the neighbours named their router, and
     * an unescaped quote turns /api/status into invalid JSON and the page into a blank. */
    json_escape("Bob's \"Wi-Fi\"", out, sizeof(out));
    CHECK_STR(out, "Bob's \\\"Wi-Fi\\\"");

    json_escape("back\\slash", out, sizeof(out));
    CHECK_STR(out, "back\\\\slash");

    json_escape("a\nb\tc\rd", out, sizeof(out));
    CHECK_STR(out, "a\\nb\\tc\\rd");

    /* Control characters have no literal JSON form. */
    json_escape("bell\x07here", out, sizeof(out));
    CHECK_STR(out, "bell\\u0007here");

    /* UTF-8 passes through untouched -- JSON strings are UTF-8, and mangling a non-ASCII
     * SSID into escapes would be a regression, not a safety measure. */
    json_escape("caf\xc3\xa9", out, sizeof(out));
    CHECK_STR(out, "caf\xc3\xa9");

    json_escape("", out, sizeof(out));
    CHECK_STR(out, "");

    json_escape(NULL, out, sizeof(out));
    CHECK_STR(out, "");

    SUITE("json_escape truncation");

    /* Truncation must never cut an escape in half: "abc\ would be invalid JSON, and the
     * consumer of that is a browser that shows nothing at all. */
    char tiny[8];
    json_escape("abcdefghijkl", tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny), "wrote past the buffer");
    CHECK_STR(tiny, "ab");

    char small[10];
    json_escape("\"\"\"\"\"\"\"\"", small, sizeof(small));
    CHECK(strlen(small) < sizeof(small), "wrote past the buffer");
    CHECK(strlen(small) % 2 == 0, "truncated inside a \\\" escape: \"%s\"", small);

    char ctl[10];
    json_escape("\x01\x02\x03\x04", ctl, sizeof(ctl));
    CHECK(strlen(ctl) < sizeof(ctl), "wrote past the buffer");
    CHECK(strlen(ctl) % 6 == 0, "truncated inside a \\uXXXX escape: \"%s\"", ctl);

    /* A one-byte buffer can hold nothing but the terminator. */
    char one[1] = { 'x' };
    json_escape("hello", one, sizeof(one));
    CHECK_INT(one[0], 0);

    /* Zero length must write nothing at all rather than a stray terminator. */
    char guard[2] = { 'A', 'B' };
    json_escape("hello", guard, 0);
    CHECK_INT(guard[0], 'A');
}
