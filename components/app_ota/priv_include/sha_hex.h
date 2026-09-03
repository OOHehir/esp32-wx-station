/* Formatting and comparing a SHA-256 digest as text.
 *
 * Trivial, and therefore exactly the sort of thing that fails silently: a comparison
 * that returns true for everything turns the manifest's sha256 field into decoration,
 * and nothing about the device's behaviour would look different. Pure C, no ESP-IDF, so
 * the host tests can hold it to account.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SHA_HEX_LEN 64   /* characters, excluding the terminator */

/* Writes exactly SHA_HEX_LEN lowercase hex characters plus a terminator. */
void sha_hex_format(const unsigned char digest[32], char out[SHA_HEX_LEN + 1]);

/* True when both are well-formed SHA-256 hex strings naming the same digest. Comparison
 * is case-insensitive because hex is; anything that is not 64 hex characters is not a
 * digest and never matches -- an empty or truncated field must fail the check rather
 * than pass it. */
bool sha_hex_equal(const char *a, const char *b);

/* True when `s` is a well-formed SHA-256 hex string, so a caller can tell "the manifest
 * did not carry a hash" apart from "the hash did not match". */
bool sha_hex_valid(const char *s);
