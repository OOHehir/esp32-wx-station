#include <stdlib.h>

#include "fixtures/forecast_dublin.h"
#include "app_ui.h"
#include "test.h"
#include "wx_location.h"
#include "wx_parse.h"

/* Parse the recorded fixture, or fail loudly. Every test below starts from the real
 * response, because a parser proved against JSON written to suit it has been proved
 * against the wrong document. */
static wx_parse_verdict_t parse_fixture(app_wx_forecast_t *out)
{
    return wx_parse(FORECAST_DUBLIN, strlen(FORECAST_DUBLIN), out);
}

/* The fixture with one substring swapped, so each malformed case stays recognisably the
 * real response with one thing wrong -- which is what a bad response from Open-Meteo
 * would look like, rather than a document from somewhere else entirely. */
static char *fixture_with(const char *find, const char *replace)
{
    const char *at = strstr(FORECAST_DUBLIN, find);
    if (at == NULL) {
        return NULL;
    }
    const size_t head = (size_t)(at - FORECAST_DUBLIN);
    const size_t tail = strlen(at + strlen(find));
    char *out = malloc(head + strlen(replace) + tail + 1);
    memcpy(out, FORECAST_DUBLIN, head);
    memcpy(out + head, replace, strlen(replace));
    memcpy(out + head + strlen(replace), at + strlen(find), tail + 1);
    return out;
}

static void check_rejected(const char *find, const char *replace,
                           wx_parse_verdict_t want, const char *what)
{
    char *body = fixture_with(find, replace);
    CHECK(body != NULL, "fixture does not contain \"%s\" -- test is stale", find);
    if (body == NULL) {
        return;
    }

    /* Pre-loaded with a forecast that must survive: a rejected response has to leave the
     * cached one alone, or a bad afternoon of weather data wipes a good morning's. */
    app_wx_forecast_t f;
    memset(&f, 0, sizeof(f));
    f.count = 99;
    f.hour[0].temp_c = -273.0f;

    wx_parse_verdict_t v = wx_parse(body, strlen(body), &f);
    CHECK(v == want, "%s: got %s, want %s", what,
          wx_parse_verdict_name(v), wx_parse_verdict_name(want));
    CHECK_INT(f.count, 99);
    CHECK(f.hour[0].temp_c == -273.0f, "%s overwrote the caller's forecast", what);
    free(body);
}

