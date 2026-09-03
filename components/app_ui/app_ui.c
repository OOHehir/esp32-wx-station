/* PLAN 6's layout. See app_ui.h for why none of this does any I/O. */
#include "app_ui.h"

#include <stdio.h>
#include <string.h>

/* Band boundaries, from PLAN 6. They close exactly on 448 rows; a change here that does
 * not is a layout that silently overlaps rather than one that fails to build. */
#define HEADER_TOP      0
#define TEMP_TOP        80
#define RAIN_TOP        200
#define SUN_TOP         320
#define FOOTER_TOP      420
#define PANEL_BOTTOM    APP_GFX_HEIGHT

#define GUTTER          48                 /* axis labels live left of this */
#define PLOT_LEFT       GUTTER
#define COLUMNS         12

/* The charts start at hour 1: hour 0 is the hour in progress & is the big number in the
 * header instead. Everything that scales a chart has to agree with that, or the axis is
 * annotated with a value the chart does not contain. */
#define FIRST_COLUMN_HOUR 1
#define COL_W           45
#define PLOT_RIGHT      (PLOT_LEFT + COLUMNS * COL_W)   /* 588 */

/* Inset from a chart's top and bottom so a bar at full scale does not touch the rule
 * above it. */
#define CHART_PAD       14

/* The header's two outer columns, in the 16 px label face rather than the 12 px one:
 * these are read across a room, which is the distance the panel is actually looked at
 * from, and the small face is for axis ticks.
 *
 * Three baselines at 24, 46 and 68. The third is the stale marker, so the right column
 * runs High / Low / stale; 68 plus the face's four rows of descender is 72, clear of the
 * rule at TEMP_TOP - 1. HEADER_LINE_H is a little more than the face's own line_height
 * of 19, which is what stops two stacked lines reading as one block. */
#define HEADER_COL_X    8
#define HEADER_LINE_1   24
#define HEADER_LINE_H   22

/* The middle column: the temperature now, and what it feels like under it.
 *
 * A baseline of 46 puts the big face's ink between rows 10 and 46 -- its tallest glyph
 * stands 36 rows above the baseline -- which is where the place name used to be. The name
 * is gone rather than moved: it answers a question nobody standing in their own hall is
 * asking, and it was taking the top of the panel to do it. What it feels like is a
 * question they are asking, because it is the difference between needing a coat and not.
 *
 * The feels-like line takes the outer columns' third baseline rather than one of its own,
 * so all three columns close on the same row, well clear of the rule at TEMP_TOP - 1. It
 * is set in the label face: two numbers in the 48 px face would be two headlines arguing
 * over which is the reading, and that face carries no letters to set "Feels like" with
 * anyway (see tools/mkfont.py). */
#define HEADER_TEMP_BASE   46
#define HEADER_FEELS_LINE  2

static int col_x(int i)
{
    return PLOT_LEFT + i * COL_W;
}

static void text_right(const app_gfx_surface_t *s, int right, int baseline,
                       const app_gfx_font_t *font, const char *str, app_gfx_colour_t c)
{
    app_gfx_text(s, right - app_gfx_text_width(font, str), baseline, font, str, c);
}

static void text_centre(const app_gfx_surface_t *s, int centre, int baseline,
                        const app_gfx_font_t *font, const char *str, app_gfx_colour_t c)
{
    app_gfx_text(s, centre - app_gfx_text_width(font, str) / 2, baseline, font, str, c);
}

/* Local wall-clock hour for an entry, as two digits.
 *
 * Deliberately arithmetic rather than localtime(): the device has no TZ database, and
 * Open-Meteo already answered the question by returning utc_offset_seconds (PLAN 4a).
 * app_wx_civil_local does that arithmetic once, for here and for the two timestamps
 * below, so an hour label and a date label cannot disagree about what local time is. */
static int local_hour(const app_wx_forecast_t *f, uint8_t i)
{
    app_wx_civil_t c;
    app_wx_civil_local(f->hour[i].t, f->utc_offset_s, &c);
    return c.hour;
}

/* "HH:MM" for a UTC instant, in the forecast's local zone. `t` of 0 means the event does
 * not exist -- a polar day has no sunrise -- and prints as blanks rather than as the
 * perfectly plausible 00:00 it would otherwise become. */
static void local_hhmm(char *buf, size_t len, time_t t, int32_t utc_offset_s)
{
    if (t == 0) {
        snprintf(buf, len, "--:--");
        return;
    }
    app_wx_civil_t c;
    app_wx_civil_local(t, utc_offset_s, &c);
    snprintf(buf, len, "%02u:%02u", c.hour, c.min);
}

