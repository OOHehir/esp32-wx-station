/* app_ui and app_wx's pure helpers.
 *
 * The band test here is the one that matters: test_app_gfx proves the primitives are
 * seam-safe on a synthetic scene, this proves the actual layout is, which is the thing
 * that ships (PLAN 5a).
 */
#include "test.h"
#include "app_ui.h"
#include <stdlib.h>
#include <string.h>

#define WHOLE ((size_t)APP_GFX_WIDTH * APP_GFX_HEIGHT)

/* A forecast with shape in every series: a warm first hour, rain at the start, sun only
 * at the ends, and a day/night transition. Fixed, so a layout change shows up as a
 * deliberate edit rather than as noise. */
static void sample_forecast(app_wx_forecast_t *f)
{
    static const float  temp[13]   = {19.9f, 18.3f, 18.4f, 18.2f, 18.0f, 18.3f, 18.2f,
                                      17.8f, 17.5f, 18.1f, 18.2f, 18.2f, 18.1f};
    /* Colder than the air all day, and a decimal digit that differs from it: the header
     * draws both to one place, and two series that happened to round the same way would
     * let a layout drawing temp_c twice pass this. */
    static const float  feels[13]  = {17.4f, 15.6f, 15.8f, 15.5f, 15.2f, 15.6f, 15.5f,
                                      14.9f, 14.6f, 15.3f, 15.5f, 15.4f, 15.3f};
    static const float  rain[13]   = {0.1f, 1.4f, 0.9f, 0, 0, 0, 0, 0, 0, 0, 0, 0.1f, 0.7f};
    static const uint8_t pct[13]   = {20, 80, 70, 10, 5, 5, 5, 5, 5, 10, 15, 30, 60};
    static const int16_t solar[13] = {66, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 13};

    memset(f, 0, sizeof(*f));
    f->count = 13;
    f->utc_offset_s = 3600;
    f->fetched_at = 1000;
    for (uint8_t i = 0; i < 13; i++) {
        f->hour[i].t = 1000 + 3600 * i;
        f->hour[i].temp_c = temp[i];
        f->hour[i].feels_c = feels[i];
        f->hour[i].precip_mm = rain[i];
        f->hour[i].precip_pct = pct[i];
        f->hour[i].solar_wm2 = solar[i];
        f->hour[i].is_day = (i == 0 || i == 12);
    }

    /* Sun times on the day hour[0] falls in, so the band tests actually render the two
     * strings the layout draws from them. Without these the sunlight heading is the one
     * piece of text no seam test ever crosses. */
    f->day_count = 2;
    f->day[0].sunrise = 6 * 3600 - 3600;        /* 06:00 local, 1 Jan */
    f->day[0].sunset  = 20 * 3600 - 3600;       /* 20:00 local, same day */
    f->day[1].sunrise = 86400 + 6 * 3600 - 3600;
    f->day[1].sunset  = 86400 + 20 * 3600 - 3600;
}

static app_ui_state_t sample_state(app_ui_mode_t mode)
{
    app_ui_state_t st = {
        .mode = mode, .now = 1000, .place = "Dublin", .version = "v0.1.0",
        .net_state = "connected", .ip = "192.168.1.124",
        .ap_ssid = "wx-station-a1b2c3", .ap_pass = "EXAMPLE7PASS",
    };
    return st;
}

static void render_whole(uint8_t *buf, const app_wx_forecast_t *f, const app_ui_state_t *st)
{
    app_gfx_surface_t s = { .pixels = buf, .y0 = 0, .rows = APP_GFX_HEIGHT };
    app_ui_draw(&s, f, st);
}

/* Walks the panel the way app_display will, clamping the last band to what is left.
 *
 * The clamp is the contract, not a convenience: a surface running past the last row is
 * rejected outright by app_gfx_surface_valid, so an unclamped final band would silently
 * draw nothing at all. app_display must do this. */
static void render_banded(uint8_t *buf, const app_wx_forecast_t *f,
                          const app_ui_state_t *st, uint16_t rows)
{
    for (uint16_t y0 = 0; y0 < APP_GFX_HEIGHT; y0 += rows) {
        uint16_t left = (uint16_t)(APP_GFX_HEIGHT - y0);
        app_gfx_surface_t band = {
            .pixels = buf + (size_t)y0 * APP_GFX_WIDTH,
            .y0 = y0,
            .rows = rows < left ? rows : left,
        };
        app_ui_draw(&band, f, st);
    }
}