void test_wx_parse(void)
{
    SUITE("wx_parse -- the recorded response");

    app_wx_forecast_t f;
    memset(&f, 0, sizeof(f));
    CHECK_INT(parse_fixture(&f), WX_PARSE_OK);
    CHECK_INT(f.count, 13);

    /* Local wall-clock in, UTC out: 2026-09-03T20:00 local at offset +3600 is 19:00 UTC.
     * The offset is applied to every hour or to none, and this is where a parser that
     * stored local time and called it UTC would show up. */
    CHECK_INT(f.utc_offset_s, 3600);
    CHECK_STR(f.tz_abbr, "GMT+1");
    CHECK_INT(f.hour[0].t, 1788462000L);          /* 2026-09-03 19:00:00 UTC */
    CHECK_INT(f.hour[1].t - f.hour[0].t, 3600);

    /* Values from the fixture, read off the recorded JSON by hand. */
    CHECK(f.hour[0].temp_c > 15.69f && f.hour[0].temp_c < 15.71f,
          "hour 0 temp: got %.2f, want 15.7", (double)f.hour[0].temp_c);
    CHECK_INT(f.hour[0].precip_pct, 95);
    CHECK_INT(f.hour[0].solar_wm2, 14);
    CHECK_INT(f.hour[0].cloud_pct, 100);
    CHECK_INT(f.hour[0].weather_code, 53);
    CHECK_INT(f.hour[0].is_day, 1);

    /* Apparent temperature, which is its own series rather than a number derived here:
     * 14.4 against an air temperature of 15.7, and the two are adjacent floats in the
     * struct. A table row transposed with temperature_2m would leave both fields holding
     * plausible temperatures and every chart correct, which is exactly the silent failure
     * SERIES_KEY exists to prevent -- so the two are asserted apart, not just in range. */
    CHECK(f.hour[0].feels_c > 14.39f && f.hour[0].feels_c < 14.41f,
          "hour 0 feels-like: got %.2f, want 14.4", (double)f.hour[0].feels_c);
    CHECK(f.hour[10].feels_c > 12.79f && f.hour[10].feels_c < 12.81f,
          "hour 10 feels-like: got %.2f, want 12.8", (double)f.hour[10].feels_c);

    /* Hour 5 is the middle of the night: no sun, and is_day says so. The pair together
     * are what makes a sunlight chart of twelve zeroes read as night rather than as a
     * series the parser dropped. */
    CHECK_INT(f.hour[5].is_day, 0);
    CHECK_INT(f.hour[5].solar_wm2, 0);

    /* The last hour is on the other side of local midnight. A parser that mishandles the
     * date rollover gets this one wrong and every earlier one right. */
    CHECK_INT(f.hour[12].t, 1788505200L);         /* 2026-09-04 07:00:00 UTC */
    CHECK_INT(f.hour[12].is_day, 1);
    CHECK_INT(f.hour[12].solar_wm2, 54);
    CHECK_INT(f.hour[12].weather_code, 3);

    /* The daily block. Local wall-clock strings in the response, UTC in the struct, the
     * same conversion the hourly timestamps get -- and the fixture spans two days, so a
     * conversion applied to the first and forgotten on the second shows up here. */
    CHECK_INT(f.day_count, 2);
    CHECK_INT(f.day[0].sunrise, 1788413880L);     /* 2026-09-03 06:38 local */
    CHECK_INT(f.day[0].sunset,  1788462600L);     /* 2026-09-03 20:10 local */
    CHECK_INT(f.day[1].sunrise, 1788500340L);     /* 2026-09-04 06:39 local */
    CHECK_INT(f.day[1].sunset,  1788548880L);     /* 2026-09-04 20:08 local */

    /* A null sunrise is a day without one, not a broken response: north of the Arctic
     * circle that is what June looks like, and a device there must still draw a forecast.
     * Zero, so the layout can tell it from a real midnight. */
    {
        char *polar = fixture_with("\"sunrise\":[\"2026-09-03T06:38\"", "\"sunrise\":[null");
        CHECK(polar != NULL, "fixture does not contain the first sunrise -- test is stale");
        if (polar != NULL) {
            app_wx_forecast_t pf;
            memset(&pf, 0, sizeof(pf));
            CHECK_INT(wx_parse(polar, strlen(polar), &pf), WX_PARSE_OK);
            CHECK_INT(pf.day[0].sunrise, 0);
            CHECK_INT(pf.day[0].sunset, 1788462600L);
            free(polar);
        }
    }

    /* An absurd value is bounded rather than carried. A temperature of 1e400 parses to
     * an infinity, which reads as an ordinary float everywhere until app_ui scales a
     * chart from it and app_gfx_line -- unclipped by design -- walks it. */
    {
        char *wild = fixture_with("\"temperature_2m\":[15.7,", "\"temperature_2m\":[1e400,");
        CHECK(wild != NULL, "fixture does not contain the first temperature -- test is stale");
        if (wild != NULL) {
            app_wx_forecast_t wf;
            memset(&wf, 0, sizeof(wf));
            CHECK_INT(wx_parse(wild, strlen(wild), &wf), WX_PARSE_OK);
            CHECK(wf.hour[0].temp_c == 100.0f,
                  "an infinite temperature was not clamped: got %.2f", (double)wf.hour[0].temp_c);
            free(wild);
        }
    }

    SUITE("wx_parse -- rejects rather than half-parses");

    check_rejected("\"weather_code\":[", "\"weather_codes\":[",
                   WX_PARSE_MALFORMED, "a missing series");
    check_rejected("\"apparent_temperature\":[", "\"apparent_temperatures\":[",
                   WX_PARSE_MALFORMED, "a missing apparent temperature");
    check_rejected("\"utc_offset_seconds\":3600", "\"utc_offset_secondz\":3600",
                   WX_PARSE_MALFORMED, "a missing offset");
    check_rejected("\"hourly\":{", "\"hourlies\":{",
                   WX_PARSE_MALFORMED, "no hourly object");
    check_rejected("15.7,15.4,", "15.7,",
                   WX_PARSE_RAGGED, "a series one short");
    check_rejected("15.7,15.4,", "15.7,null,",
                   WX_PARSE_MALFORMED, "a null in a series");
    check_rejected("\"2026-09-03T20:00\"", "\"2026-09-03 20:00\"",
                   WX_PARSE_MALFORMED, "a timestamp of the wrong shape");
    check_rejected("\"2026-09-03T20:00\"", "\"2026-13-03T20:00\"",
                   WX_PARSE_MALFORMED, "an impossible month");

    /* The daily block is required, for the reason the hourly series are: the query asks
     * for it, so a response without it is one this parser does not understand, and half
     * a forecast must never replace a whole cached one. */
    check_rejected("\"daily\":{", "\"dailies\":{",
                   WX_PARSE_MALFORMED, "no daily object");
    check_rejected("\"sunrise\":[\"2026-09-03", "\"sunrises\":[\"2026-09-03",
                   WX_PARSE_MALFORMED, "a missing sunrise series");
    check_rejected("\"2026-09-03T20:10\",", "",
                   WX_PARSE_RAGGED, "a sunset series one short");
    check_rejected("\"2026-09-03T06:38\"", "\"2026-09-03 06:38\"",
                   WX_PARSE_MALFORMED, "a sunrise of the wrong shape");

    /* Truncation, at every length. Any prefix of a valid response must be refused; none
     * may parse to something that looks usable. One length at a time would have missed
     * the case where the cut lands between the arrays and the offset. */
    {
        const size_t n = strlen(FORECAST_DUBLIN);
        int accepted = 0;
        for (size_t cut = 1; cut < n; cut++) {
            app_wx_forecast_t t;
            memset(&t, 0, sizeof(t));
            if (wx_parse(FORECAST_DUBLIN, cut, &t) == WX_PARSE_OK) {
                accepted++;
            }
        }
        CHECK_INT(accepted, 0);
    }

    SUITE("wx_parse -- the guards");

    {
        app_wx_forecast_t t;
        memset(&t, 0, sizeof(t));
        CHECK_INT(wx_parse(FORECAST_DUBLIN, WX_PARSE_MAX_BYTES + 1, &t), WX_PARSE_TOO_LARGE);
        CHECK_INT(wx_parse(NULL, 10, &t), WX_PARSE_MALFORMED);
        CHECK_INT(wx_parse("{}", 2, &t), WX_PARSE_MALFORMED);
        CHECK_INT(wx_parse("", 0, &t), WX_PARSE_MALFORMED);
    }

    SUITE("wx_parse_iso_hour");

    {
        time_t t = 0;
        CHECK(wx_parse_iso_hour("1970-01-01T00:00", &t), "the epoch itself");
        CHECK_INT(t, 0);
        CHECK(wx_parse_iso_hour("2026-09-03T20:00", &t), "the fixture's first hour");
        CHECK_INT(t, 1788465600L);
        /* A leap day, and the century rule that catches a naive implementation. */
        CHECK(wx_parse_iso_hour("2024-02-29T00:00", &t), "a leap day");
        CHECK_INT(t, 1709164800L);
        CHECK(wx_parse_iso_hour("2000-02-29T00:00", &t), "a leap day in a leap century");
        CHECK_INT(t, 951782400L);

        CHECK(!wx_parse_iso_hour("2026-09-03T20:00:00", &t), "seconds are not this shape");
        CHECK(!wx_parse_iso_hour("2026-09-03T20:0", &t), "too short");
        CHECK(!wx_parse_iso_hour("2026-09-03T2x:00", &t), "not a digit");
        CHECK(!wx_parse_iso_hour("2026-00-03T20:00", &t), "month zero");
        CHECK(!wx_parse_iso_hour("2026-09-03T24:00", &t), "hour 24");
        CHECK(!wx_parse_iso_hour(NULL, &t), "a null string");
    }
}

