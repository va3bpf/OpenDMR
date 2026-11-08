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

#include "theme.h"
#include "framebuffer.h"
#include "panel.h"

// RGB888, in theme_colour_t order, then four greys for the anti-aliased edges of text on the background
static const uint32_t PALETTE_RGB888[16] = {0x000000, // background
                                            0x1C2026, // line
                                            0x3A3F47, // off
                                            0x6E7580, // dim text
                                            0x9AA1AB, // labels
                                            0xD5D9DF, // second text
                                            0xF2F4F7, // text
                                            0x1E5AA8, // accent
                                            0x2BD47D, // receive
                                            0xC9302C, // transmit
                                            0xE9A23B, // warning
                                            0xFFFFFF, // white
                                            0x0E1013, 0x2A2E35, 0x50565F, 0xB7BDC5};

static uint16_t palette[16];

void theme_init(void)
{
    for (int i = 0; i < 16; i++) {
        palette[i] = panel_rgb888_to_native(PALETTE_RGB888[i]);
    }

    fb_set_palette(palette);
}

uint16_t theme_colour(theme_colour_t colour)
{
    return palette[(colour < THEME_COLOUR_COUNT) ? colour : THEME_COLOUR_TEXT];
}
