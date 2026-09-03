/* app_gfx: clipping, packing, and the band seam.
 *
 * The seam test is the reason this file exists. Everything else here is cheap
 * insurance; that one is the claim the whole banded design rests on (PLAN 5a).
 */
#include "test.h"
#include "app_gfx.h"
#include <stdlib.h>
#include <string.h>

#define WHOLE_PIXELS ((size_t)APP_GFX_WIDTH * APP_GFX_HEIGHT)

/* One drawing, used by both halves of the seam test. Deliberately full of shapes that
 * straddle a 32-row boundary: 448/32 = 14 bands, so rows 32, 64, 96 ... are the seams.
 *
 * No randomness and no loop-computed expectations -- the drawing is fixed, and the
 * assertion is that two ways of rendering it agree. */
static void draw_scene(const app_gfx_surface_t *s)
{
    app_gfx_clear(s, APP_GFX_WHITE);
    app_gfx_fill_rect(s, 10, 20, 100, 40, APP_GFX_RED);          /* crosses row 32 */
    app_gfx_rect(s, 200, 60, 150, 80, APP_GFX_BLUE);             /* crosses 64, 96, 128 */
    app_gfx_hline(s, 0, APP_GFX_WIDTH - 1, 96, APP_GFX_GREEN);   /* sits on a seam */
    app_gfx_vline(s, 300, 0, APP_GFX_HEIGHT - 1, APP_GFX_BLACK); /* crosses every seam */
    app_gfx_line(s, 0, 0, APP_GFX_WIDTH - 1, APP_GFX_HEIGHT - 1, APP_GFX_ORANGE);
    app_gfx_line(s, APP_GFX_WIDTH - 1, 0, 0, APP_GFX_HEIGHT - 1, APP_GFX_YELLOW);
    app_gfx_fill_rect(s, 580, 430, 40, 40, APP_GFX_GREEN);       /* runs off two edges */
    app_gfx_fill_rect(s, -20, -10, 50, 50, APP_GFX_BLUE);        /* starts off-panel */

    /* Text is in the scene because it is the primitive most likely to break at a seam:
     * a 48 px glyph is taller than a 32-row band, so every one of them spans at least
     * two bands and some span three. */
    app_gfx_text(s, 20, 60, &app_gfx_font_big, "12.5\u00b0C", APP_GFX_BLACK);
    app_gfx_text(s, 20, 96, &app_gfx_font_label, "Dublin", APP_GFX_RED);
    app_gfx_text(s, 400, 447, &app_gfx_font_small, "v0.1.0", APP_GFX_BLUE);
}

static void test_band_seam_matches_whole_frame(void)
{
    uint8_t *whole = malloc(WHOLE_PIXELS);
    uint8_t *banded = malloc(WHOLE_PIXELS);
    CHECK(whole && banded, "allocation failed");
    if (!whole || !banded) {
        free(whole); free(banded); return;
    }
    memset(whole, 0xAA, WHOLE_PIXELS);
    memset(banded, 0xAA, WHOLE_PIXELS);

    app_gfx_surface_t full = { .pixels = whole, .y0 = 0, .rows = APP_GFX_HEIGHT };
    draw_scene(&full);

    const uint16_t band_rows = 32;
    for (uint16_t y0 = 0; y0 < APP_GFX_HEIGHT; y0 += band_rows) {
        app_gfx_surface_t band = {
            .pixels = banded + (size_t)y0 * APP_GFX_WIDTH,
            .y0 = y0,
            .rows = band_rows,
        };
        draw_scene(&band);
    }

    CHECK(memcmp(whole, banded, WHOLE_PIXELS) == 0,
          "banded render differs from whole-frame render");

    /* When it does differ, say where -- "not equal" over 268 800 bytes is not a bug
     * report. */
    for (size_t i = 0; i < WHOLE_PIXELS; i++) {
        if (whole[i] != banded[i]) {
            printf("        first difference at x=%zu y=%zu: whole=%u banded=%u\n",
                   i % APP_GFX_WIDTH, i / APP_GFX_WIDTH, whole[i], banded[i]);
            break;
        }
    }
    free(whole);
    free(banded);
}

static void test_packs_left_pixel_into_high_nibble(void)
{
    uint8_t row[APP_GFX_WIDTH];
    app_gfx_surface_t s = { .pixels = row, .y0 = 0, .rows = 1 };
    app_gfx_clear(&s, APP_GFX_WHITE);
    app_gfx_pixel(&s, 0, 0, APP_GFX_BLACK);   /* 0 */
    app_gfx_pixel(&s, 1, 0, APP_GFX_ORANGE);  /* 6 */

    uint8_t out[APP_GFX_WIDTH / 2];
    size_t n = app_gfx_pack(&s, out, sizeof(out));

    CHECK_INT(n, APP_GFX_WIDTH / 2);
    CHECK_INT(out[0], 0x06);   /* left pixel high, right pixel low */
    CHECK_INT(out[1], 0x11);   /* two white pixels */
}

static void test_pack_refuses_a_short_buffer(void)
{
    uint8_t row[APP_GFX_WIDTH];
    app_gfx_surface_t s = { .pixels = row, .y0 = 0, .rows = 1 };
    app_gfx_clear(&s, APP_GFX_WHITE);

    uint8_t out[8];
    CHECK_INT(app_gfx_pack(&s, out, sizeof(out)), 0);
}

