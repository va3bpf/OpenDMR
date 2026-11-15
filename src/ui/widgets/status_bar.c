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

#include <stdio.h>
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "battery.h"
#include "datetime.h"
#include "settings.h"
#include "gps.h"

#define ICONS_WIDTH 20 // icons.h: all icons are 20 x 18

// What is drawn on the receive colour: white where it shows on it (3:1), else the darker of the theme's background
// and text (a dark theme's green is light)
static theme_colour_t on_receive(void)
{
    if (theme_contrast(THEME_COLOUR_WHITE, THEME_COLOUR_RX) >= 3.0f) {
        return THEME_COLOUR_WHITE;
    }
    return (theme_contrast(THEME_COLOUR_BACKGROUND, THEME_COLOUR_RX) >= theme_contrast(THEME_COLOUR_TEXT, THEME_COLOUR_RX))
               ? THEME_COLOUR_BACKGROUND
               : THEME_COLOUR_TEXT;
}

// Signal bars on the left, then the items (time slot, colour code); GPS, scan icon, time and battery on the right.
// Receiving: the whole bar green; transmitting: red with "TX" (a line under the band was lost next to a light
// theme's dark band). On those fills one colour for everything on them.
void status_bar_draw(const status_bar_data_t *data)
{
    const font_t *font = &font_roboto17_bold;
    const int32_t text_y = (WIDGET_STATUS_BAR_HEIGHT - font->line_height) / 2;
    bool transmitting = (data->state == STATUS_BAR_TRANSMITTING);
    bool receiving = (data->state == STATUS_BAR_RECEIVING);
    theme_colour_t fill = (transmitting ? THEME_COLOUR_TX : (receiving ? THEME_COLOUR_RX : THEME_COLOUR_PANEL));
    theme_colour_t on = (transmitting ? THEME_COLOUR_WHITE : on_receive());
    theme_colour_t text = ((transmitting || receiving) ? on : THEME_COLOUR_PANEL_TEXT);
    theme_colour_t label = ((transmitting || receiving) ? on : THEME_COLOUR_PANEL_LABEL);
    int32_t x = WIDGET_MARGIN - 4;
    char buffer[16];

    draw_fill_rect(0, 0, FB_WIDTH, WIDGET_STATUS_BAR_HEIGHT, fill); // the bar only: the 2 rows under it the screen's
    draw_fill_rect(0, WIDGET_STATUS_BAR_HEIGHT, FB_WIDTH, 2, THEME_COLOUR_BACKGROUND);

    if (transmitting) {
        x += icons_draw(x, (WIDGET_STATUS_BAR_HEIGHT - ICONS_SIZE) / 2, ICONS_TX, THEME_COLOUR_WHITE) + 4;
        x = font_draw_text(x, text_y, "TX", font, FONT_ALIGN_LEFT, theme_colour(THEME_COLOUR_WHITE)) + 12;
    } else {
        // the lit bars white (the band's text colour), or the colour on the green
        icons_draw_signal(x, (WIDGET_STATUS_BAR_HEIGHT - 16) / 2, data->signal_bars, text, receiving);
        x += ICONS_SIGNAL_WIDTH + 14;
    }

    // right side first (battery, time, GPS, scanning): the items on the left only where they fit
    int32_t right = FB_WIDTH - WIDGET_MARGIN + 4;
    int percent = battery_percent();
    bool low = battery_is_low();
    bool has_time = datetime_format_local_time(buffer, sizeof(buffer));

    icons_draw_battery(right - 30, (WIDGET_STATUS_BAR_HEIGHT - 14) / 2, percent, (low ? THEME_COLOUR_WARNING : text),
                       text);
    right -= 30 + 6;

    if (!has_time) {
        snprintf(buffer, sizeof(buffer), "%d%%", percent);
    }
    right = font_draw_text(right, text_y, buffer, &font_roboto17, FONT_ALIGN_RIGHT,
                           theme_colour((low && !has_time) ? THEME_COLOUR_WARNING : (has_time ? text : label)));
    right -= font_text_width(&font_roboto17, buffer) + 8;

    // the GPS on: its pin, dim while it searches, lit with a fix
    if (settings_valid() && (settings.gps >= GPS_MODE_ON)) {
        icons_draw(right - ICONS_WIDTH, (WIDGET_STATUS_BAR_HEIGHT - ICONS_SIZE) / 2, ICONS_LOCATION,
                   (gps_has_fix() ? text : label));
        right -= ICONS_WIDTH + 4;
    }

    if (data->scanning) {
        icons_draw(right - 18, (WIDGET_STATUS_BAR_HEIGHT - ICONS_SIZE) / 2, ICONS_SCAN, THEME_COLOUR_WARNING);
        right -= ICONS_WIDTH + 4;
    }

    if (data->item1) {
        x = font_draw_text(x, text_y, data->item1, (data->item1_bold ? &font_roboto17_bold : &font_roboto17),
                           FONT_ALIGN_LEFT, theme_colour(text)) +
            10;
    }

    if (data->item2 && ((x + font_text_width(&font_roboto17, data->item2)) <= right)) {
        font_draw_text(x, text_y, data->item2, &font_roboto17, FONT_ALIGN_LEFT, theme_colour(label));
    }
}