static void check_bands_match(app_ui_mode_t mode, uint16_t rows, const char *what)
{
    uint8_t *whole = malloc(WHOLE);
    uint8_t *banded = malloc(WHOLE);
    if (whole == NULL || banded == NULL) {
        CHECK(false, "allocation failed");
        free(whole); free(banded); return;
    }
    app_wx_forecast_t f;
    sample_forecast(&f);
    app_ui_state_t st = sample_state(mode);

    render_whole(whole, &f, &st);
    render_banded(banded, &f, &st, rows);

    int same = memcmp(whole, banded, WHOLE) == 0;
    CHECK(same, "%s: %u-row bands differ from the whole frame", what, (unsigned)rows);
    if (!same) {
        for (size_t i = 0; i < WHOLE; i++) {
            if (whole[i] != banded[i]) {
                printf("        first difference at x=%zu y=%zu: whole=%u banded=%u\n",
                       i % APP_GFX_WIDTH, i / APP_GFX_WIDTH, whole[i], banded[i]);
                break;
            }
        }
    }
    free(whole);
    free(banded);
}

static void test_layout_bands_match_whole_frame(void)
{
    check_bands_match(APP_UI_OK, 32, "forecast");
    check_bands_match(APP_UI_STALE, 32, "stale");
    check_bands_match(APP_UI_NO_FORECAST, 32, "no forecast");
    check_bands_match(APP_UI_UNPROVISIONED, 32, "unprovisioned");
}

static void test_layout_survives_an_awkward_band_height(void)
{
    /* 448 is 2^6 x 7, so 28, 32 and 64 all divide it evenly and none of them exercises a
     * short final band. 30 and 100 do: the last band is 28 rows and 48 rows
     * respectively, and an implementation that assumed a fixed height would either draw
     * past the panel or drop the footer entirely. */
    check_bands_match(APP_UI_OK, 30, "forecast, 30-row bands");
    check_bands_match(APP_UI_OK, 100, "forecast, 100-row bands");
    check_bands_match(APP_UI_OK, 1, "forecast, one row at a time");
}

static void test_draws_nothing_without_a_state(void)
{
    uint8_t band[APP_GFX_WIDTH * 4];
    memset(band, 0xAA, sizeof(band));
    app_gfx_surface_t s = { .pixels = band, .y0 = 0, .rows = 4 };
    app_wx_forecast_t f;
    sample_forecast(&f);

    app_ui_draw(&s, &f, NULL);

    for (size_t i = 0; i < sizeof(band); i++) {
        if (band[i] != 0xAA) {
            CHECK(false, "a NULL state still drew at byte %zu", i);
            return;
        }
    }
    CHECK(true, "unreachable");
}

static void test_an_empty_forecast_shows_the_waiting_screen(void)
{
    /* An empty forecast must not fall through to the chart code, which would divide by a
     * zero range and label axes with uninitialised numbers. */
    uint8_t *a = malloc(WHOLE);
    uint8_t *b = malloc(WHOLE);
    if (a == NULL || b == NULL) { CHECK(false, "allocation failed"); free(a); free(b); return; }

    app_wx_forecast_t empty;
    memset(&empty, 0, sizeof(empty));
    app_ui_state_t ok = sample_state(APP_UI_OK);
    app_ui_state_t none = sample_state(APP_UI_NO_FORECAST);

    render_whole(a, &empty, &ok);      /* claims OK but has no hours */
    render_whole(b, NULL, &none);      /* explicitly the waiting screen */

    CHECK(memcmp(a, b, WHOLE) == 0,
          "a zero-count forecast did not fall back to the waiting screen");
    free(a);
    free(b);
}

static void test_temp_range_over_the_forecast(void)
{
    app_wx_forecast_t f;
    sample_forecast(&f);
    float lo = 0.0f, hi = 0.0f;

    CHECK(app_wx_temp_range(&f, 0, &lo, &hi), "range over a full forecast failed");
    CHECK_INT((int)(lo * 10), 175);
    CHECK_INT((int)(hi * 10), 199);

    /* From hour 1, which is what the chart draws. The sample's warmest hour is hour 0 at
     * 19.9, so the two answers differ -- & a chart scaled to 19.9 would annotate its axis
     * with a temperature no plotted point reaches. */
    CHECK(app_wx_temp_range(&f, 1, &lo, &hi), "range from hour 1 failed");
    CHECK_INT((int)(lo * 10), 175);
    CHECK_INT((int)(hi * 10), 184);

    /* Past the end reduces over nothing rather than reading off the array. */
    CHECK(!app_wx_temp_range(&f, 13, &lo, &hi), "a first past the end reported a range");
    CHECK(!app_wx_temp_range(&f, 200, &lo, &hi), "a first well past the end reported a range");
}

static void test_temp_range_of_an_empty_forecast_is_refused(void)
{
    app_wx_forecast_t empty;
    memset(&empty, 0, sizeof(empty));
    float lo = -999.0f, hi = -999.0f;

    CHECK(!app_wx_temp_range(&empty, 0, &lo, &hi), "empty forecast reported a range");
    CHECK_INT((int)lo, -999);   /* outputs untouched, so a careless caller cannot print rubbish */
    CHECK_INT((int)hi, -999);
    CHECK(!app_wx_temp_range(NULL, 0, &lo, &hi), "NULL forecast reported a range");
}

