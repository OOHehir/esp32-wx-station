#include "app_gfx.h"

/* Every primitive funnels through app_gfx_pixel, which is the only place that knows a
 * surface is a band. Clipping once, in one function, is what makes the band seam a
 * property of the design rather than of each caller remembering. */

bool app_gfx_surface_valid(const app_gfx_surface_t *surface)
{
    return surface != NULL
        && surface->pixels != NULL
        && surface->rows > 0
        && surface->y0 < APP_GFX_HEIGHT
        && (int)surface->y0 + (int)surface->rows <= APP_GFX_HEIGHT;
}

void app_gfx_pixel(const app_gfx_surface_t *surface, int x, int y, app_gfx_colour_t colour)
{
    if (!app_gfx_surface_valid(surface)) {
        return;
    }
    if (x < 0 || x >= APP_GFX_WIDTH) {
        return;
    }
    int row = y - (int)surface->y0;
    if (row < 0 || row >= (int)surface->rows) {
        return;
    }
    surface->pixels[(size_t)row * APP_GFX_WIDTH + (size_t)x] = (uint8_t)(colour & 0x07);
}

void app_gfx_clear(const app_gfx_surface_t *surface, app_gfx_colour_t colour)
{
    if (!app_gfx_surface_valid(surface)) {
        return;
    }
    size_t n = (size_t)surface->rows * APP_GFX_WIDTH;
    uint8_t v = (uint8_t)(colour & 0x07);
    for (size_t i = 0; i < n; i++) {
        surface->pixels[i] = v;
    }
}

void app_gfx_fill_rect(const app_gfx_surface_t *surface, int x, int y, int w, int h,
                       app_gfx_colour_t colour)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            app_gfx_pixel(surface, col, row, colour);
        }
    }
}

void app_gfx_hline(const app_gfx_surface_t *surface, int x0, int x1, int y,
                   app_gfx_colour_t colour)
{
    if (x1 < x0) {
        int t = x0; x0 = x1; x1 = t;
    }
    for (int x = x0; x <= x1; x++) {
        app_gfx_pixel(surface, x, y, colour);
    }
}

void app_gfx_vline(const app_gfx_surface_t *surface, int x, int y0, int y1,
                   app_gfx_colour_t colour)
{
    if (y1 < y0) {
        int t = y0; y0 = y1; y1 = t;
    }
    for (int y = y0; y <= y1; y++) {
        app_gfx_pixel(surface, x, y, colour);
    }
}

void app_gfx_rect(const app_gfx_surface_t *surface, int x, int y, int w, int h,
                  app_gfx_colour_t colour)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    app_gfx_hline(surface, x, x + w - 1, y, colour);
    app_gfx_hline(surface, x, x + w - 1, y + h - 1, colour);
    app_gfx_vline(surface, x, y, y + h - 1, colour);
    app_gfx_vline(surface, x + w - 1, y, y + h - 1, colour);
}

void app_gfx_line(const app_gfx_surface_t *surface, int x0, int y0, int x1, int y1,
                  app_gfx_colour_t colour)
{
    /* Integer Bresenham, stepped over the whole line even where it is outside the
     * surface. Clipping the endpoints instead would change which pixels the algorithm
     * chooses, and the band above would then disagree with the band below along the
     * shared edge -- which is exactly the seam bug this design exists to make
     * impossible. Walking the whole line costs a few hundred rejected pixels. */
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx >= 0 ? 1 : -1;
    int sy = dy >= 0 ? 1 : -1;
    dx = dx >= 0 ? dx : -dx;
    dy = dy >= 0 ? dy : -dy;

    int err = dx - dy;
    for (;;) {
        app_gfx_pixel(surface, x0, y0, colour);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

size_t app_gfx_pack(const app_gfx_surface_t *surface, uint8_t *out, size_t out_len)
{
    if (!app_gfx_surface_valid(surface) || out == NULL) {
        return 0;
    }
    size_t n = (size_t)surface->rows * APP_GFX_WIDTH;
    size_t need = n / 2;
    if (out_len < need) {
        return 0;
    }
    for (size_t i = 0; i < need; i++) {
        uint8_t left  = surface->pixels[i * 2]     & 0x07;
        uint8_t right = surface->pixels[i * 2 + 1] & 0x07;
        out[i] = (uint8_t)((left << 4) | right);
    }
    return need;
}

/* --- Text -------------------------------------------------------------------------- */

/* Decode one UTF-8 sequence. Returns the codepoint and advances *p, or returns 0 at the
 * end of the string. Malformed input consumes one byte and yields 0xFFFD, which no font
 * here contains, so it draws nothing -- see app_gfx_text on why that is deliberate.
 *
 * Two-byte sequences are all this needs (the degree sign is U+00B0); three and four are
 * handled only so a stray multi-byte character cannot desynchronise the rest of a line. */
static uint32_t utf8_next(const char **p)
{
    const uint8_t *s = (const uint8_t *)*p;
    if (s[0] == 0) {
        return 0;
    }
    if (s[0] < 0x80) {
        *p += 1;
        return s[0];
    }
    int len = (s[0] & 0xE0) == 0xC0 ? 2
            : (s[0] & 0xF0) == 0xE0 ? 3
            : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 0) {
        *p += 1;
        return 0xFFFD;
    }
    uint32_t cp = s[0] & (0x7F >> len);
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *p += 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += len;
    return cp;
}

static const app_gfx_glyph_t *find_glyph(const app_gfx_font_t *font, uint32_t cp)
{
    if (font == NULL || font->glyphs == NULL || cp > 0xFFFF) {
        return NULL;
    }
    int lo = 0;
    int hi = (int)font->glyph_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint16_t at = font->glyphs[mid].codepoint;
        if (at == cp) {
            return &font->glyphs[mid];
        }
        if (at < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

int app_gfx_text_width(const app_gfx_font_t *font, const char *text)
{
    if (font == NULL || text == NULL) {
        return 0;
    }
    int width = 0;
    const char *p = text;
    for (uint32_t cp = utf8_next(&p); cp != 0; cp = utf8_next(&p)) {
        const app_gfx_glyph_t *g = find_glyph(font, cp);
        if (g != NULL) {
            width += g->advance;
        }
    }
    return width;
}

void app_gfx_text(const app_gfx_surface_t *surface, int x, int baseline,
                  const app_gfx_font_t *font, const char *text, app_gfx_colour_t colour)
{
    if (font == NULL || font->bitmap == NULL || text == NULL) {
        return;
    }
    /* Note there is no early return on an invalid surface: the pen still has to advance
     * so that a caller measuring by drawing gets the same answer either way, and
     * app_gfx_pixel rejects every write on its own. */
    const char *p = text;
    for (uint32_t cp = utf8_next(&p); cp != 0; cp = utf8_next(&p)) {
        const app_gfx_glyph_t *g = find_glyph(font, cp);
        if (g == NULL) {
            continue;
        }
        size_t stride = ((size_t)g->width + 7) / 8;
        for (int row = 0; row < (int)g->height; row++) {
            const uint8_t *bits = &font->bitmap[g->offset + (size_t)row * stride];
            for (int col = 0; col < (int)g->width; col++) {
                if (bits[col >> 3] & (0x80 >> (col & 7))) {
                    app_gfx_pixel(surface,
                                  x + g->left + col,
                                  baseline + g->top + row,
                                  colour);
                }
            }
        }
        x += g->advance;
    }
}
