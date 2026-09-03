#include "test.h"
#include "sha_hex.h"

/* sha256 of the empty input -- a real digest, so a typo in the formatter shows up as a
 * value someone can check against `sha256sum </dev/null`. */
#define EMPTY "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

void test_sha_hex(void)
{
    SUITE("sha_hex_format");

    static const unsigned char empty_digest[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
    };
    char hex[SHA_HEX_LEN + 1];
    sha_hex_format(empty_digest, hex);
    CHECK_STR(hex, EMPTY);
    CHECK_INT(strlen(hex), SHA_HEX_LEN);

    /* Both nibbles of every byte, and the low nibble in particular: a formatter that
     * drops the leading zero of 0x0f produces a 63-character string that then fails to
     * match anything, which reads as "the server is corrupting images". */
    static const unsigned char edges[32] = {
        0x00, 0x0f, 0xf0, 0xff, [31] = 0x01,
    };
    sha_hex_format(edges, hex);
    CHECK_INT(strlen(hex), SHA_HEX_LEN);
    CHECK(strncmp(hex, "000ff0ff", 8) == 0, "got \"%.8s\", want \"000ff0ff\"", hex);
    CHECK_STR(hex + SHA_HEX_LEN - 2, "01");

    SUITE("sha_hex_valid");

    CHECK(sha_hex_valid(EMPTY), "a real digest must be valid");
    CHECK(!sha_hex_valid(NULL), "NULL is not a digest");
    CHECK(!sha_hex_valid(""), "the empty string is not a digest");
    /* A manifest that simply omits the field leaves this empty, and that must be
     * distinguishable from a match -- not silently accepted. */
    CHECK(!sha_hex_valid("abc"), "too short");
    CHECK(!sha_hex_valid(EMPTY "0"), "too long");
    CHECK(!sha_hex_valid("g3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "non-hex characters");
    CHECK(!sha_hex_valid("e3b0c44298fc1c149afbf4c8996fb924 7ae41e4649b934ca495991b7852b855"),
          "a space is not hex");

    SUITE("sha_hex_equal");

    CHECK(sha_hex_equal(EMPTY, EMPTY), "identical digests must match");

    /* Hex is case-insensitive, and sha256sum's output is lowercase while plenty of tools
     * emit uppercase. Rejecting on case would look exactly like a corrupt image. */
    char upper[SHA_HEX_LEN + 1];
    for (size_t i = 0; i < SHA_HEX_LEN; i++) {
        char c = EMPTY[i];
        upper[i] = (c >= 'a' && c <= 'f') ? (char)(c - 'a' + 'A') : c;
    }
    upper[SHA_HEX_LEN] = '\0';
    CHECK(sha_hex_equal(EMPTY, upper), "case must not matter");

    /* One nibble different, at each end -- a comparison that stops early or starts late
     * would pass one of these. */
    char a[SHA_HEX_LEN + 1], b[SHA_HEX_LEN + 1];
    memcpy(a, EMPTY, sizeof(a));
    memcpy(b, EMPTY, sizeof(b));
    a[0] = (a[0] == 'e') ? 'f' : 'e';
    CHECK(!sha_hex_equal(a, EMPTY), "a differing first character must not match");
    b[SHA_HEX_LEN - 1] = (b[SHA_HEX_LEN - 1] == '5') ? '6' : '5';
    CHECK(!sha_hex_equal(b, EMPTY), "a differing last character must not match");

    /* Anything malformed fails closed. This is the whole point: if a missing or
     * truncated hash compared equal, the manifest's sha256 field would be decoration and
     * nothing about the device would look different. */
    CHECK(!sha_hex_equal(NULL, EMPTY), "NULL never matches");
    CHECK(!sha_hex_equal(EMPTY, NULL), "NULL never matches");
    CHECK(!sha_hex_equal(NULL, NULL), "two NULLs are not a match");
    CHECK(!sha_hex_equal("", ""), "two empty strings are not a match");
    CHECK(!sha_hex_equal("abc", "abc"), "two equal non-digests are not a match");
}
