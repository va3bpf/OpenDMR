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

#ifndef DM32_TEXT_ENTRY_H
#define DM32_TEXT_ENTRY_H

#include <stdbool.h>
#include <stdint.h>
#include "ui.h"

// Editing a text or a number in place, in the value of a list row (list_view_row_t.cursor shows where). A screen
// keeps a text_entry_t for the field and gives it the events of the selected row first.
//
// Text: the keys 0 - 9 and * type letters by tapping (as the main firmware's keypad, io/keyboard.c): the same key
// again within TEXT_ENTRY_TAP_MS takes the next character in place, another key or the time takes the character and
// moves on. LEFT / RIGHT move the cursor (held: to the start / the end); with SK2 held, LEFT deletes the character
// before the cursor and RIGHT inserts a space. Codeplug names may hold GB2312 characters (two bytes): the cursor
// steps over them whole.
// Number: the digit keys add a digit at the end, LEFT deletes the last one.
//
// While a character is being tapped, the entry uses the screen's timer (ui_timer_start()): the screen gives
// UI_EVENT_TIMER to text_entry_handle() first.

#define TEXT_ENTRY_TAP_MS 1000

typedef struct {
    char *text;  // NUL terminated, up to max bytes
    uint8_t max; // bytes, without the NUL
    uint8_t pos; // the cursor, a byte index
    bool numeric;
    int8_t tap_key; // the key being tapped (0 - 9, 10: *); -1: none
    int8_t tap_index;
} text_entry_t;

void text_entry_start(text_entry_t *entry, char *text, int max, bool numeric); // the cursor at the end
bool text_entry_handle(text_entry_t *entry, const ui_event_t *ev);             // true: it took the event (draw again)
void text_entry_commit(text_entry_t *entry); // takes a tapped character now (leaving the row, saving)
// list_view_row_t.cursor for the text shown in UTF-8 (codeplug_text_name()), after prefix_bytes of the value
int16_t text_entry_cursor(const text_entry_t *entry, int prefix_bytes);

#endif
