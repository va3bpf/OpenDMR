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
#include "theme.h"
#include "framebuffer.h"
#include "panel.h"
#include "themes/themes.h"

// Text on these is drawn without anti-aliasing (its edges would take the greys made for the background)
#define SOLID_BACKGROUNDS ((1U << THEME_COLOUR_ACCENT) | (1U << THEME_COLOUR_RX) | (1U << THEME_COLOUR_TX) |                            (1U << THEME_COLOUR_WARNING))

static uint16_t palette[16];
static uint8_t current = 0;

uint8_t theme_count(void)
{
    return THEMES_COUNT;
}

const theme_t *theme_get(uint8_t index)
{
    return (index < THEMES_COUNT) ? THEMES[index] : NULL;
}

uint8_t theme_first(bool dark)
{
    for (uint8_t i = 0; i < THEMES_COUNT; i++) {
        if (THEMES[i]->dark == dark) {
            return i;
        }
    }
    return 0;
}

void theme_set(uint8_t index)
{
    const theme_t *theme = theme_get(index);

    if (theme == NULL) {
        index = 0;
        theme = THEMES[0];
    }
    current = index;
    for (int i = 0; i < THEME_COLOUR_COUNT; i++) {
        palette[i] = panel_rgb888_to_native(theme->colours[i]);
    }
    fb_set_solid_backgrounds(SOLID_BACKGROUNDS);
    fb_set_palette(palette); // the buffer holds palette entries: the whole screen takes the new colours
}

uint8_t theme_current(void)
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


// WCAG relative luminance of an RGB888 colour, 0 - 1 (the sRGB curve as a square: close enough to compare)
static float luminance(uint32_t rgb)
{
    float r = (float)((rgb >> 16) & 0xFF) / 255.0f;
    float g = (float)((rgb >> 8) & 0xFF) / 255.0f;
    float b = (float)(rgb & 0xFF) / 255.0f;

    return (0.2126f * r * r) + (0.7152f * g * g) + (0.0722f * b * b);
}

float theme_contrast(theme_colour_t a, theme_colour_t b)
{
    const theme_t *t = THEMES[current];
    float la = luminance(t->colours[a]);
    float lb = luminance(t->colours[b]);

    return (la > lb) ? ((la + 0.05f) / (lb + 0.05f)) : ((lb + 0.05f) / (la + 0.05f));
}
