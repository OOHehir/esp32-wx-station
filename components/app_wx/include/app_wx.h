/* app_wx -- the forecast: what it looks like, and (phase 3) where it comes from.
 *
 * The types are deliberately free of ESP-IDF: app_ui and the host tests include this
 * header, and neither may drag in a toolchain. The fetching half arrives in phase 3 and
 * goes behind APP_WX_HAS_FETCH, so this file stays compilable with plain gcc.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Twelve hours on the display plus the hour in progress. Open-Meteo's `forecast_hours=13`
 * starts at the current hour, verified against the live API, so the array needs no
 * offset hunting -- see PLAN 4a, and PLAN 8 on why that is asserted rather than assumed. */
#define APP_WX_HOURS 13

typedef struct {
    time_t  t;              /* start of the hour, UTC */
    float   temp_c;
    /* Open-Meteo's apparent_temperature: the air temperature adjusted for wind chill,
     * humidity and solar radiation. A separate series rather than something derived
     * here -- the humidity and wind it is computed from are not otherwise fetched. */
    float   feels_c;
    float   precip_mm;
    uint8_t precip_pct;
    int16_t solar_wm2;      /* shortwave radiation */
    uint8_t cloud_pct;
    uint8_t weather_code;   /* WMO */
    bool    is_day;
} app_wx_hour_t;

/* Sunrise and sunset for one calendar day, UTC like everything else here.
 *
 * Two days rather than one because the thirteen hours on the panel cross midnight: the
 * day the forecast starts in is the one drawn, and the second is what makes finding it a
 * lookup by date rather than a guess that hour[0] and daily[0] agree. Zero means the day
 * has no such event -- above the Arctic circle that is an ordinary answer, not a fault. */
typedef struct {
    time_t sunrise;
    time_t sunset;
} app_wx_day_t;

#define APP_WX_DAYS 2

typedef struct {
    app_wx_hour_t hour[APP_WX_HOURS];
    uint8_t       count;
    int32_t       utc_offset_s;
    time_t        fetched_at;   /* 0 == never */
    char          tz_abbr[8];
    app_wx_day_t  day[APP_WX_DAYS];
    uint8_t       day_count;
} app_wx_forecast_t;

/* --- The fetching half (phase 3) ---------------------------------------------------- */
/*
 * Behind APP_WX_HAS_FETCH so this header stays compilable with plain gcc: app_ui and the
 * host tests include it, and neither may drag in a toolchain.
 */
#ifdef APP_WX_HAS_FETCH
#include "esp_err.h"

/* Load the cached forecast from NVS and start nothing else. Call once at start-up,
 * before the display draws: a device that has just rebooted then shows the last good
 * forecast marked stale rather than a blank panel for up to an hour (PLAN 4b). */
esp_err_t app_wx_start(void);

/* Fetch, parse and cache. Blocks for up to the configured timeout, so it belongs on a
 * task that can afford to wait.
 *
 * **The calling task needs about 12 KB of stack.** Nearly all of it is the TLS handshake,
 * and overrunning it does not look like a network fault: it is a stack protection fault
 * reported against whatever task made the call, which sends you reading that task's code
 * rather than this line. Measured on the bench board -- 4 KB, comfortable for everything
 * else app_display does, crashed on the first handshake.
 *
 * On any failure the cached forecast is left exactly as it was and the error is
 * returned: the caller's job is to keep showing the old forecast, marked stale, not to
 * blank the panel because one request timed out. */
esp_err_t app_wx_fetch(void);

/* The forecast this device currently holds, cached or freshly fetched. Returns false when
 * there has never been one, in which case `out` is untouched. */
bool app_wx_get(app_wx_forecast_t *out);

/* Where the forecast is fetched for. Strings, because they are decimal degrees with a
 * sign and go straight into a URL; the Kconfig defaults are the compiled-in fallback and
 * NVS overrides them once somebody sets a location from the portal. */
typedef struct {
    char lat[16];
    char lon[16];
    char place[32];
} app_wx_location_t;

/* The location currently in force, NVS if set and the build's defaults otherwise. */
void app_wx_get_location(app_wx_location_t *out);

/* Validate and store a location. Returns ESP_ERR_INVALID_ARG when any field is refused,
 * having stored nothing -- a half-applied location would fetch for one place and label
 * the panel with another. Takes effect at the next fetch. */
esp_err_t app_wx_set_location(const char *lat, const char *lon, const char *place);

