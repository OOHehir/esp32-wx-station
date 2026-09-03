#include "wx_parse.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

/* The eight hourly series the query of PLAN 4a asks for, in the order they are unpacked.
 *
 * Named once, here, because the failure this table prevents is a silent one: a series
 * read into the wrong field produces a forecast that renders perfectly and is wrong.
 * Adding a series to the query means adding a row here and a field to app_wx_hour_t, and
 * the ragged-length check below then covers it for free. */
typedef enum {
    SERIES_TEMP = 0,
    SERIES_FEELS,
    SERIES_PRECIP,
    SERIES_PRECIP_PCT,
    SERIES_SOLAR,
    SERIES_CLOUD,
    SERIES_IS_DAY,
    SERIES_CODE,
    SERIES_COUNT,
} series_t;

static const char *const SERIES_KEY[SERIES_COUNT] = {
    [SERIES_TEMP]       = "temperature_2m",
    [SERIES_FEELS]      = "apparent_temperature",
    [SERIES_PRECIP]     = "precipitation",
    [SERIES_PRECIP_PCT] = "precipitation_probability",
    [SERIES_SOLAR]      = "shortwave_radiation",
    [SERIES_CLOUD]      = "cloud_cover",
    [SERIES_IS_DAY]     = "is_day",
    [SERIES_CODE]       = "weather_code",
};

const char *wx_parse_verdict_name(wx_parse_verdict_t v)
{
    switch (v) {
    case WX_PARSE_OK:        return "OK";
    case WX_PARSE_TOO_LARGE: return "TOO_LARGE";
    case WX_PARSE_MALFORMED: return "MALFORMED";
    case WX_PARSE_EMPTY:     return "EMPTY";
    case WX_PARSE_RAGGED:    return "RAGGED";
    }
    return "?";
}

/* Days from 1970-01-01 to y-m-d, proleptic Gregorian.
 *
 * Howard Hinnant's civil-from-days, which is exact for every date this device will ever
 * see and needs no library. The alternative is timegm(), which glibc has and several
 * other libcs do not, or mktime(), which would read the process timezone and make the
 * answer depend on where the test is run. */
static long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                    /* [0, 399]   */
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;     /* [0, 146096]*/
    return (long)era * 146097L + (long)doe - 719468L;
}

static bool digits(const char *s, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

bool wx_parse_iso_hour(const char *s, time_t *out)
{
    /* Exactly "YYYY-MM-DDTHH:MM". Open-Meteo emits no seconds and no zone suffix on this
     * field; anything else is a response shaped differently from the one this parser was
     * written against, and guessing at it is how a wrong hour reaches the glass. */
    if (s == NULL || strlen(s) != 16) {
        return false;
    }
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':') {
        return false;
    }

    int y, mo, d, h, mi;
    if (!digits(s, 4, &y) || !digits(s + 5, 2, &mo) || !digits(s + 8, 2, &d) ||
        !digits(s + 11, 2, &h) || !digits(s + 14, 2, &mi)) {
        return false;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59) {
        return false;
    }

    *out = (time_t)(days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400L
                    + h * 3600L + mi * 60L);
    return true;
}

/* A cJSON array of numbers, or NULL if the key is absent or not an array. */
static const cJSON *number_array(const cJSON *hourly, const char *key)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(hourly, key);
    return cJSON_IsArray(a) ? a : NULL;
}

