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
#include "sound.h"

// The large number entry of docs/manual number-entry.png (the VFO screen, the channel details): the digits entered,
// then unlit zeros, the next one underlined
#define CURSOR_H 4

bool digit_entry_active(const digit_entry_t *entry)
{
    return entry->count > 0;
}

void digit_entry_stop(digit_entry_t *entry)
{
    entry->count = 0;
}

uint32_t digit_entry_value(const digit_entry_t *entry)
{
    uint32_t value = 0;
    int count = (entry->zero_fill ? entry->max : entry->count);

    for (int i = 0; i < count; i++) {
        value = (value * 10) + ((i < entry->count) ? (uint32_t)(entry->digits[i] - '0') : 0);
    }
    return value;
}

// The number entered to set(): the entry ends when it was set
// Taken: the ack beep, refused: the error beep, for this key (uiVFOMode.c, menuChannelDetails.c)
static void apply(digit_entry_t *entry, digit_entry_set_t set)
{
    bool taken = set(digit_entry_value(entry));

    ui_key_beep(taken ? SOUND_ACK : SOUND_ERROR);
    if (taken) {
        entry->count = 0;
    } else if (entry->count == entry->max) {
        entry->count--; // the last digit again
    }
}

bool digit_entry_handle(digit_entry_t *entry, const ui_event_t *ev, digit_entry_set_t set)
{
    int digit = ui_key_digit(ev->key);
    bool press = (ev->type == UI_EVENT_KEY_PRESS);

    if (press && (digit >= 0) && (entry->count < entry->max)) {
        entry->digits[entry->count++] = (char)('0' + digit);
        if (entry->count == entry->max) {
            apply(entry, set);
        }
        return true;
    }
    if (entry->count == 0) {
        return false;
    }
    if ((press || (ev->type == UI_EVENT_KEY_REPEAT)) && (ev->key == UI_KEY_LEFT)) {
        entry->count--;
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_RED)) {
        entry->count = 0;
        return true;
    }
    if ((ev->type == (entry->ok_on_release ? UI_EVENT_KEY_RELEASE : UI_EVENT_KEY_PRESS)) && (ev->key == UI_KEY_GREEN)) {
        apply(entry, set);
        return true;
    }
    return false;
}

void digit_entry_draw(const digit_entry_t *entry, int32_t x, int32_t y)
{
    const font_t *font = &font_roboto44_bold;

    for (int i = 0; i < entry->max; i++) {
        char digit[2] = {((i < entry->count) ? entry->digits[i] : '0'), '\0'};
        int32_t start = x;

        x = font_draw_text(x, y, digit, font, FONT_ALIGN_LEFT,
                           theme_colour((i < entry->count) ? THEME_COLOUR_TEXT : THEME_COLOUR_OFF));
        if (i == entry->count) {
            draw_fill_rect(start, y + font->line_height - 2, x - start, CURSOR_H, THEME_COLOUR_ACCENT);
        }
        if ((entry->dot_after > 0) && (i == (entry->dot_after - 1))) {
            x = font_draw_text(x, y, ".", font, FONT_ALIGN_LEFT, theme_colour(THEME_COLOUR_TEXT));
        }
    }
}

void digit_entry_format_frequency(uint32_t frequency, char *out, size_t size)
{
    snprintf(out, size, "%lu.%05lu", (unsigned long)(frequency / 100000), (unsigned long)(frequency % 100000));
}