/* Seconds since the held forecast was fetched, or -1 when there is none. The display uses
 * this to decide whether to say "stale" rather than inventing its own rule. */
int64_t app_wx_age_s(void);

#endif /* APP_WX_HAS_FETCH */

/* --- Pure helpers, used by app_ui and tested on the host ---------------------------- */

/* Reductions from `first` to the end of the forecast.
 *
 * `first` exists because the header and the charts want different answers. The header's
 * High and Low mean "now and the twelve hours ahead", so they start at 0; the charts draw
 * hours 1 to 12, with the current hour shown as the big number instead, so they start at
 * 1. Scaling a chart from an hour it does not plot puts an axis label on a value that is
 * not there and flattens the bars that are -- and in the sunlight chart it also loses the
 * peak highlight, because the peak is an hour off the left of it. `first` beyond the end
 * reduces over nothing.
 *
 * Lowest and highest temperature. Returns false and leaves the outputs untouched on an
 * empty range, so a caller that ignores the result still cannot print an uninitialised
 * number. */
bool app_wx_temp_range(const app_wx_forecast_t *f, uint8_t first,
                       float *out_min, float *out_max);

/* Largest value in each series, for scaling a chart's y axis. Zero when every hour is
 * zero -- a dry day and a missing forecast must not look the same, which is why the
 * caller is expected to check `count` rather than infer it from a zero here. */
float   app_wx_precip_max(const app_wx_forecast_t *f, uint8_t first);
int16_t app_wx_solar_max(const app_wx_forecast_t *f, uint8_t first);

/* Sunrise and sunset for the day the forecast starts in, UTC.
 *
 * Deliberately that day rather than "the next of each": at 22:00 the next sunset is
 * tomorrow's and the next sunrise is tomorrow's too, which draws a sunset before a
 * sunrise and reads as a bug. Today's pair is what the words mean, and both are
 * unambiguous whether they have happened yet or not.
 *
 * Returns false when there is no forecast, no daily data, or no entry for that date, in
 * which case the outputs are untouched -- a panel drawing 00:00 for a sunrise it does not
 * have is the kind of confident wrong number PLAN 5b is written against. */
bool app_wx_sun_today(const app_wx_forecast_t *f, time_t *out_rise, time_t *out_set);

/* Broken-down local time, without a TZ database.
 *
 * The device has no zoneinfo and localtime() would answer for the build machine's zone in
 * the host tests, so the offset Open-Meteo already returned is applied by hand and the
 * civil date derived arithmetically (Howard Hinnant's civil-from-days, the inverse of the
 * one wx_parse uses). `utc` of 0 is a real instant here, not a sentinel: callers that mean
 * "never" must check before calling. */
typedef struct {
    int     year;
    uint8_t mon;    /* 1-12 */
    uint8_t day;    /* 1-31 */
    uint8_t hour;   /* 0-23 */
    uint8_t min;    /* 0-59 */
} app_wx_civil_t;

void app_wx_civil_local(time_t utc, int32_t utc_offset_s, app_wx_civil_t *out);

/* Does this forecast still start at the hour it is being drawn in?
 *
 * A cached forecast is fine to show and a wrong one is not, and the difference is only
 * ever this: `hour[0]` is the hour in progress. Past that, the leftmost column is
 * labelled with an hour that has already happened, which is not stale data, it is wrong
 * data wearing a stale label. PLAN 5b names the test.
 *
 * `now` is UTC. Tolerates a forecast up to one hour old, because that is exactly the
 * window in which hour[0] is still the current hour. */
bool app_wx_starts_this_hour(const app_wx_forecast_t *f, time_t now);

/* Drop the hours that have already happened, so hour[0] is the hour in progress again.
 *
 * This is what lets an old forecast still be drawn honestly. PLAN 4c says a cached
 * forecast is shown with a stale marker rather than replaced by "waiting"; the layout
 * says hour[0] is now, and an hour-old forecast drawn unshifted would label its leftmost
 * column with an hour that has passed. Shifting satisfies both: the panel keeps every
 * hour it can still tell the truth about, and simply grows shorter as it ages.
 *
 * `now` is UTC. Returns false when nothing usable is left -- every hour is behind us --
 * in which case `f` is untouched & the caller has no forecast rather than an empty one.
 * A forecast that already starts at the current hour is left exactly as it is. */
bool app_wx_rebase(app_wx_forecast_t *f, time_t now);

#ifdef __cplusplus
}
#endif
