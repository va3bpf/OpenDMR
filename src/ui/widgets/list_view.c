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

#include <string.h>
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "util.h"

// Menu of docs/ui-design.md (5.6): title bar, rows of an icon and a name (a value on the right), the selected row
// white on the accent colour.
void list_view_draw_title(const char *title, const char *position)
{
    const font_t *font = &font_roboto17_bold;
    const int32_t text_y = (WIDGET_STATUS_BAR_HEIGHT - font->line_height) / 2;

    draw_fill_rect(0, 0, FB_WIDTH, WIDGET_STATUS_BAR_HEIGHT, THEME_COLOUR_BACKGROUND);
    draw_fill_rect(0, WIDGET_STATUS_BAR_HEIGHT, FB_WIDTH, 2, THEME_COLOUR_LINE);

    if (title) {
        draw_text(WIDGET_MARGIN, text_y, title, font, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
    }

    if (position) {
        draw_text(FB_WIDTH - WIDGET_MARGIN, text_y, position, &font_roboto17, FONT_ALIGN_RIGHT,
                  THEME_COLOUR_TEXT_LABEL);
    }
}

// Bytes of the UTF-8 character starting with c
static int32_t utf8_length(uint8_t c)
{
    return (((c & 0x80) == 0) ? 1 : (((c & 0xE0) == 0xC0) ? 2 : (((c & 0xF0) == 0xE0) ? 3 : 4)));
}

// At most len bytes of src, and at most size - 1
static void copy_text(char *dst, int32_t size, const char *src, int32_t len)
{
    len = MIN(len, (size - 1));
    memcpy(dst, src, len);
    dst[len] = 0;
}

// The text cut (whole characters) to fit in width pixels
static const char *fit_text(char *buffer, int32_t size, const char *text, const font_t *font, int32_t width)
{
    int32_t len;

    if (font_text_width(font, text) <= width) {
        return text;
    }

    copy_text(buffer, size, text, strlen(text));
    len = strlen(buffer);

    while ((len > 0) && (font_text_width(font, buffer) > width)) {
        do {
            len--;
        } while ((len > 0) && ((buffer[len] & 0xC0) == 0x80));
        buffer[len] = 0;
    }

    return buffer;
}

// Value right aligned at x, with the text entry cursor (an underline) if any
static void draw_value(int32_t x, int32_t y, const list_view_row_t *data, const font_t *font, theme_colour_t colour,
                       theme_colour_t cursor_colour)
{
    char part[48];
    int32_t len, index, before, char_width, total;

    if (data->cursor <= 0) {
        draw_text(x, y, data->value, font, FONT_ALIGN_RIGHT, colour);
        return;
    }

    len = strlen(data->value);
    index = MIN(data->cursor - 1, len);

    copy_text(part, sizeof(part), data->value, index);
    before = font_text_width(font, part);

    if (index < len) {
        copy_text(part, sizeof(part), &data->value[index], MIN(utf8_length(data->value[index]), (len - index)));
        char_width = font_text_width(font, part);
        total = font_text_width(font, data->value);
    } else {
        // after the text: room for one more character
        char_width = font_text_width(font, "0");
        total = font_text_width(font, data->value) + char_width;
    }

    x -= total;
    draw_text(x, y, data->value, font, FONT_ALIGN_LEFT, colour);
    draw_fill_rect(x + before, y + font->line_height - 1, MAX(char_width, 4), 2, cursor_colour);
}

#define SWATCH_WIDTH  28
#define SWATCH_HEIGHT 18
#define SWATCH_X      (FB_WIDTH - WIDGET_MARGIN - SWATCH_WIDTH)

void list_view_draw_row(int32_t row, const list_view_row_t *data)
{
    const font_t *font = &font_roboto20;
    int32_t y = LIST_VIEW_TOP + (row * LIST_VIEW_ROW_HEIGHT);
    int32_t text_y = y + ((LIST_VIEW_ROW_HEIGHT - font->line_height) / 2);
    int32_t x = WIDGET_MARGIN;
    theme_colour_t name_colour =
        (data->selected ? THEME_COLOUR_WHITE : (data->dimmed ? THEME_COLOUR_TEXT_DIM : THEME_COLOUR_TEXT));
    theme_colour_t value_colour = (data->selected ? THEME_COLOUR_WHITE : THEME_COLOUR_TEXT_LABEL);

    if ((row < 0) || (row >= LIST_VIEW_ROWS)) {
        return;
    }

    draw_fill_rect(0, y, FB_WIDTH, LIST_VIEW_ROW_HEIGHT,
                   (data->selected ? THEME_COLOUR_ACCENT : THEME_COLOUR_BACKGROUND));

    if (data->icon < ICONS_COUNT) {
        x += icons_draw(x, y + ((LIST_VIEW_ROW_HEIGHT - ICONS_SIZE) / 2), data->icon,
                        (data->selected ? THEME_COLOUR_WHITE : THEME_COLOUR_TEXT_LABEL)) +
             10;
    }

    if (data->name) {
        draw_text(x, text_y, data->name, font, FONT_ALIGN_LEFT, name_colour);
    }

    if (data->swatch) {
        // the frame, filled later (list_view_fill_swatch())
        int32_t sy = y + ((LIST_VIEW_ROW_HEIGHT - SWATCH_HEIGHT) / 2);
        theme_colour_t frame = (data->selected ? THEME_COLOUR_WHITE : THEME_COLOUR_TEXT_LABEL);

        draw_fill_rect(SWATCH_X, sy, SWATCH_WIDTH, 1, frame);
        draw_fill_rect(SWATCH_X, sy + SWATCH_HEIGHT - 1, SWATCH_WIDTH, 1, frame);
        draw_fill_rect(SWATCH_X, sy, 1, SWATCH_HEIGHT, frame);
        draw_fill_rect(SWATCH_X + SWATCH_WIDTH - 1, sy, 1, SWATCH_HEIGHT, frame);
    }

    if (data->value) {
        draw_value(FB_WIDTH - WIDGET_MARGIN - (data->swatch ? (SWATCH_WIDTH + 8) : 0), text_y, data, font, value_colour,
                   (data->selected ? THEME_COLOUR_WHITE : THEME_COLOUR_ACCENT));
    }
}

void list_view_fill_swatch(int32_t row, uint16_t colour)
{
    int32_t sy = LIST_VIEW_TOP + (row * LIST_VIEW_ROW_HEIGHT) + ((LIST_VIEW_ROW_HEIGHT - SWATCH_HEIGHT) / 2);

    if ((row >= 0) && (row < LIST_VIEW_ROWS)) {
        fb_fill_direct(SWATCH_X + 1, sy + 1, SWATCH_X + SWATCH_WIDTH - 1, sy + SWATCH_HEIGHT - 1, colour);
    }
}

void list_view_draw_row2(int32_t y, const list_view_row_t *data)
{
    const font_t *font = &font_roboto17;
    const font_t *sub_font = &font_roboto14;
    const int32_t name_y = y + 3;
    const int32_t sub_y = name_y + font->line_height;
    int32_t x = WIDGET_MARGIN;
    int32_t right = FB_WIDTH - WIDGET_MARGIN;
    char buffer[64];

    draw_fill_rect(0, y, FB_WIDTH, LIST_VIEW_ROW2_HEIGHT,
                   (data->selected ? THEME_COLOUR_ACCENT : THEME_COLOUR_BACKGROUND));

    if (data->icon < ICONS_COUNT) {
        x += icons_draw(x, y + ((LIST_VIEW_ROW2_HEIGHT - ICONS_SIZE) / 2), data->icon,
                        (data->selected ? THEME_COLOUR_WHITE : THEME_COLOUR_TEXT_LABEL)) +
             10;
    }

    if (data->value) {
        draw_text(right, y + ((LIST_VIEW_ROW2_HEIGHT - font->line_height) / 2), data->value, font, FONT_ALIGN_RIGHT,
                  (data->selected ? THEME_COLOUR_WHITE : THEME_COLOUR_TEXT_LABEL));
        right -= font_text_width(font, data->value) + 8;
    }

    if (data->name) {
        draw_text(x, name_y, fit_text(buffer, sizeof(buffer), data->name, font, right - x), font, FONT_ALIGN_LEFT,
                  (data->selected ? THEME_COLOUR_WHITE : (data->dimmed ? THEME_COLOUR_TEXT_DIM : THEME_COLOUR_TEXT)));
    }

    if (data->sub) {
        draw_text(x, sub_y, fit_text(buffer, sizeof(buffer), data->sub, sub_font, right - x), sub_font, FONT_ALIGN_LEFT,
                  (data->selected ? THEME_COLOUR_WHITE : THEME_COLOUR_TEXT_LABEL));
    }
}

void list_view_draw_text(const char *line1, const char *line2, theme_colour_t colour)
{
    const font_t *font = &font_roboto20;
    const int32_t y = WIDGET_MAIN_TOP + 18;

    if (line1) {
        draw_text(WIDGET_MARGIN, y, line1, font, FONT_ALIGN_LEFT, colour);
    }

    if (line2) {
        draw_text(WIDGET_MARGIN, y + font->line_height + 8, line2, font, FONT_ALIGN_LEFT, colour);
    }
}
