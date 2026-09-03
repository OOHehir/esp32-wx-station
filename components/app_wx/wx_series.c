/* Pure reductions over a forecast. Separate from the fetching half so the host tests can
 * link them without a toolchain (PLAN 5b). */
#include "app_wx.h"

#include <string.h>

bool app_wx_rebase(app_wx_forecast_t *f, time_t now)
{
    if (f == NULL || f->count == 0) {
        return false;
    }
    const int64_t delta = (int64_t)now - (int64_t)f->hour[0].t;
    if (delta < 0) {
        /* A forecast from the future: the device's clock and the server's disagree, and
         * shifting would not make the labels true. Refused, as it always was. */
        return false;
    }
    if (app_wx_starts_this_hour(f, now)) {
        return true;
    }
    const int64_t k = delta / 3600;
    if (k >= (int64_t)f->count) {
        return false;
    }
    const uint8_t drop = (uint8_t)k;
    memmove(&f->hour[0], &f->hour[drop],
            (size_t)(f->count - drop) * sizeof(f->hour[0]));
    f->count = (uint8_t)(f->count - drop);
    /* `day` is looked up by date rather than by index, so it needs no shifting -- and
     * must not get one, or a forecast that crossed midnight would lose today. */
    return true;
}

bool app_wx_temp_range(const app_wx_forecast_t *f, uint8_t first,
                       float *out_min, float *out_max)
{
    if (f == NULL || first >= f->count || first >= APP_WX_HOURS) {
        return false;
    }
    float lo = f->hour[first].temp_c;
    float hi = lo;
    for (uint8_t i = (uint8_t)(first + 1); i < f->count && i < APP_WX_HOURS; i++) {
        if (f->hour[i].temp_c < lo) { lo = f->hour[i].temp_c; }
        if (f->hour[i].temp_c > hi) { hi = f->hour[i].temp_c; }
    }
    if (out_min != NULL) { *out_min = lo; }
    if (out_max != NULL) { *out_max = hi; }
    return true;
}

float app_wx_precip_max(const app_wx_forecast_t *f, uint8_t first)
{
    if (f == NULL) {
        return 0.0f;
    }
    float m = 0.0f;
    for (uint8_t i = first; i < f->count && i < APP_WX_HOURS; i++) {
        if (f->hour[i].precip_mm > m) { m = f->hour[i].precip_mm; }
    }
    return m;
}

int16_t app_wx_solar_max(const app_wx_forecast_t *f, uint8_t first)
{
    if (f == NULL) {
        return 0;
    }
    int16_t m = 0;
    for (uint8_t i = first; i < f->count && i < APP_WX_HOURS; i++) {
        if (f->hour[i].solar_wm2 > m) { m = f->hour[i].solar_wm2; }
    }
    return m;
}

bool app_wx_starts_this_hour(const app_wx_forecast_t *f, time_t now)
{
    if (f == NULL || f->count == 0) {
        return false;
    }
    /* hour[0] is the start of an hour, so "the same hour as now" is a window that opens
     * at hour[0] and closes 3600 seconds later. A forecast from the future is refused as
     * firmly as an old one: it means the device's clock and the server's disagree, and
     * drawing it would put confident numbers under wrong labels. */
    const int64_t delta = (int64_t)now - (int64_t)f->hour[0].t;
    return delta >= 0 && delta < 3600;
}

/* Civil date from a day count, proleptic Gregorian.
 *
 * Howard Hinnant's civil_from_days, the exact inverse of the days_from_civil wx_parse
 * uses to read a timestamp. The pair is deliberately symmetric: a date that survives one
 * and not the other is a bug that only shows up on one day of the year, which is the
 * worst possible way to find it. */
static void civil_from_days(long z, int *y, unsigned *m, unsigned *d)
{
    z += 719468L;
    const long era = (z >= 0 ? z : z - 146096L) / 146097L;
    const unsigned long doe = (unsigned long)(z - era * 146097L);            /* [0, 146096] */
    const unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long yr = (long)yoe + era * 400;
    const unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);       /* [0, 365]    */
    const unsigned long mp = (5 * doy + 2) / 153;                            /* [0, 11]     */
    const unsigned long dy = doy - (153 * mp + 2) / 5 + 1;                   /* [1, 31]     */
    const unsigned long mo = mp + (mp < 10 ? 3 : -9);                        /* [1, 12]     */

    *y = (int)(yr + (mo <= 2));
    *m = (unsigned)mo;
    *d = (unsigned)dy;
}

void app_wx_civil_local(time_t utc, int32_t utc_offset_s, app_wx_civil_t *out)
{
    if (out == NULL) {
        return;
    }

    const long long local = (long long)utc + (long long)utc_offset_s;

    /* Floor division, not truncation. C divides towards zero, so a local time before the
     * epoch would land on the wrong day and an hour of 23 would print as -1. The device
     * will never see 1969, but the host tests can, and a helper that is only correct for
     * inputs somebody happened to think of is not one to build a date label on. */
    long days = (long)(local / 86400);
    long secs = (long)(local % 86400);
    if (secs < 0) {
        secs += 86400;
        days -= 1;
    }

    int y = 0;
    unsigned m = 0, d = 0;
    civil_from_days(days, &y, &m, &d);

    out->year = y;
    out->mon  = (uint8_t)m;
    out->day  = (uint8_t)d;
    out->hour = (uint8_t)(secs / 3600);
    out->min  = (uint8_t)((secs / 60) % 60);
}

bool app_wx_sun_today(const app_wx_forecast_t *f, time_t *out_rise, time_t *out_set)
{
    if (f == NULL || f->count == 0 || f->day_count == 0) {
        return false;
    }

    /* Matched on the local calendar date rather than taken from day[0].
     *
     * Open-Meteo does return today first, so the two agree today -- but a forecast is
     * also read back from NVS after a reboot, and the cheap assumption is exactly the one
     * that would put yesterday's sunset under today's hours for as long as the cache
     * outlives the day it was fetched on. */
    app_wx_civil_t want;
    app_wx_civil_local(f->hour[0].t, f->utc_offset_s, &want);

    for (uint8_t i = 0; i < f->day_count && i < APP_WX_DAYS; i++) {
        /* Either event dates the day, so a polar day with a sunset and no sunrise still
         * identifies itself. Both zero is a day with neither, which matches nothing. */
        const time_t stamp = f->day[i].sunrise != 0 ? f->day[i].sunrise : f->day[i].sunset;
        if (stamp == 0) {
            continue;
        }

        app_wx_civil_t got;
        app_wx_civil_local(stamp, f->utc_offset_s, &got);
        if (got.year != want.year || got.mon != want.mon || got.day != want.day) {
            continue;
        }

        if (out_rise != NULL) { *out_rise = f->day[i].sunrise; }
        if (out_set  != NULL) { *out_set  = f->day[i].sunset; }
        return true;
    }
    return false;
}
