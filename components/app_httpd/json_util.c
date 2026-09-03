#include "json_util.h"

#include <stdio.h>

void json_escape(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    size_t o = 0;
    /* Reserve room for the longest escape (\uXXXX, six bytes) plus the terminator, so a
     * truncation never lands inside a sequence and leaves invalid JSON behind. */
    for (size_t i = 0; in && in[i] && o + 6 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) {
                /* Control characters have no literal form in JSON. */
                o += (size_t)snprintf(out + o, out_len - o, "\\u%04x", c);
            } else {
                out[o++] = (char)c;
            }
        }
    }
    out[o] = '\0';
}
