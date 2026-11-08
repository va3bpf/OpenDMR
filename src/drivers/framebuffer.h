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

#ifndef DM32_FRAMEBUFFER_H
#define DM32_FRAMEBUFFER_H

#include <stdint.h>

// Frame buffer of the 240 x 320 LCD (docs/ui-design.md, 6.1): 4 bit indexes into a 16 colour palette, 2 pixels per byte.
// Colours are in the native format (byte swapped RGB565), as the theme stores them.
#define FB_WIDTH        240
#define FB_HEIGHT       320
#define FB_BYTES        ((FB_WIDTH * FB_HEIGHT) / 2)
#define FB_ALPHA_OPAQUE 16 // alpha of fb_blend_pixel(): 0 (nothing) to 16 (the colour)

uint8_t *fb_get_buffer(void);
uint8_t *fb_get_primary_buffer(void);
void fb_set_buffer(uint8_t *buffer); // draw into another buffer of FB_BYTES (NULL: the primary one)

void fb_set_palette(
    const uint16_t colours[16]); // fixed palette (theme.c); every colour gets the nearest entry
void fb_clear(uint16_t colour);  // the whole buffer
void fb_fill(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t colour); // pixels [x0, x1) x [y0, y1), clipped
void fb_set_pixel(int32_t x, int32_t y, uint16_t colour);
void fb_blend_pixel(int32_t x, int32_t y, uint16_t colour, int32_t alpha); // colour over what is there (anti-aliasing)

// Font pixels: levels 0 (nothing) to 15 (the colour), anti-aliasing from a table computed once per colour
void fb_draw_levels(int32_t x0, int32_t y0, const uint8_t *levels, int32_t width, int32_t height,
                    uint16_t colour); // 2 a byte, rows of (width + 1) / 2 bytes
void fb_draw_level(int32_t x, int32_t y, uint16_t colour, uint8_t level);
// Sends the display what changed in the buffer. Everyone just draws and calls this: rows not drawn since the last flush
// cost nothing, unchanged pixels are never sent.
void fb_flush(void);
void fb_mark_rows_drawn(int32_t y0, int32_t y1); // rows [y0, y1) were written directly into the buffer
void fb_fill_direct(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                    uint16_t colour); // [x0, x1) x [y0, y1) straight to the display, any colour

#endif /* DM32_FRAMEBUFFER_H */