/* Night as one strip above the hour labels, rather than a wash over the charts.
 *
 * The wash was tried first -- every fourth pixel across all three charts -- and it
 * dominated the image: eleven of twelve columns are night in this forecast, so almost
 * the whole panel became texture and the traces had to be hunted for. A strip says the
 * same thing in one row of ink and leaves the data alone. */
static void draw_day_night_strip(const app_gfx_surface_t *s, const app_wx_forecast_t *f)
{
    const int y = FOOTER_TOP - 16;
    for (uint8_t i = 1; i <= COLUMNS && i < f->count; i++) {
        app_gfx_fill_rect(s, col_x(i - 1), y, COL_W, 5,
                          f->hour[i].is_day ? APP_GFX_YELLOW : APP_GFX_BLUE);
    }
    app_gfx_rect(s, PLOT_LEFT, y, COLUMNS * COL_W, 5, APP_GFX_BLACK);
}

static void draw_hour_axis(const app_gfx_surface_t *s, const app_wx_forecast_t *f)
{
    char buf[8];
    for (uint8_t i = 1; i <= COLUMNS && i < f->count; i++) {
        snprintf(buf, sizeof(buf), "%02d", local_hour(f, i));
        text_centre(s, col_x(i - 1) + COL_W / 2, FOOTER_TOP - 3,
                    &app_gfx_font_small, buf, APP_GFX_BLACK);
    }
}

/* One line of one of the header's outer columns, by index rather than by pixel, so the
 * two columns cannot drift apart and a gap left by a missing sun time stays a gap rather
 * than shuffling the line below it up into the wrong meaning. */
static int header_baseline(int line)
{
    return HEADER_LINE_1 + line * HEADER_LINE_H;
}

static void header_left(const app_gfx_surface_t *s, int line, const char *str)
{
    app_gfx_text(s, HEADER_COL_X, header_baseline(line), &app_gfx_font_label, str,
                 APP_GFX_BLACK);
}

static void header_right(const app_gfx_surface_t *s, int line, const char *str,
                         app_gfx_colour_t c)
{
    text_right(s, APP_GFX_WIDTH - HEADER_COL_X, header_baseline(line),
               &app_gfx_font_label, str, c);
}

static void draw_header(const app_gfx_surface_t *s, const app_wx_forecast_t *f,
                        const app_ui_state_t *st)
{
    char buf[32];

    /* The temperature down the middle with the feels-like under it, today's four
     * supporting numbers in a column at each edge: the sun on the left, the range on the
     * right.
     *
     * The reading anybody crossing the room wants is the big one, and centring it gives
     * it the whole width to sit in rather than the space left over beside a label. Two
     * short columns rather than one of four keeps each pair together -- two times and two
     * temperatures -- and leaves the middle clear for the two numbers that matter. */
    if (f != NULL && f->count > 0) {
        snprintf(buf, sizeof(buf), "%.1f°C", (double)f->hour[0].temp_c);
        text_centre(s, APP_GFX_WIDTH / 2, HEADER_TEMP_BASE, &app_gfx_font_big, buf,
                    APP_GFX_RED);

        /* Drawn whether or not it differs from the temperature above. A line that appears
         * only when the two disagree makes its own absence mean something, and "no
         * feels-like today" is not a thing the forecast ever says -- a still, dry 15 C
         * that feels like 15 C is an answer, and one worth showing. */
        snprintf(buf, sizeof(buf), "Feels like %.1f°C", (double)f->hour[0].feels_c);
        text_centre(s, APP_GFX_WIDTH / 2, header_baseline(HEADER_FEELS_LINE),
                    &app_gfx_font_label, buf, APP_GFX_BLACK);

        time_t rise = 0, set = 0;
        if (app_wx_sun_today(f, &rise, &set)) {
            char hhmm[8];
            local_hhmm(hhmm, sizeof(hhmm), rise, f->utc_offset_s);
            snprintf(buf, sizeof(buf), "Sunrise %s", hhmm);
            header_left(s, 0, buf);
            local_hhmm(hhmm, sizeof(hhmm), set, f->utc_offset_s);
            snprintf(buf, sizeof(buf), "Sunset %s", hhmm);
            header_left(s, 1, buf);
        }

        float lo = 0.0f, hi = 0.0f;
        if (app_wx_temp_range(f, 0, &lo, &hi)) {
            snprintf(buf, sizeof(buf), "High %.0f", (double)hi);
            header_right(s, 0, buf, APP_GFX_BLACK);
            snprintf(buf, sizeof(buf), "Low %.0f", (double)lo);
            header_right(s, 1, buf, APP_GFX_BLACK);
        }
    }

    /* The stale marker takes the third line of the right column, under the range. It used
     * to sit at a pixel row of its own; with a column there now, a marker that ignored it
     * would land on top of "Low". */
    if (st->mode == APP_UI_STALE || st->mode == APP_UI_STALE_OLD) {
        long age_min = f != NULL && f->fetched_at != 0
                     ? (long)((st->now - f->fetched_at) / 60) : 0;
        snprintf(buf, sizeof(buf), "stale %ldm", age_min);
        header_right(s, 2, buf,
                     st->mode == APP_UI_STALE_OLD ? APP_GFX_RED : APP_GFX_ORANGE);
    }

    app_gfx_hline(s, 0, APP_GFX_WIDTH - 1, TEMP_TOP - 1, APP_GFX_BLACK);
}

