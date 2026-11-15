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

// RGB888, in theme_colour_t order; then four greys for the anti-aliased edges of text on the background, made from
// the background and the text (AA_GREYS)
static const uint32_t THEMES[THEMES_COUNT][THEME_COLOUR_COUNT] = {
    [THEME_DARK] = {0x000000,  // background
                    0x1C2026,  // line
                    0x3A3F47,  // off
                    0x6E7580,  // dim text
                    0x9AA1AB,  // labels
                    0xD5D9DF,  // second text
                    0xF2F4F7,  // text
                    0x1E5AA8,  // accent
                    0x2BD47D,  // receive
                    0xC9302C,  // transmit
                    0xE9A23B,  // warning
                    0xFFFFFF}, // white
    [THEME_LIGHT] = {0xF6F7F9, // background
                     0xD8DCE2, // line
                     0xC2C8D0, // off
                     0x8B929C, // dim text
                     0x5E6571, // labels
                     0x333943, // second text
                     0x0F1217, // text
                     0x1E5AA8, // accent
                     0x12904B, // receive: darker than on black
                     0xC9302C, // transmit
                     0xB8740A, // warning: darker than on black
                     0xFFFFFF}, // white
};

// Where the anti-aliasing greys lie from the background (0) to the text (256): those of the dark theme
static const uint16_t AA_GREYS[16 - THEME_COLOUR_COUNT] = {17, 49, 91, 199};

// Text on these is drawn without anti-aliasing (its edges would take the greys made for the background)
#define SOLID_BACKGROUNDS ((1U << THEME_COLOUR_ACCENT) | (1U << THEME_COLOUR_RX) | (1U << THEME_COLOUR_TX) | \
                           (1U << THEME_COLOUR_WARNING))

static uint16_t palette[16];
static theme_t current = THEME_DARK;

static uint32_t mix(uint32_t from, uint32_t to, uint32_t part)
{
    uint32_t out = 0;

    for (int shift = 0; shift < 24; shift += 8) {
        int32_t a = (int32_t)((from >> shift) & 0xFF);
        int32_t b = (int32_t)((to >> shift) & 0xFF);

        out |= (uint32_t)(a + (((b - a) * (int32_t)part) / 256)) << shift;
    }
    return out;
}

void theme_set(theme_t theme)
{
    const uint32_t *colours = THEMES[(theme < THEMES_COUNT) ? theme : THEME_DARK];

    current = (theme < THEMES_COUNT) ? theme : THEME_DARK;
    for (int i = 0; i < THEME_COLOUR_COUNT; i++) {
        palette[i] = panel_rgb888_to_native(colours[i]);
    }
    for (int i = THEME_COLOUR_COUNT; i < 16; i++) {
        palette[i] = panel_rgb888_to_native(mix(colours[THEME_COLOUR_BACKGROUND], colours[THEME_COLOUR_TEXT],
                                                AA_GREYS[i - THEME_COLOUR_COUNT]));
    }
    fb_set_solid_backgrounds(SOLID_BACKGROUNDS);
    fb_set_palette(palette); // the buffer holds palette entries: the whole screen takes the new colours
}

theme_t theme_get(void)
{
    return current;
}

void theme_init(void)
{
    theme_set(current);
}

uint16_t theme_colour(theme_colour_t colour)
{
    return palette[(colour < THEME_COLOUR_COUNT) ? colour : THEME_COLOUR_TEXT];
}
