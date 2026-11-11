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

#include "widgets.h"
#include "draw.h"
#include "fonts.h"

const char *bottom_bar_power_label(int level)
{
    static const char *const labels[] = {"50mW", "500mW", "1W", "2W", "3W", "4W", "5W", "7W", "10W", "+W-"};
    int count = (int)(sizeof(labels) / sizeof(labels[0]));

    return labels[(level < 0) ? 0 : ((level >= count) ? (count - 1) : level)];
}

void bottom_bar_draw(const bottom_bar_data_t *data)
{
    const int32_t bar_top = WIDGET_BOTTOM_BAR_TOP + 2;
    const int32_t bar_height = FB_HEIGHT - bar_top;
    const font_t *font = &font_roboto17;
    const int32_t text_y = bar_top + ((bar_height - font->line_height) / 2);
    int32_t x = WIDGET_MARGIN - 4;

    draw_fill_rect(0, WIDGET_BOTTOM_BAR_TOP, FB_WIDTH, 2, THEME_COLOUR_LINE);
    draw_fill_rect(0, bar_top, FB_WIDTH, bar_height, THEME_COLOUR_BACKGROUND);

    if (data->mode) {
        x += draw_chip(x, bar_top + ((bar_height - 24) / 2), 24, data->mode, &font_roboto17_bold, THEME_COLOUR_WHITE,
                       THEME_COLOUR_ACCENT) +
             12;
    }

    if (data->item1) {
        x = font_draw_text(x, text_y, data->item1, font, FONT_ALIGN_LEFT, theme_colour(THEME_COLOUR_TEXT_SECOND)) + 12;
    }

    if (data->item2) {
        draw_text(x, text_y, data->item2, font, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    }

    if (data->right) {
        draw_text(FB_WIDTH - WIDGET_MARGIN, text_y, data->right, font, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT_LABEL);
    }
}

void bottom_bar_draw_soft_keys(const char *left, const char *right)
{
    const int32_t bar_top = WIDGET_BOTTOM_BAR_TOP + 2;
    const int32_t bar_height = FB_HEIGHT - bar_top;
    const font_t *font = &font_roboto17_bold;
    const int32_t text_y = bar_top + ((bar_height - font->line_height) / 2);

    draw_fill_rect(0, WIDGET_BOTTOM_BAR_TOP, FB_WIDTH, 2, THEME_COLOUR_LINE);
    draw_fill_rect(0, bar_top, FB_WIDTH, bar_height, THEME_COLOUR_BACKGROUND);

    if (left) {
        draw_text(WIDGET_MARGIN, text_y, left, font, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
    }

    if (right) {
        draw_text(FB_WIDTH - WIDGET_MARGIN, text_y, right, font, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT);
    }
}