/* A rule and a caption at the top of a chart band. Without them the three charts abut
 * and read as one tall plot with a confusing mixture of marks. */
static void chart_heading(const app_gfx_surface_t *s, int top, const char *title)
{
    app_gfx_hline(s, 0, APP_GFX_WIDTH - 1, top, APP_GFX_BLACK);
    app_gfx_text(s, 4, top + 12, &app_gfx_font_small, title, APP_GFX_BLACK);
}

static void draw_temperature(const app_gfx_surface_t *s, const app_wx_forecast_t *f)
{
    float lo = 0.0f, hi = 0.0f;
    if (!app_wx_temp_range(f, FIRST_COLUMN_HOUR, &lo, &hi)) {
        return;
    }
    /* A flat forecast would divide by zero and, worse, would draw a trace pinned to one
     * edge. One degree of headroom either side keeps a still day looking still. */
    if (hi - lo < 1.0f) {
        lo -= 1.0f;
        hi += 1.0f;
    }

    const int top = TEMP_TOP + CHART_PAD + 4;
    const int bottom = RAIN_TOP - 6;

    chart_heading(s, TEMP_TOP, "TEMPERATURE  °C");

    char buf[12];
    snprintf(buf, sizeof(buf), "%.0f", (double)hi);
    text_right(s, GUTTER - 6, top + 10, &app_gfx_font_small, buf, APP_GFX_BLACK);
    snprintf(buf, sizeof(buf), "%.0f", (double)lo);
    text_right(s, GUTTER - 6, bottom, &app_gfx_font_small, buf, APP_GFX_BLACK);

    /* Plotted on the same column centres as the bars below, so all three charts share one
     * x grid and the hour labels apply to every one of them. Plotting hour 0 as well put
     * the first point inside the gutter, on top of the axis label. Hour 0 is the big
     * number in the header; the charts are the twelve hours ahead. */
    int prev_x = 0, prev_y = 0;
    for (uint8_t i = 1; i <= COLUMNS && i < f->count; i++) {
        int x = col_x(i - 1) + COL_W / 2;
        float frac = (f->hour[i].temp_c - lo) / (hi - lo);
        int y = bottom - (int)(frac * (float)(bottom - top));
        if (i > 1) {
            /* Two-pixel trace: one line is too thin to read across a room, which is the
             * distance this display is actually looked at from. */
            app_gfx_line(s, prev_x, prev_y, x, y, APP_GFX_RED);
            app_gfx_line(s, prev_x, prev_y + 1, x, y + 1, APP_GFX_RED);
        }
        prev_x = x;
        prev_y = y;
    }
}