static void test_series_maxima(void)
{
    app_wx_forecast_t f;
    sample_forecast(&f);
    CHECK_INT((int)(app_wx_precip_max(&f, 0) * 10), 14);
    CHECK_INT(app_wx_solar_max(&f, 0), 66);

    /* The sample puts its sunlight peak at hour 0, which no bar represents: scaling the
     * chart from it flattens every bar that is drawn. From hour 1 the chart scales to the
     * tallest bar it actually has. */
    CHECK_INT(app_wx_solar_max(&f, 1), 13);
    CHECK_INT((int)(app_wx_precip_max(&f, 1) * 10), 14);

    app_wx_forecast_t empty;
    memset(&empty, 0, sizeof(empty));
    CHECK_INT((int)app_wx_precip_max(&empty, 0), 0);
    CHECK_INT(app_wx_solar_max(&empty, 0), 0);
    CHECK_INT((int)app_wx_precip_max(NULL, 0), 0);
    CHECK_INT(app_wx_solar_max(NULL, 0), 0);
    CHECK_INT(app_wx_solar_max(&f, 99), 0);
}

/* Rebasing is what lets an old forecast still be drawn, so the thing to prove is that it
 * never leaves a label that has stopped being true. */
static void test_rebase_shifts_onto_the_current_hour(void)
{
    app_wx_forecast_t f;

    /* Already the current hour: left exactly alone, not shifted by zero and rewritten. */
    sample_forecast(&f);
    CHECK(app_wx_rebase(&f, f.hour[0].t), "a forecast starting now was refused");
    CHECK_INT(f.count, 13);
    CHECK_INT(f.hour[0].t, 1000);

    /* Part way through that hour is still that hour. */
    sample_forecast(&f);
    CHECK(app_wx_rebase(&f, f.hour[0].t + 3599), "still inside hour 0");
    CHECK_INT(f.count, 13);
    CHECK_INT(f.hour[0].t, 1000);

    /* Two hours on: two columns gone, and hour[0] is what hour[2] was -- temperature
     * carried with the timestamp, because a shift that moved one and not the other would
     * draw the right label over the wrong number. */
    sample_forecast(&f);
    const time_t t2 = f.hour[2].t;
    const float  c2 = f.hour[2].temp_c;
    CHECK(app_wx_rebase(&f, f.hour[0].t + 2 * 3600), "two hours on was refused");
    CHECK_INT(f.count, 11);
    CHECK_INT(f.hour[0].t, t2);
    CHECK_INT((int)(f.hour[0].temp_c * 10), (int)(c2 * 10));

    /* The sun times are looked up by date, so they must not shift with the hours. */
    app_wx_forecast_t before;
    sample_forecast(&before);
    sample_forecast(&f);
    CHECK(app_wx_rebase(&f, f.hour[0].t + 5 * 3600), "five hours on was refused");
    CHECK_INT(f.day_count, before.day_count);
    CHECK_INT(f.day[0].sunrise, before.day[0].sunrise);
    CHECK_INT(f.day[1].sunset, before.day[1].sunset);

    /* Past the end there is nothing true left to draw, and the forecast is untouched so
     * a caller that ignores the result cannot render an empty one. */
    sample_forecast(&f);
    CHECK(!app_wx_rebase(&f, f.hour[0].t + 13 * 3600), "an exhausted forecast was accepted");
    CHECK_INT(f.count, 13);

    /* A forecast from the future is refused rather than shifted: the clocks disagree, and
     * no amount of shifting makes the labels true. */
    sample_forecast(&f);
    CHECK(!app_wx_rebase(&f, f.hour[0].t - 60), "a future forecast was accepted");
    CHECK_INT(f.count, 13);

    CHECK(!app_wx_rebase(NULL, 1000), "NULL was accepted");
}

/* The stale rows of PLAN 4c only exist if an aged forecast survives rebasing, which is
 * the bug this pair guards: refusing it made every mode below OK unreachable. */
static void test_an_aged_forecast_still_draws_stale(void)
{
    app_wx_forecast_t f;
    sample_forecast(&f);
    const int64_t age = 2 * 3600;
    CHECK(app_wx_rebase(&f, f.hour[0].t + age), "a two-hour-old forecast was refused");
    CHECK_INT(app_ui_choose_mode(true, true, age), APP_UI_STALE);
    CHECK_INT(app_ui_choose_mode(true, true, 4 * 3600), APP_UI_STALE_OLD);
}

void test_app_ui(void)
{
    SUITE("app_ui");
    test_rebase_shifts_onto_the_current_hour();
    test_an_aged_forecast_still_draws_stale();
    test_layout_bands_match_whole_frame();
    test_layout_survives_an_awkward_band_height();
    test_draws_nothing_without_a_state();
    test_an_empty_forecast_shows_the_waiting_screen();
    test_temp_range_over_the_forecast();
    test_temp_range_of_an_empty_forecast_is_refused();
    test_series_maxima();
}