static double at(const cJSON *array, int i)
{
    const cJSON *v = cJSON_GetArrayItem(array, i);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

/* Every element of `array` a number? A null in a series -- which Open-Meteo does emit
 * for a variable it has no data for -- would otherwise read as 0.0 and put a plausible
 * zero on the panel. */
static bool all_numbers(const cJSON *array, int n)
{
    for (int i = 0; i < n; i++) {
        if (!cJSON_IsNumber(cJSON_GetArrayItem(array, i))) {
            return false;
        }
    }
    return true;
}

/* One `daily` timestamp into UTC. A null is a day without that event -- a polar summer
 * has no sunrise -- and reads as zero rather than as a malformed response. */
static bool parse_day_time(const cJSON *v, int32_t utc_offset_s, time_t *out)
{
    if (cJSON_IsNull(v)) {
        *out = 0;
        return true;
    }
    if (!cJSON_IsString(v) || !wx_parse_iso_hour(v->valuestring, out)) {
        return false;
    }
    *out -= utc_offset_s;
    return true;
}

static uint8_t clamp_u8(double v)
{
    if (v < 0.0)   { return 0; }
    if (v > 255.0) { return 255; }
    return (uint8_t)(v + 0.5);
}

/* The float series need the same treatment as the integer ones, for a sharper reason.
 * app_ui scales a chart from these values and hands the result to app_gfx_line, whose
 * Bresenham walk is deliberately unclipped -- so one absurd temperature does not draw a
 * wrong trace, it steps a couple of billion pixels inside the display task and trips the
 * watchdog. The `!(v >= lo)` test catches NaN, which every ordinary comparison passes. */
static float clamp_f(double v, float lo, float hi)
{
    if (!(v >= (double)lo)) { return lo; }
    if (v > (double)hi)     { return hi; }
    return (float)v;
}

static int16_t clamp_i16(double v)
{
    if (v < -32768.0) { return -32768; }
    if (v >  32767.0) { return  32767; }
    return (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
}

wx_parse_verdict_t wx_parse(const char *json, size_t len, app_wx_forecast_t *out)
{
    if (json == NULL || out == NULL) {
        return WX_PARSE_MALFORMED;
    }
    if (len > WX_PARSE_MAX_BYTES) {
        /* Checked before cJSON is handed the buffer rather than after: the cap exists to
         * bound the work, and a cap enforced after parsing bounds nothing. */
        return WX_PARSE_TOO_LARGE;
    }

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return WX_PARSE_MALFORMED;
    }

    wx_parse_verdict_t verdict = WX_PARSE_MALFORMED;
    app_wx_forecast_t f;
    memset(&f, 0, sizeof(f));

    const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    if (!cJSON_IsObject(hourly)) {
        goto done;
    }

    const cJSON *times = cJSON_GetObjectItemCaseSensitive(hourly, "time");
    if (!cJSON_IsArray(times)) {
        goto done;
    }

    const cJSON *series[SERIES_COUNT];
    for (int s = 0; s < SERIES_COUNT; s++) {
        if ((series[s] = number_array(hourly, SERIES_KEY[s])) == NULL) {
            goto done;
        }
    }

    const int n_time = cJSON_GetArraySize(times);
    if (n_time == 0) {
        verdict = WX_PARSE_EMPTY;
        goto done;
    }

    /* Every series must agree with `time` about how many hours there are.
     *
     * Rejecting rather than taking the shortest is the deliberate choice of PLAN 5b: a
     * response whose series disagree is not a short forecast, it is a response this
     * parser does not understand, and the honest answer is to keep the cached one. */
    for (int s = 0; s < SERIES_COUNT; s++) {
        if (cJSON_GetArraySize(series[s]) != n_time) {
            verdict = WX_PARSE_RAGGED;
            goto done;
        }
        if (!all_numbers(series[s], n_time)) {
            goto done;
        }
    }

    /* More hours than the display has room for is not an error -- the query asks for 13
     * and the panel shows 13, but a future query may ask for more and the extra hours are
     * simply not carried. Fewer is equally fine and shows as a shorter chart. */
    const int n = n_time < APP_WX_HOURS ? n_time : APP_WX_HOURS;

    for (int i = 0; i < n; i++) {
        const cJSON *ts = cJSON_GetArrayItem(times, i);
        if (!cJSON_IsString(ts) || !wx_parse_iso_hour(ts->valuestring, &f.hour[i].t)) {
            goto done;
        }
        f.hour[i].temp_c       = clamp_f(at(series[SERIES_TEMP], i), -100.0f, 100.0f);
        f.hour[i].feels_c      = clamp_f(at(series[SERIES_FEELS], i), -100.0f, 100.0f);
        f.hour[i].precip_mm    = clamp_f(at(series[SERIES_PRECIP], i), 0.0f, 1000.0f);
        f.hour[i].precip_pct   = clamp_u8(at(series[SERIES_PRECIP_PCT], i));
        f.hour[i].solar_wm2    = clamp_i16(at(series[SERIES_SOLAR], i));
        f.hour[i].cloud_pct    = clamp_u8(at(series[SERIES_CLOUD], i));
        f.hour[i].weather_code = clamp_u8(at(series[SERIES_CODE], i));
        f.hour[i].is_day       = at(series[SERIES_IS_DAY], i) != 0.0;
    }
    f.count = (uint8_t)n;

    /* timezone=auto gives local wall-clock times plus the offset that made them. The
     * struct stores UTC, so the offset comes back off; app_ui puts it back on to draw a
     * local hour label. Storing local time would mean a forecast that silently shifts
     * when the device is moved or when the clocks change mid-forecast. */
    const cJSON *off = cJSON_GetObjectItemCaseSensitive(root, "utc_offset_seconds");
    if (!cJSON_IsNumber(off)) {
        goto done;
    }
    f.utc_offset_s = (int32_t)off->valuedouble;
    for (int i = 0; i < n; i++) {
        f.hour[i].t -= f.utc_offset_s;
    }

    const cJSON *abbr = cJSON_GetObjectItemCaseSensitive(root, "timezone_abbreviation");
    if (cJSON_IsString(abbr) && abbr->valuestring != NULL) {
        snprintf(f.tz_abbr, sizeof(f.tz_abbr), "%s", abbr->valuestring);
    }

    /* The daily block: sunrise and sunset, in the same local wall-clock shape as the
     * hourly timestamps and converted to UTC the same way.
     *
     * Required, not optional. The query asks for it, so a response without it is a
     * response this parser does not understand, and PLAN 5b's rule is that a half-
     * understood response leaves the cached forecast alone rather than half-replacing it.
     * The one thing that is *not* a failure is a null time: above the Arctic circle a day
     * genuinely has no sunrise, and Open-Meteo says so with a null. That reads as zero,
     * which app_wx_sun_today and the layout both treat as "no such event today". */
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!cJSON_IsObject(daily)) {
        goto done;
    }

    const cJSON *d_time = cJSON_GetObjectItemCaseSensitive(daily, "time");
    const cJSON *d_rise = cJSON_GetObjectItemCaseSensitive(daily, "sunrise");
    const cJSON *d_set  = cJSON_GetObjectItemCaseSensitive(daily, "sunset");
    if (!cJSON_IsArray(d_time) || !cJSON_IsArray(d_rise) || !cJSON_IsArray(d_set)) {
        goto done;
    }

    const int n_days = cJSON_GetArraySize(d_time);
    if (n_days == 0) {
        verdict = WX_PARSE_EMPTY;
        goto done;
    }
    if (cJSON_GetArraySize(d_rise) != n_days || cJSON_GetArraySize(d_set) != n_days) {
        verdict = WX_PARSE_RAGGED;
        goto done;
    }

    /* More days than the struct carries is not an error, for the same reason more hours
     * is not: the query asks for two and the panel draws one. */
    const int nd = n_days < APP_WX_DAYS ? n_days : APP_WX_DAYS;
    for (int i = 0; i < nd; i++) {
        if (!parse_day_time(cJSON_GetArrayItem(d_rise, i), f.utc_offset_s,
                            &f.day[i].sunrise) ||
            !parse_day_time(cJSON_GetArrayItem(d_set, i), f.utc_offset_s,
                            &f.day[i].sunset)) {
            goto done;
        }
    }
    f.day_count = (uint8_t)nd;

    *out = f;
    verdict = WX_PARSE_OK;

done:
    cJSON_Delete(root);
    return verdict;
}
