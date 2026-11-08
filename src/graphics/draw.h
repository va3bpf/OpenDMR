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

#ifndef DM32_DRAW_H
#define DM32_DRAW_H

#include <stdbool.h>
#include <stdint.h>
#include "font.h"
#include "theme.h"

// Shapes in LCD pixels (240 x 320), colours of the theme
void draw_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, theme_colour_t colour);
void draw_hline(int32_t x, int32_t y, int32_t w, theme_colour_t colour);
void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, theme_colour_t colour, bool thick); // thick: 2 x 2
void draw_text(int32_t x, int32_t y, const char *text, const font_t *font, font_align_t align,
               theme_colour_t colour); // y: top of the line

// Label on a filled box with slightly rounded corners, text left at x; returns its width
// Anti-aliased circles, in quarter pixels (radius 4 * r): a disc, and a ring of the given width
void draw_fill_circle(int32_t cx, int32_t cy, int32_t radius_q, theme_colour_t colour);
void draw_ring(int32_t cx, int32_t cy, int32_t radius_q, int32_t width_q, theme_colour_t colour);
int32_t draw_chip(int32_t x, int32_t y, int32_t h, const char *text, const font_t *font, theme_colour_t text_colour,
                  theme_colour_t box_colour);

#endif /* DM32_DRAW_H */
