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

#ifndef DM32_FONT_H
#define DM32_FONT_H

#include <stdbool.h>
#include <stdint.h>

// Anti-aliased, proportional fonts (docs/ui-design.md, 4.2), made by tools/fonts/mkfont.py: 4 bits of coverage per pixel,
// each glyph cropped to its ink, rows packed 2 pixels per byte (left pixel in the high nibble).
#define FONT_GLYPH_NONE 0xFFFFFFFFU

typedef struct {
    uint8_t width; // ink box
    uint8_t height;
    int8_t x_offset; // ink box from the pen position
    int8_t y_offset; // ink box from the top of the line
    uint8_t advance; // pen move
    uint32_t offset; // in the bitmap, FONT_GLYPH_NONE: not in the font
} font_glyph_t;

typedef struct {
    uint8_t line_height;
    uint8_t ascent; // baseline from the top of the line
    uint16_t first; // first character code
    uint16_t count;
    const font_glyph_t *glyphs;
    const uint8_t *bitmap;
} font_t;

typedef enum {
    FONT_ALIGN_LEFT = 0,
    FONT_ALIGN_CENTER,
    FONT_ALIGN_RIGHT
} font_align_t;

// Next character of an UTF-8 string (0 at the end); invalid bytes are returned as they are
uint32_t font_utf8_next(const char **text);

bool font_has_char(const font_t *font, uint32_t code);
int32_t font_text_width(const font_t *font, const char *text);

// Draws text with the top of its line at y, in LCD pixels; x is the left, centre or right edge depending on align.
// Returns the x after the text.
int32_t font_draw_text(int32_t x, int32_t y, const char *text, const font_t *font, font_align_t align, uint16_t colour);

// Draws one character with the top of its line at y; returns its advance (0 when the font doesn't have it)
int32_t font_draw_char(int32_t x, int32_t y, uint32_t code, const font_t *font, uint16_t colour);

#endif /* DM32_FONT_H */
