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

// Layout of docs/ui-design.md 5.1, in LCD pixels. The zone line starts at y 44: the OpenGD77 header redraw clears and sends
// the LCD rows above 40.
#define ZONE_Y      44
#define NAME_Y      68
#define FREQUENCY_Y 110
#define SEPARATOR_Y 146
#define LINE_Y      160

void channel_panel_draw(const channel_panel_data_t *data)
{
    const int32_t x = WIDGET_MARGIN;

    if (data->zone) {
        draw_text(x, ZONE_Y, data->zone, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    }

    if (data->name) {
        draw_text(x, NAME_Y, data->name, &font_roboto32_bold, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
    }

    if (data->frequency) {
        draw_text(x, FREQUENCY_Y, data->frequency, &font_roboto22, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_SECOND);
    }

    if (data->line_icon < ICONS_COUNT) {
        const font_t *font = &font_roboto20;
        int32_t lx = x;

        draw_hline(x, SEPARATOR_Y, FB_WIDTH - (2 * WIDGET_MARGIN), THEME_COLOUR_LINE);

        lx += icons_draw(lx, LINE_Y + ((font->line_height - ICONS_SIZE) / 2) + 2, data->line_icon,
                         THEME_COLOUR_TEXT_LABEL) +
              8;

        if (data->line_text) {
            lx = font_draw_text(lx, LINE_Y, data->line_text, font, FONT_ALIGN_LEFT, theme_colour(THEME_COLOUR_TEXT)) +
                 10;
        }

        if (data->line_detail) {
            draw_text(lx, LINE_Y + 2, data->line_detail, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
        }
    }
}
