#include "sha_hex.h"

#include <string.h>

static int nibble(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

void sha_hex_format(const unsigned char digest[32], char out[SHA_HEX_LEN + 1])
{
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = HEX[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = HEX[digest[i] & 0x0f];
    }
    out[SHA_HEX_LEN] = '\0';
}

bool sha_hex_valid(const char *s)
{
    if (!s || strlen(s) != SHA_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < SHA_HEX_LEN; i++) {
        if (nibble(s[i]) < 0) {
            return false;
        }
    }
    return true;
}

bool sha_hex_equal(const char *a, const char *b)
{
    if (!sha_hex_valid(a) || !sha_hex_valid(b)) {
        return false;
    }
    for (size_t i = 0; i < SHA_HEX_LEN; i++) {
        if (nibble(a[i]) != nibble(b[i])) {
            return false;
        }
    }
    return true;
}