/* PLAN 4c's table, and PLAN 5b's staleness test. Both are pure decisions whose failure
 * mode is a panel that looks entirely correct while showing the wrong thing, which is
 * why they are asserted here rather than eyeballed on glass. */
void test_wx_state(void)
{
    SUITE("app_ui_choose_mode -- PLAN 4c's table");

    /* No credentials beats everything, cached forecast included: after a factory reset a
     * device holds a forecast it can no longer refresh, and the screen worth showing is
     * the one explaining how to fix that. */
    CHECK_INT(app_ui_choose_mode(false, false, -1), APP_UI_UNPROVISIONED);
    CHECK_INT(app_ui_choose_mode(false, true, 0), APP_UI_UNPROVISIONED);
    CHECK_INT(app_ui_choose_mode(false, true, 99999), APP_UI_UNPROVISIONED);

    CHECK_INT(app_ui_choose_mode(true, false, -1), APP_UI_NO_FORECAST);

    CHECK_INT(app_ui_choose_mode(true, true, 0), APP_UI_OK);
    CHECK_INT(app_ui_choose_mode(true, true, 3599), APP_UI_OK);
    CHECK_INT(app_ui_choose_mode(true, true, 3600), APP_UI_STALE);
    CHECK_INT(app_ui_choose_mode(true, true, APP_UI_STALE_AFTER_S - 1), APP_UI_STALE);
    CHECK_INT(app_ui_choose_mode(true, true, APP_UI_STALE_AFTER_S), APP_UI_STALE_OLD);
    CHECK_INT(app_ui_choose_mode(true, true, 24 * 3600), APP_UI_STALE_OLD);

    /* An unknown age is shown, and shown as stale. A forecast fetched before the clock
     * was set may be perfectly good, but nothing can say so. */
    CHECK_INT(app_ui_choose_mode(true, true, -1), APP_UI_STALE);

    SUITE("app_wx_starts_this_hour");

    {
        app_wx_forecast_t f;
        memset(&f, 0, sizeof(f));
        CHECK(!app_wx_starts_this_hour(&f, 1788375600L), "an empty forecast starts no hour");
        CHECK(!app_wx_starts_this_hour(NULL, 1788375600L), "a null forecast");

        f.count = 13;
        f.hour[0].t = 1788375600L;                       /* 2026-09-02 19:00:00 UTC */
        CHECK(app_wx_starts_this_hour(&f, 1788375600L), "the hour it starts");
        CHECK(app_wx_starts_this_hour(&f, 1788375600L + 3599), "the last second of it");
        CHECK(!app_wx_starts_this_hour(&f, 1788375600L + 3600), "the next hour");
        CHECK(!app_wx_starts_this_hour(&f, 1788375600L + 86400), "a day later");

        /* A forecast from the future is refused as firmly as an old one: it means the
         * device's clock and the server's disagree, and drawing it would put confident
         * numbers under labels that have not happened. */
        CHECK(!app_wx_starts_this_hour(&f, 1788375600L - 1), "one second before it starts");
    }
}

