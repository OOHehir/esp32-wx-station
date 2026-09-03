#include "wx_location.h"

#include <string.h>

bool wx_coord_valid(const char *s, double min, double max)
{
    if (s == NULL) {
        return false;
    }

    size_t i = 0;
    bool negative = false;
    if (s[0] == '-') {
        negative = true;
        i = 1;
    }

    /* Parsed by hand, in one pass, accumulating as we validate. Two passes -- check the
     * shape then convert -- would mean two places that have to agree about what the shape
     * is, and they drift. */
    double value = 0.0;
    size_t int_digits = 0;
    for (; s[i] >= '0' && s[i] <= '9'; i++, int_digits++) {
        value = value * 10.0 + (s[i] - '0');
    }
    /* Counted after the loop, not inside it: the counter is incremented by the loop's
     * third clause, so a check in the body tests the count before this digit and lets one
     * more through than it appears to. */
    if (int_digits == 0 || int_digits > 3) {
        return false;       /* ".5", "-", "" and "1000" alike */
    }

    if (s[i] == '.') {
        i++;
        size_t frac_digits = 0;
        double scale = 0.1;
        for (; s[i] >= '0' && s[i] <= '9'; i++, frac_digits++) {
            value += (s[i] - '0') * scale;
            scale /= 10.0;
        }
        /* Eight places is about a millimetre. More is not precision, it is a string that
         * came from somewhere other than a person typing a location. */
        if (frac_digits == 0 || frac_digits > 8) {
            return false;
        }
    }

    /* Anything left over is a rejection, not something to stop politely before. This is
     * the line that refuses "53.3&extra=1" and "53.3 " and "53.3abc". */
    if (s[i] != '\0') {
        return false;
    }

    if (negative) {
        value = -value;
    }
    return value >= min && value <= max;
}

bool wx_place_valid(const char *s, size_t max_len)
{
    if (s == NULL) {
        return false;
    }
    size_t n = strlen(s);
    if (n == 0 || n > max_len) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7e) {
            return false;   /* control characters and anything non-ASCII */
        }
        if (c == '"' || c == '\\') {
            return false;   /* would need escaping again wherever it is re-emitted */
        }
    }
    return true;
}
