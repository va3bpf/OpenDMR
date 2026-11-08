/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 *    disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 *    following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden without the written
 *    permission of the copyright holder. This includes, without limitation, incorporation in a commercial product or
 *    incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "draw.h"
#include "framebuffer.h"

void draw_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, theme_colour_t colour)
{
    fb_fill(x, y, x + w, y + h, theme_colour(colour));
}

void draw_hline(int32_t x, int32_t y, int32_t w, theme_colour_t colour)
{
    fb_fill(x, y, x + w, y + 1, theme_colour(colour));
}

void draw_text(int32_t x, int32_t y, const char *text, const font_t *font, font_align_t align, theme_colour_t colour)
{
    font_draw_text(x, y, text, font, align, theme_colour(colour));
}

// Coverage (0 - 16) of the pixel at squared distance d2 (in quarter pixels squared) from the centre by a disc of
// radius rQ: full inside rQ - 2, none outside rQ + 2, linear in d2 in between (no square root).
static int32_t disc_coverage(int32_t d2, int32_t r_q)
{
    int32_t inner = (r_q > 2) ? ((r_q - 2) * (r_q - 2)) : 0;
    int32_t outer = (r_q + 2) * (r_q + 2);

    if (d2 <= inner) {
        return FB_ALPHA_OPAQUE;
    }
    if (d2 >= outer) {
        return 0;
    }
    return ((outer - d2) * FB_ALPHA_OPAQUE) / (outer - inner);
}

static void draw_circle(int32_t cx, int32_t cy, int32_t outer_q, int32_t inner_q, theme_colour_t colour)
{
    uint16_t c = theme_colour(colour);
    int32_t extent = (outer_q / 4) + 1;

    for (int32_t y = -extent; y <= extent; y++) {
        for (int32_t x = -extent; x <= extent; x++) {
            int32_t d2 = ((4 * x) * (4 * x)) + ((4 * y) * (4 * y));
            int32_t alpha = disc_coverage(d2, outer_q) - ((inner_q > 0) ? disc_coverage(d2, inner_q) : 0);

            if (alpha > 0) {
                fb_blend_pixel(cx + x, cy + y, c, alpha);
            }
        }
    }
}

void draw_fill_circle(int32_t cx, int32_t cy, int32_t radius_q, theme_colour_t colour)
{
    draw_circle(cx, cy, radius_q, 0, colour);
}

void draw_ring(int32_t cx, int32_t cy, int32_t radius_q, int32_t width_q, theme_colour_t colour)
{
    draw_circle(cx, cy, radius_q + (width_q / 2), radius_q - (width_q / 2), colour);
}

int32_t draw_chip(int32_t x, int32_t y, int32_t h, const char *text, const font_t *font, theme_colour_t text_colour,
                  theme_colour_t box_colour)
{
    const int32_t padding = 6;
    int32_t w = font_text_width(font, text) + (2 * padding);
    uint16_t box = theme_colour(box_colour);

    // box with its corner pixels left out
    fb_fill(x + 1, y, x + w - 1, y + h, box);
    fb_fill(x, y + 1, x + 1, y + h - 1, box);
    fb_fill(x + w - 1, y + 1, x + w, y + h - 1, box);

    font_draw_text(x + padding, y + ((h - font->line_height) / 2), text, font, FONT_ALIGN_LEFT,
                   theme_colour(text_colour));

    return w;
}

// Bresenham
void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, theme_colour_t colour, bool thick)
{
    int32_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int32_t dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx + dy;

    for (;;) {
        draw_fill_rect(x0, y0, (thick ? 2 : 1), (thick ? 2 : 1), colour);
        if ((x0 == x1) && (y0 == y1)) {
            break;
        }
        int32_t e2 = 2 * err;

        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}
