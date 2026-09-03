/* app_gfx -- drawing onto a horizontal band of the panel.
 *
 * No I/O of any kind: this component never touches SPI, GPIO or NVS, which is what lets
 * the whole of it, and the whole of app_ui above it, run under plain gcc on the build
 * machine (PLAN 5b).
 *
 * The band is the unusual part, and it is forced by the hardware. A whole 600x448 frame
 * is 268 800 bytes at one byte per pixel; the XIAO ESP32-C3's largest contiguous heap
 * pool is 156 KiB, measured on the bench board, and Wi-Fi with a TLS session is still to
 * come out of it. So nothing ever holds a whole frame. app_display walks the panel in
 * bands, and every primitive here clips to whichever band it is handed.
 *
 * The contract that makes this safe is that drawing is a pure function of the surface it
 * is given: rendering fourteen 32-row bands must produce exactly the bytes that rendering
 * one 448-row surface would. test_band_seam_matches_whole_frame asserts precisely that,
 * because a seam bug is invisible until a panel is in front of you and then costs a
 * 30-second refresh per attempt.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "app_gfx_fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Panel geometry. The one place it is written down; PLAN 8 notes that a move to the 7.3"
 * panel changes these two numbers and nothing else in this component. */
#define APP_GFX_WIDTH   600
#define APP_GFX_HEIGHT  448

/* The panel's seven inks, in the controller's own numbering.
 *
 * Not an arbitrary choice: Pimoroni's Python driver and Soldered's C++ driver were
 * written independently and agree on this order (PLAN 3d), so a buffer built here can be
 * diffed against output from either. Note that it is not universal -- one ESP-IDF
 * component numbers the same seven colours differently (PLAN 1d). */
typedef enum {
    APP_GFX_BLACK  = 0,
    APP_GFX_WHITE  = 1,
    APP_GFX_GREEN  = 2,
    APP_GFX_BLUE   = 3,
    APP_GFX_RED    = 4,
    APP_GFX_YELLOW = 5,
    APP_GFX_ORANGE = 6,
    APP_GFX_CLEAN  = 7,
} app_gfx_colour_t;

/* A window onto `rows` rows of the panel, starting at absolute row `y0`.
 *
 * Callers draw in absolute panel coordinates throughout -- a surface subtracts its own
 * y0. That is deliberate: it means app_ui's layout arithmetic never mentions bands, so
 * the same code renders one band or the whole panel and cannot disagree with itself. */
typedef struct {
    uint8_t *pixels;   /* rows * APP_GFX_WIDTH, one colour index per pixel */
    uint16_t y0;       /* absolute row of pixels[0] */
    uint16_t rows;
} app_gfx_surface_t;

/* True when `surface` is well formed: non-NULL pixels, non-zero rows, and entirely
 * within the panel. Every primitive is a no-op on a surface this rejects, so a caller
 * that gets the arithmetic wrong draws nothing rather than corrupting the heap. */
bool app_gfx_surface_valid(const app_gfx_surface_t *surface);

/* Fill the whole surface with one colour. */
void app_gfx_clear(const app_gfx_surface_t *surface, app_gfx_colour_t colour);

/* Set one pixel, in absolute panel coordinates. Outside the surface, does nothing. */
void app_gfx_pixel(const app_gfx_surface_t *surface, int x, int y, app_gfx_colour_t colour);

/* Filled rectangle, absolute coordinates, clipped to the surface. Negative or zero
 * width or height draws nothing. */
void app_gfx_fill_rect(const app_gfx_surface_t *surface, int x, int y, int w, int h,
                       app_gfx_colour_t colour);

/* One-pixel outline of the same rectangle. */
void app_gfx_rect(const app_gfx_surface_t *surface, int x, int y, int w, int h,
                  app_gfx_colour_t colour);

/* Axis-aligned lines. Endpoints are inclusive. */
void app_gfx_hline(const app_gfx_surface_t *surface, int x0, int x1, int y,
                   app_gfx_colour_t colour);
void app_gfx_vline(const app_gfx_surface_t *surface, int x, int y0, int y1,
                   app_gfx_colour_t colour);

/* Bresenham line, any angle. The temperature trace in PLAN 6 is the only caller that
 * needs it, and it is the primitive most likely to expose a band seam. */
void app_gfx_line(const app_gfx_surface_t *surface, int x0, int y0, int x1, int y1,
                  app_gfx_colour_t colour);

/* --- Text ---------------------------------------------------------------------------
 *
 * `x` is the pen position and `y` is the BASELINE, not the top of the text. Baselines
 * are what makes two different sizes on one row line up, and PLAN 6's header puts a
 * 48 px temperature beside 16 px labels.
 *
 * Text clips like every other primitive, so a line crossing a band boundary renders
 * correctly in both bands -- test_text_across_a_seam_matches_whole_frame asserts it.
 *
 * `text` is UTF-8. Only the degree sign needs more than one byte; an unrecognised or
 * malformed sequence is skipped rather than drawn as a replacement box, because a
 * mystery character on a weather display is worse than a missing one.
 */
void app_gfx_text(const app_gfx_surface_t *surface, int x, int baseline,
                  const app_gfx_font_t *font, const char *text, app_gfx_colour_t colour);

/* Width in pixels the same string would occupy. Needed for right-aligning and centring,
 * which is most of PLAN 6's layout. Does not draw, and needs no surface. */
int app_gfx_text_width(const app_gfx_font_t *font, const char *text);

/* Pack a surface into the panel's wire format: two pixels per byte, the left-hand pixel
 * of each pair in the high nibble (PLAN 3c).
 *
 * `out` must hold rows * APP_GFX_WIDTH / 2 bytes. Returns the number written, or 0 if
 * the surface is invalid. */
size_t app_gfx_pack(const app_gfx_surface_t *surface, uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif
