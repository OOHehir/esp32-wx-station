/* Free of ESP-IDF for the same reason as dns_pack.h: it is host-testable, and an
 * escaping bug is silent until an SSID with a quote in it breaks the setup page. */
#pragma once

#include <stddef.h>

/* Escape `in` for embedding inside a JSON string literal, writing at most out_len bytes
 * including the terminator. Truncates rather than overflows, and never truncates in the
 * middle of an escape sequence. `out` is always NUL-terminated when out_len > 0. */
void json_escape(const char *in, char *out, size_t out_len);
