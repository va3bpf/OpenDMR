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

#ifndef DM32_FONT_CJK_H
#define DM32_FONT_CJK_H

#include <stdbool.h>
#include <stdint.h>

// Chinese font in the SPI flash (tools/fonts/mkcjk.py, written by tools/fonts/upload_cjk.py): the characters that are
// not in the built-in fonts are drawn from it. Raw 25Q128 addresses, below the OpenGD77 window and free in the stock
// layout too (the stock fonts and pictures end at 0x27E000).
#define FONT_CJK_FLASH_ADDRESS 0x280000
#define FONT_CJK_FLASH_END     0x630000

typedef struct {
    uint32_t bitmap; // flash address of the compressed bitmap
    uint8_t width;   // ink box
    uint8_t height;
    int8_t x_offset; // ink box from the pen position
    int8_t top;      // top of the ink box from the baseline (negative: above)
    uint8_t advance;
} font_cjk_glyph_t;

// Glyph of code for a built-in font with that line height (the big font for the large lines, else the small one)
bool font_cjk_get_glyph(uint32_t code, uint8_t line_height, font_cjk_glyph_t *glyph);
// Draws it with its baseline at y (LCD pixels)
void font_cjk_draw_glyph(int32_t x, int32_t baseline, const font_cjk_glyph_t *glyph, uint16_t colour);
// Unicode of a GB2312 character (names in the codeplug), 0 when unknown or without the font file
uint16_t font_cjk_gb2312_to_unicode(uint8_t high, uint8_t low);

#endif /* DM32_FONT_CJK_H */