/* The two helpers the panel's dates and sun times are built on.
 *
 * Both are silent when wrong in exactly the way PLAN 5b warns about: a date label that is
 * one day out, or a sunrise taken from the wrong day, renders as confidently as a correct
 * one. Neither can be checked by looking at the glass, so they are checked here.
 */
void test_wx_time(void)
{
    SUITE("app_wx_civil_local");

    {
        app_wx_civil_t c;

        app_wx_civil_local(0, 0, &c);
        CHECK_INT(c.year, 1970); CHECK_INT(c.mon, 1); CHECK_INT(c.day, 1);
        CHECK_INT(c.hour, 0); CHECK_INT(c.min, 0);

        /* The fixture's first hour, and the offset that made it: 19:00 UTC is 20:00 in
         * Dublin in September, on the same day. */
        app_wx_civil_local(1788375600L, 3600, &c);
        CHECK_INT(c.year, 2026); CHECK_INT(c.mon, 9); CHECK_INT(c.day, 2);
        CHECK_INT(c.hour, 20); CHECK_INT(c.min, 0);

        /* The offset carrying the date forwards over midnight, and backwards over it.
         * Getting these two right is the whole reason the offset is applied here rather
         * than assumed to be small. */
        app_wx_civil_local(1788393600L, 3600, &c);       /* 2026-09-03 00:00 UTC */
        CHECK_INT(c.day, 3); CHECK_INT(c.hour, 1);
        app_wx_civil_local(1788393600L, -3600, &c);
        CHECK_INT(c.mon, 9); CHECK_INT(c.day, 2); CHECK_INT(c.hour, 23);

        /* A leap day, and the century rule -- the same dates wx_parse_iso_hour is checked
         * against, so the two halves of the conversion are proved on the same days. */
        app_wx_civil_local(1709164800L, 0, &c);          /* 2024-02-29 00:00 UTC */
        CHECK_INT(c.year, 2024); CHECK_INT(c.mon, 2); CHECK_INT(c.day, 29);
        app_wx_civil_local(951782400L, 0, &c);           /* 2000-02-29 00:00 UTC */
        CHECK_INT(c.year, 2000); CHECK_INT(c.mon, 2); CHECK_INT(c.day, 29);

        /* Before the epoch, where truncating division would give hour -1 on the wrong
         * day. The device will never see it; the helper is still expected to be right. */
        app_wx_civil_local(-1, 0, &c);
        CHECK_INT(c.year, 1969); CHECK_INT(c.mon, 12); CHECK_INT(c.day, 31);
        CHECK_INT(c.hour, 23); CHECK_INT(c.min, 59);
    }

    SUITE("app_wx_sun_today");

    {
        app_wx_forecast_t f;
        memset(&f, 0, sizeof(f));
        time_t rise = -1, set = -1;

        CHECK(!app_wx_sun_today(NULL, &rise, &set), "a null forecast");
        CHECK(!app_wx_sun_today(&f, &rise, &set), "an empty forecast");

        /* The fixture's shape: hours starting on 2 September local, and two days of sun
         * times of which the first is that date. */
        f.count = 13;
        f.utc_offset_s = 3600;
        f.hour[0].t = 1788375600L;                       /* 2026-09-02 20:00 local */
        f.day_count = 2;
        f.day[0].sunrise = 1788327420L;                  /* 2026-09-02 06:37 local */
        f.day[0].sunset  = 1788376380L;                  /* 2026-09-02 20:13 local */
        f.day[1].sunrise = 1788413940L;                  /* 2026-09-03 06:39 local */
        f.day[1].sunset  = 1788462660L;                  /* 2026-09-03 20:11 local */

        CHECK(app_wx_sun_today(&f, &rise, &set), "the day the forecast starts in");
        CHECK_INT(rise, 1788327420L);
        CHECK_INT(set, 1788376380L);

        /* The lookup is by date, not by index. Swapping the two days must not change the
         * answer -- if it does, the panel is showing whichever day the server happened to
         * put first, which is right until the day a cache outlives its date. */
        {
            app_wx_day_t tmp = f.day[0];
            f.day[0] = f.day[1];
            f.day[1] = tmp;
            rise = set = -1;
            CHECK(app_wx_sun_today(&f, &rise, &set), "the days in the other order");
            CHECK_INT(rise, 1788327420L);
            CHECK_INT(set, 1788376380L);
        }

        /* A day that is not in the daily block at all: no answer, and the outputs left
         * alone rather than filled with the nearest thing to hand. */
        f.hour[0].t += 3 * 86400;
        rise = set = -1;
        CHECK(!app_wx_sun_today(&f, &rise, &set), "a date the daily block does not cover");
        CHECK_INT(rise, -1);
        CHECK_INT(set, -1);

        /* A polar day: no sunrise, but the sunset still dates the entry, so the pair is
         * found and the missing half comes back as zero for the layout to blank out. */
        memset(&f.day, 0, sizeof(f.day));
        f.hour[0].t = 1788375600L;
        f.day_count = 1;
        f.day[0].sunrise = 0;
        f.day[0].sunset  = 1788376380L;
        rise = set = -1;
        CHECK(app_wx_sun_today(&f, &rise, &set), "a day with a sunset and no sunrise");
        CHECK_INT(rise, 0);
        CHECK_INT(set, 1788376380L);
    }
}

