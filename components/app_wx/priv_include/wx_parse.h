/* Turn an Open-Meteo response into an app_wx_forecast_t.
 *
 * Free of ESP-IDF so it can be tested on the host against a recorded fixture (PLAN 5b).
 * cJSON is the one dependency, and app_ota already carries it, so it costs nothing new.
 *
 * This is the layer where a silent bug is most expensive. A parser that half-succeeds
 * puts a plausible-looking forecast on a panel that stays there for an hour, and a
 * wrong number nobody can tell is wrong is worse than a blank screen with "no forecast"
 * on it. Every failure here is therefore total: the output struct is left untouched
 * unless every field of every hour arrived.
 */
#pragma once

#include <stddef.h>

#include "app_wx.h"

typedef enum {
    WX_PARSE_OK = 0,
    WX_PARSE_TOO_LARGE,   /* body over the cap; rejected before cJSON sees it */
    WX_PARSE_MALFORMED,   /* not JSON, or `hourly` and its arrays are not where they must be */
    WX_PARSE_EMPTY,       /* valid JSON, no hours in it */
    WX_PARSE_RAGGED,      /* the series disagree about how many hours there are */
} wx_parse_verdict_t;

/* Largest response accepted. PLAN 4a measured the real one at 1103 bytes and this is
 * seven times that: enough for the query to grow a series, far short of anything that
 * could be a different document entirely. */
#define WX_PARSE_MAX_BYTES 8192

/* Parse `json` (`len` bytes, need not be NUL-terminated) into `out`.
 *
 * `out` is written only on WX_PARSE_OK. On every failure it is left exactly as it was,
 * so a caller holding a cached forecast keeps it rather than having it half-overwritten
 * by a bad response. */
wx_parse_verdict_t wx_parse(const char *json, size_t len, app_wx_forecast_t *out);

/* Human-readable name for a verdict, for logs and test failures. */
const char *wx_parse_verdict_name(wx_parse_verdict_t v);

/* Seconds since the epoch for a `YYYY-MM-DDTHH:MM` string read as UTC.
 *
 * Exposed for the tests. Deliberately not strptime/timegm: both consult the process
 * timezone or are absent on some libcs, and a parser whose result depends on the TZ
 * environment variable of whoever runs it is not one to trust on a device that has no
 * timezone at all. Returns false on anything that is not exactly that shape.
 */
bool wx_parse_iso_hour(const char *s, time_t *out);