static void test_rejects_a_surface_off_the_panel(void)
{
    uint8_t row[APP_GFX_WIDTH];
    app_gfx_surface_t past_bottom = { .pixels = row, .y0 = APP_GFX_HEIGHT - 1, .rows = 8 };
    app_gfx_surface_t no_rows     = { .pixels = row, .y0 = 0, .rows = 0 };
    app_gfx_surface_t no_pixels   = { .pixels = NULL, .y0 = 0, .rows = 1 };

    CHECK(!app_gfx_surface_valid(&past_bottom), "surface running past the last row");
    CHECK(!app_gfx_surface_valid(&no_rows), "zero-row surface");
    CHECK(!app_gfx_surface_valid(&no_pixels), "surface with no pixels");
}

static void test_drawing_outside_the_band_touches_nothing(void)
{
    uint8_t band[APP_GFX_WIDTH * 4];
    app_gfx_surface_t s = { .pixels = band, .y0 = 100, .rows = 4 };
    app_gfx_clear(&s, APP_GFX_WHITE);

    app_gfx_fill_rect(&s, 0, 0, APP_GFX_WIDTH, 50, APP_GFX_RED);        /* all above */
    app_gfx_fill_rect(&s, 0, 200, APP_GFX_WIDTH, 50, APP_GFX_RED);      /* all below */
    app_gfx_pixel(&s, -1, 101, APP_GFX_RED);
    app_gfx_pixel(&s, APP_GFX_WIDTH, 101, APP_GFX_RED);

    for (size_t i = 0; i < sizeof(band); i++) {
        if (band[i] != APP_GFX_WHITE) {
            CHECK(false, "pixel %zu was written, expected untouched white", i);
            return;
        }
    }
    CHECK(true, "unreachable");
}

static void test_measures_text_without_a_surface(void)
{
    /* Right-aligning is most of PLAN 6's layout, so the width has to be correct without
     * drawing anything. */
    CHECK_INT(app_gfx_text_width(&app_gfx_font_label, ""), 0);
    CHECK(app_gfx_text_width(&app_gfx_font_label, "Dublin") > 0, "non-empty string measured zero");

    int one = app_gfx_text_width(&app_gfx_font_label, "8");
    int two = app_gfx_text_width(&app_gfx_font_label, "88");
    CHECK_INT(two, one * 2);
}

static void test_measures_a_degree_sign_as_one_glyph(void)
{
    /* U+00B0 is two bytes in UTF-8. If the decoder were byte-oriented it would either
     * measure two glyphs or fall over, and "21 C" is not what the header should say. */
    int with = app_gfx_text_width(&app_gfx_font_big, "1\u00b0");
    int without = app_gfx_text_width(&app_gfx_font_big, "1");
    int degree = app_gfx_text_width(&app_gfx_font_big, "\u00b0");

    CHECK(degree > 0, "degree sign measured zero -- is it in the font?");
    CHECK_INT(with, without + degree);
}

static void test_skips_characters_the_font_lacks(void)
{
    /* font_big carries digits, a minus, a full stop, a degree sign and a capital C. A
     * letter it does not have must contribute nothing rather than drawing a stray
     * glyph -- a mystery character on a weather display is worse than a missing one. */
    CHECK_INT(app_gfx_text_width(&app_gfx_font_big, "Q"), 0);
    CHECK_INT(app_gfx_text_width(&app_gfx_font_big, "1Q2"),
              app_gfx_text_width(&app_gfx_font_big, "12"));
}

static void test_text_baseline_sits_where_asked(void)
{
    /* The bearing convention is the thing that was wrong first time round: Pillow
     * measures from the top of the line box, the device draws from the baseline.
     *
     * The tolerance is not slop. Round glyphs -- C, O, S -- are cut slightly below the
     * baseline on purpose so they look optically level beside flat-bottomed ones, and
     * DejaVu's C overshoots by one pixel at 48 px. A test demanding bottom < baseline
     * fails on correct output, which is how this one was written first. */
    static uint8_t px[APP_GFX_WIDTH * 80];
    app_gfx_surface_t s = { .pixels = px, .y0 = 0, .rows = 80 };
    app_gfx_clear(&s, APP_GFX_WHITE);
    app_gfx_text(&s, 10, 60, &app_gfx_font_big, "C", APP_GFX_BLACK);

    int top = -1, bottom = -1;
    for (int y = 0; y < 80; y++) {
        for (int x = 0; x < APP_GFX_WIDTH; x++) {
            if (px[y * APP_GFX_WIDTH + x] != APP_GFX_WHITE) {
                if (top < 0) { top = y; }
                bottom = y;
                break;
            }
        }
    }
    CHECK(top >= 0, "nothing was drawn");
    CHECK(bottom <= 61, "glyph fell well below its baseline (bottom=%d)", bottom);
    CHECK(bottom >= 58, "glyph floated above its baseline (bottom=%d)", bottom);
    CHECK(top > 60 - app_gfx_font_big.line_height,
          "glyph starts above one line height from the baseline (top=%d)", top);
}

void test_app_gfx(void)
{
    SUITE("app_gfx");
    test_band_seam_matches_whole_frame();
    test_packs_left_pixel_into_high_nibble();
    test_pack_refuses_a_short_buffer();
    test_rejects_a_surface_off_the_panel();
    test_drawing_outside_the_band_touches_nothing();
    test_measures_text_without_a_surface();
    test_measures_a_degree_sign_as_one_glyph();
    test_skips_characters_the_font_lacks();
    test_text_baseline_sits_where_asked();
}