static void draw_rainfall(const app_gfx_surface_t *s, const app_wx_forecast_t *f)
{
    const int top = RAIN_TOP + CHART_PAD + 4;
    const int bottom = SUN_TOP - 6;
    const int height = bottom - top;

    float max = app_wx_precip_max(f, FIRST_COLUMN_HOUR);
    /* Scale to at least 1 mm so a drizzle of 0.1 mm does not draw a full-height bar and
     * read as a downpour. */
    float scale = max < 1.0f ? 1.0f : max;

    chart_heading(s, RAIN_TOP, "RAIN  mm filled / chance outlined");

    char buf[12];
    /* Units live in the caption, not the axis: "1.4mm" at 12 px does not fit the gutter
     * and was clipped against the left edge. */
    snprintf(buf, sizeof(buf), "%.1f", (double)scale);
    text_right(s, GUTTER - 6, top + 5, &app_gfx_font_small, buf, APP_GFX_BLACK);

    for (uint8_t i = 1; i <= COLUMNS && i < f->count; i++) {
        int x = col_x(i - 1) + 6;
        int w = COL_W - 12;

        /* Probability is an outline, the amount is a solid bar. Filled, the two competed
         * -- a 5 % chance of no rain drew a solid green block that read as the primary
         * series. An outline is unmistakably the secondary one. */
        int ph = (int)((float)f->hour[i].precip_pct / 100.0f * (float)height);
        if (ph > 1) {
            app_gfx_rect(s, x, bottom - ph, w, ph, APP_GFX_GREEN);
        }

        int bh = (int)(f->hour[i].precip_mm / scale * (float)height);
        if (bh > 0) {
            app_gfx_fill_rect(s, x + 5, bottom - bh, w - 10, bh, APP_GFX_BLUE);
        }
    }
    app_gfx_hline(s, GUTTER, PLOT_RIGHT, bottom, APP_GFX_BLACK);
}

static void draw_sunlight(const app_gfx_surface_t *s, const app_wx_forecast_t *f)
{
    const int top = SUN_TOP + CHART_PAD + 4;
    const int bottom = FOOTER_TOP - 20;   /* room for the day/night strip and labels */
    const int height = bottom - top;

    int16_t max = app_wx_solar_max(f, FIRST_COLUMN_HOUR);
    int16_t scale = max < 50 ? 50 : max;

    chart_heading(s, SUN_TOP, "SUN  W/m²");

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)scale);
    text_right(s, GUTTER - 6, top + 5, &app_gfx_font_small, buf, APP_GFX_BLACK);

    /* The peak hour is drawn orange rather than yellow. Yellow on this panel is pale
     * enough that a chart of nothing but yellow bars has no focal point. */
    /* Seeded on the first drawn column, not on hour 0: hour 0 is the header's number and
     * has no bar, so a peak found there matches no `i` below and the highlight silently
     * disappears -- which is exactly what a sunny midday fetch produced. */
    uint8_t peak = FIRST_COLUMN_HOUR;
    for (uint8_t i = FIRST_COLUMN_HOUR; i <= COLUMNS && i < f->count; i++) {
        if (f->hour[i].solar_wm2 >= f->hour[peak].solar_wm2) {
            peak = i;
        }
    }

    for (uint8_t i = 1; i <= COLUMNS && i < f->count; i++) {
        int h = (int)((float)f->hour[i].solar_wm2 / (float)scale * (float)height);
        if (h <= 0) {
            continue;
        }
        app_gfx_fill_rect(s, col_x(i - 1) + 6, bottom - h, COL_W - 12, h,
                          (i == peak && max > 0) ? APP_GFX_ORANGE : APP_GFX_YELLOW);
    }
    app_gfx_hline(s, GUTTER, PLOT_RIGHT, bottom, APP_GFX_BLACK);
}

static void draw_footer(const app_gfx_surface_t *s, const app_wx_forecast_t *f,
                        const app_ui_state_t *st)
{
    char buf[64];
    app_gfx_hline(s, 0, APP_GFX_WIDTH - 1, FOOTER_TOP, APP_GFX_BLACK);

    snprintf(buf, sizeof(buf), "%s  %s",
             st->version != NULL ? st->version : "",
             st->ip != NULL && st->ip[0] != '\0' ? st->ip : "offline");
    app_gfx_text(s, 8, PANEL_BOTTOM - 8, &app_gfx_font_small, buf, APP_GFX_BLACK);

    /* When the numbers above were fetched, in words rather than as an age.
     *
     * "stale 47m" in the header answers "should I trust this"; it does not answer "did my
     * pressing the button do anything", which is the question somebody standing at the
     * panel is actually asking. A wall-clock time does, and it is the one part of the
     * frame that changes on every successful fetch -- so a manual refresh that finds an
     * unchanged forecast still visibly repaints instead of looking like a dead button.
     *
     * Omitted entirely when the fetch time is unknown: a forecast fetched before SNTP set
     * the clock has a fetched_at of nothing in particular, and a made-up date under
     * "Last updated" is worse than no line at all. The header already says stale. */
    if (f != NULL && f->fetched_at != 0) {
        app_wx_civil_t c;
        app_wx_civil_local(f->fetched_at, f->utc_offset_s, &c);
        snprintf(buf, sizeof(buf), "Last updated at %02u:%02u %u/%u/%d",
                 c.hour, c.min, c.day, c.mon, c.year);
        text_right(s, APP_GFX_WIDTH - 8, PANEL_BOTTOM - 8, &app_gfx_font_small, buf,
                   APP_GFX_BLACK);
    }
}

