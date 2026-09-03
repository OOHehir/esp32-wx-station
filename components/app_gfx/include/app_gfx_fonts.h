/* Bitmap fonts for app_gfx, generated from a TTF by tools/mkfont.py.
 *
 * One bit per pixel: the panel has seven inks and no greys, so an antialiased glyph
 * would have to be thresholded somewhere. mkfont.py does it once, at build time, rather
 * than leaving every draw call to decide.
 *
 * The tables are committed rather than generated during the build -- a firmware build
 * should not need Pillow and FreeType on every machine that clones the repository, and
 * glyphs change only when somebody deliberately re-runs the tool.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One glyph. `offset` indexes the font's bitmap blob, where each row is padded to a
 * whole number of bytes; `left` and `top` are FreeType's bearings, relative to the pen
 * position and the baseline respectively. */
typedef struct {
    uint16_t codepoint;
    uint16_t offset;
    uint8_t  width;
    uint8_t  height;
    int8_t   left;
    int8_t   top;
    uint8_t  advance;
} app_gfx_glyph_t;

/* Glyphs are sorted by codepoint so a lookup can bisect. Sorted, not contiguous: the
 * degree sign is U+00B0 and everything else is ASCII, and a table with an eighty-glyph
 * hole in it to keep the index arithmetic simple would be worse than a bisection. */
typedef struct {
    const uint8_t         *bitmap;
    const app_gfx_glyph_t *glyphs;
    uint16_t               glyph_count;
    uint8_t                line_height;
    uint8_t                ascent;
} app_gfx_font_t;

/* PLAN 6's three sizes. `big` carries only the characters a temperature needs. */
extern const app_gfx_font_t app_gfx_font_big;    /* ~48 px, digits and a degree sign */
extern const app_gfx_font_t app_gfx_font_label;  /* ~16 px, full ASCII */
extern const app_gfx_font_t app_gfx_font_small;  /* ~12 px, full ASCII */

#ifdef __cplusplus
}
#endif
