/* Validating a coordinate that came from outside the firmware.
 *
 * Free of ESP-IDF so it can be tested on the host. It is small, and it is the reason this
 * file exists rather than an inline check: the string it approves is interpolated into
 * the query of an HTTPS URL, so anything it lets through with an '&' or a space in it
 * appends parameters to somebody else's request. Range-checking the number is the easy
 * half; refusing everything that is not a number is the half that matters.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* True when `s` is a plain decimal number within [min, max] and nothing else: an optional
 * leading '-', digits, at most one '.', digits. No exponent, no '+', no whitespace, no
 * hexadecimal, no leading or trailing anything.
 *
 * Deliberately not strtod(): it accepts "1e9", "0x10", " 12", "nan" and "inf", stops at
 * the first character it does not like rather than rejecting the string, and leaves the
 * caller to notice. Every one of those either breaks the URL or produces a request for
 * somewhere the user did not ask about.
 */
bool wx_coord_valid(const char *s, double min, double max);

/* True when `s` is safe to draw and to store as a place name: printable ASCII, no control
 * characters, no quotes or backslashes that would have to be escaped again on the status
 * page, and 1 to `max_len` characters. It never reaches a URL -- the forecast comes from
 * the coordinates -- so this is about the panel and the JSON, not about the request. */
bool wx_place_valid(const char *s, size_t max_len);