/* The two screens that are not a forecast. Both exist because a 600x448 panel telling
 * you what is wrong beats a serial log nobody is attached to (PLAN 4c). */
static void draw_no_forecast(const app_gfx_surface_t *s, const app_ui_state_t *st)
{
    app_gfx_clear(s, APP_GFX_WHITE);
    text_centre(s, APP_GFX_WIDTH / 2, 180, &app_gfx_font_big, "--°C", APP_GFX_BLACK);
    text_centre(s, APP_GFX_WIDTH / 2, 240, &app_gfx_font_label,
                "Waiting for first forecast", APP_GFX_BLACK);
    text_centre(s, APP_GFX_WIDTH / 2, 280, &app_gfx_font_label,
                st->net_state != NULL ? st->net_state : "", APP_GFX_RED);
    draw_footer(s, NULL, st);
}

static void draw_unprovisioned(const app_gfx_surface_t *s, const app_ui_state_t *st)
{
    app_gfx_clear(s, APP_GFX_WHITE);
    text_centre(s, APP_GFX_WIDTH / 2, 120, &app_gfx_font_label,
                "Join this network to set up the display", APP_GFX_BLACK);
    /* The SSID and passphrase are in the label face, not the big one: the 48 px face
     * carries only the characters a temperature needs, so it has no letters to set them
     * with (see tools/mkfont.py). */
    text_centre(s, APP_GFX_WIDTH / 2, 210, &app_gfx_font_label,
                st->ap_ssid != NULL ? st->ap_ssid : "", APP_GFX_RED);
    text_centre(s, APP_GFX_WIDTH / 2, 260, &app_gfx_font_label,
                st->ap_pass != NULL ? st->ap_pass : "", APP_GFX_BLUE);
    text_centre(s, APP_GFX_WIDTH / 2, 320, &app_gfx_font_label,
                "then open http://192.168.4.1/", APP_GFX_BLACK);
    draw_footer(s, NULL, st);
}

void app_ui_draw(const app_gfx_surface_t *surface,
                 const app_wx_forecast_t *forecast,
                 const app_ui_state_t *state)
{
    if (state == NULL) {
        return;
    }
    if (state->mode == APP_UI_UNPROVISIONED) {
        draw_unprovisioned(surface, state);
        return;
    }
    if (forecast == NULL || forecast->count == 0 || state->mode == APP_UI_NO_FORECAST) {
        draw_no_forecast(surface, state);
        return;
    }

    app_gfx_clear(surface, APP_GFX_WHITE);
    draw_header(surface, forecast, state);
    draw_temperature(surface, forecast);
    draw_rainfall(surface, forecast);
    draw_sunlight(surface, forecast);
    draw_day_night_strip(surface, forecast);
    draw_hour_axis(surface, forecast);
    draw_footer(surface, forecast, state);
}

app_ui_mode_t app_ui_choose_mode(bool provisioned, bool have_forecast, int64_t age_s)
{
    /* Order matters, and this is PLAN 4c's table read top to bottom.
     *
     * Unprovisioned wins over everything: a device with no credentials cannot refresh
     * whatever it is holding, and the screen that tells somebody how to fix that is worth
     * more than a forecast they cannot update. It is checked first even when a cached
     * forecast exists, which happens after a factory reset. */
    if (!provisioned) {
        return APP_UI_UNPROVISIONED;
    }
    if (!have_forecast) {
        return APP_UI_NO_FORECAST;
    }

    /* A negative age means a forecast whose fetch time is unknown -- fetched before SNTP
     * set the clock, or restored from a cache written then. It is shown, and shown as
     * stale: the data may be perfectly good, but nothing here can say so, and PLAN 4c's
     * rule is that the panel must never show a lie. */
    if (age_s < 0) {
        return APP_UI_STALE;
    }
    if (age_s >= APP_UI_STALE_AFTER_S) {
        return APP_UI_STALE_OLD;
    }
    /* Fresher than one refresh interval is simply current. Between that and three hours
     * the forecast is still drawn normally, with the footer carrying its age. */
    return age_s < 3600 ? APP_UI_OK : APP_UI_STALE;
}