/* A coordinate typed into a web form ends up interpolated into an HTTPS query, so the
 * validator is the boundary between a browser and somebody else's server. Range-checking
 * the number is the easy half; refusing everything that is not a number is the half a
 * missing test would let through. */
void test_wx_location(void)
{
    SUITE("wx_coord_valid -- the coordinates that should pass");

    CHECK(wx_coord_valid("53.3331", -90.0, 90.0), "Dublin latitude");
    CHECK(wx_coord_valid("-6.2489", -180.0, 180.0), "Dublin longitude");
    CHECK(wx_coord_valid("0", -90.0, 90.0), "the equator");
    CHECK(wx_coord_valid("-0.0", -90.0, 90.0), "negative zero");
    CHECK(wx_coord_valid("90", -90.0, 90.0), "the north pole, inclusive");
    CHECK(wx_coord_valid("-90", -90.0, 90.0), "the south pole, inclusive");
    CHECK(wx_coord_valid("180", -180.0, 180.0), "the antimeridian");
    CHECK(wx_coord_valid("12.345678", -90.0, 90.0), "eight decimal places");

    SUITE("wx_coord_valid -- out of range");

    CHECK(!wx_coord_valid("90.1", -90.0, 90.0), "past the pole");
    CHECK(!wx_coord_valid("-90.1", -90.0, 90.0), "past the other pole");
    CHECK(!wx_coord_valid("181", -180.0, 180.0), "past the antimeridian");
    CHECK(!wx_coord_valid("1000", -90.0, 90.0), "four digits");

    SUITE("wx_coord_valid -- not a number at all");

    /* The one that matters: an '&' would append a parameter to the forecast request. */
    CHECK(!wx_coord_valid("53.3&hourly=evil", -90.0, 90.0), "a query separator");
    CHECK(!wx_coord_valid("53.3 ", -90.0, 90.0), "a trailing space");
    CHECK(!wx_coord_valid(" 53.3", -90.0, 90.0), "a leading space");
    CHECK(!wx_coord_valid("53.3abc", -90.0, 90.0), "trailing rubbish");
    CHECK(!wx_coord_valid("1e2", -90.0, 90.0), "an exponent, which strtod would take");
    CHECK(!wx_coord_valid("0x10", -90.0, 90.0), "hexadecimal, which strtod would take");
    CHECK(!wx_coord_valid("nan", -90.0, 90.0), "nan, which strtod would take");
    CHECK(!wx_coord_valid("inf", -90.0, 90.0), "inf, which strtod would take");
    CHECK(!wx_coord_valid("+53.3", -90.0, 90.0), "a leading plus");
    CHECK(!wx_coord_valid(".5", -90.0, 90.0), "no integer part");
    CHECK(!wx_coord_valid("53.", -90.0, 90.0), "no fractional part");
    CHECK(!wx_coord_valid("53.3.7", -90.0, 90.0), "two points");
    CHECK(!wx_coord_valid("-", -90.0, 90.0), "a lone sign");
    CHECK(!wx_coord_valid("", -90.0, 90.0), "empty");
    CHECK(!wx_coord_valid(NULL, -90.0, 90.0), "null");
    CHECK(!wx_coord_valid("53.123456789", -90.0, 90.0), "absurd precision");

    SUITE("wx_place_valid");

    CHECK(wx_place_valid("Dublin", 31), "the default");
    CHECK(wx_place_valid("Dun Laoghaire", 31), "a space");
    CHECK(wx_place_valid("St. John's", 31), "an apostrophe is fine");
    CHECK(!wx_place_valid("", 31), "empty");
    CHECK(!wx_place_valid(NULL, 31), "null");
    CHECK(!wx_place_valid("Dún Laoghaire", 31), "non-ASCII, which the panel fonts lack");
    CHECK(!wx_place_valid("a\"b", 31), "a quote, which the status JSON would re-escape");
    CHECK(!wx_place_valid("a\\b", 31), "a backslash, for the same reason");
    CHECK(!wx_place_valid("a\nb", 31), "a newline");
    CHECK(!wx_place_valid("0123456789012345678901234567890123", 31), "too long");
}
