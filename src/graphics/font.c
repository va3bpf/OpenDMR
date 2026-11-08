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

#include <stddef.h>
#include "font.h"
#include "font_cjk.h"
#include "framebuffer.h"
#include "hw.h"

uint32_t font_utf8_next(const char **text)
{
    const uint8_t *p = (const uint8_t *)*text;
    uint32_t code = *p;
    int extra = 0;

    if (code == 0) {
        return 0;
    }

    if ((code & 0xE0) == 0xC0) {
        code &= 0x1F;
        extra = 1;
    } else if ((code & 0xF0) == 0xE0) {
        code &= 0x0F;
        extra = 2;
    } else if ((code & 0xF8) == 0xF0) {
        code &= 0x07;
        extra = 3;
    }

    p++;
    for (int i = 0; i < extra; i++) {
        if ((*p & 0xC0) != 0x80) {
            // not UTF-8: return the first byte as it is
            code = *(const uint8_t *)*text;
            p = (const uint8_t *)*text + 1;
            break;
        }
        code = (code << 6) | (*p & 0x3F);
        p++;
    }

    *text = (const char *)p;
    return code;
}

static const font_glyph_t *get_glyph(const font_t *font, uint32_t code)
{
    if ((code < font->first) || (code >= (font->first + font->count))) {
        return NULL;
    }

    const font_glyph_t *glyph = &font->glyphs[code - font->first];

    return ((glyph->offset == FONT_GLYPH_NONE) ? NULL : glyph);
}

bool font_has_char(const font_t *font, uint32_t code)
{
    font_cjk_glyph_t ext;

    return ((get_glyph(font, code) != NULL) || ((code >= 0x80) && font_cjk_get_glyph(code, font->line_height, &ext)));
}

// Advance of a character: from the font, else from the Chinese font in the SPI flash; 0 when neither has it
static int32_t char_advance(const font_t *font, uint32_t code)
{
    const font_glyph_t *glyph = get_glyph(font, code);
    font_cjk_glyph_t ext;

    if (glyph) {
        return glyph->advance;
    }

    if ((code >= 0x80) && font_cjk_get_glyph(code, font->line_height, &ext)) {
        return ext.advance;
    }

    return 0;
}

int32_t font_text_width(const font_t *font, const char *text)
{
    int32_t width = 0;
    uint32_t code;

    while ((code = font_utf8_next(&text)) != 0) {
        int32_t advance = char_advance(font, code);

        width += ((advance != 0) ? advance : char_advance(font, '?'));
    }

    return width;
}

int32_t RAM_CODE font_draw_char(int32_t x, int32_t y, uint32_t code, const font_t *font, uint16_t colour)
{
    const font_glyph_t *glyph = get_glyph(font, code);

    if (glyph == NULL) {
        font_cjk_glyph_t ext;

        if ((code >= 0x80) && font_cjk_get_glyph(code, font->line_height, &ext)) {
            font_cjk_draw_glyph(x, y + font->ascent, &ext, colour);
            return ext.advance;
        }
        return 0;
    }

    const uint8_t *row = &font->bitmap[glyph->offset];
    int32_t x0 = x + glyph->x_offset;
    int32_t y0 = y + glyph->y_offset;

    fb_draw_levels(x0, y0, row, glyph->width, glyph->height, colour);

    return glyph->advance;
}

int32_t font_draw_text(int32_t x, int32_t y, const char *text, const font_t *font, font_align_t align, uint16_t colour)
{
    uint32_t code;

    if (align == FONT_ALIGN_CENTER) {
        x -= (font_text_width(font, text) / 2);
    } else if (align == FONT_ALIGN_RIGHT) {
        x -= font_text_width(font, text);
    }

    while ((code = font_utf8_next(&text)) != 0) {
        int32_t advance = font_draw_char(x, y, code, font, colour);

        if (advance == 0) {
            advance = font_draw_char(x, y, '?', font, colour);
        }

        x += advance;
    }

    return x;
}
