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

/*
 * OpenDMR, UI: editing a text or a number in place, in a list row. From the name and number editing of
 * menuContactDetails.c / menuChannelDetails.c (moveCursorLeftInString(), moveCursorRightInString() of menuSystem.c)
 * and the multi-tap letters of io/keyboard.c (keypadAlphaMap): the keypad task did the tapping there, here it is the
 * entry, on the UI events.
 */

#include <string.h>
#include "codeplug_text.h"
#include "text_entry.h"

#define TAP_KEY_STAR 10
#define SHOWN_SIZE   64 // the text in UTF-8: a GB2312 character (2 bytes) is 3

// The characters of each key, in tapping order (io/keyboard.c)
static const char *const tap_map[] = {
    "0 ",      "1.!,@-:?()~/[]#<>=*+$%'`&|_^{}",
    "abc2ABC", "def3DEF",
    "ghi4GHI", "jkl5JKL",
    "mno6MNO", "pqrs7PQRS",
    "tuv8TUV", "wxyz9WXYZ",
    "*",
};

// Bytes of the character at pos: a GB2312 character is a lead byte from 0xA1 and one more (codeplug_text.c)
static int char_length(const char *text, int pos)
{
    return (((uint8_t)text[pos] >= 0xA1) && (text[pos + 1] != 0)) ? 2 : 1;
}

// Where the character before pos starts
static int previous_char(const char *text, int pos)
{
    int at = 0;

    while (at < pos) {
        int next = at + char_length(text, at);

        if (next >= pos) {
            break;
        }
        at = next;
    }
    return at;
}

static void remove_char(text_entry_t *entry)
{
    int len = strlen(entry->text);

    if (entry->pos < len) {
        int n = char_length(entry->text, entry->pos);

        memmove(&entry->text[entry->pos], &entry->text[entry->pos + n], len - entry->pos - n + 1);
    }
}

static void insert_char(text_entry_t *entry, char c)
{
    int len = strlen(entry->text);

    if (len < entry->max) {
        memmove(&entry->text[entry->pos + 1], &entry->text[entry->pos], len - entry->pos + 1);
        entry->text[entry->pos] = c;
    }
}

// The character under the cursor becomes c (the cursor doesn't move: the same key may still change it)
static void put_char(text_entry_t *entry, char c)
{
    remove_char(entry);
    insert_char(entry, c);
}

void text_entry_start(text_entry_t *entry, char *text, int max, bool numeric)
{
    int len = strlen(text);

    entry->text = text;
    entry->max = (uint8_t)max;
    entry->numeric = numeric;
    entry->tap_key = -1;
    entry->tap_index = 0;
    // At the end; a full text: on its last character, the cursor stays inside the field
    entry->pos = (uint8_t)((len < max) ? len : previous_char(text, len));
}

void text_entry_commit(text_entry_t *entry)
{
    if (entry->tap_key < 0) {
        return;
    }
    entry->tap_key = -1;
    if ((entry->pos < (int)strlen(entry->text)) && (entry->pos < (entry->max - 1))) {
        entry->pos++;
    }
}

static void tap(text_entry_t *entry, int key)
{
    if (entry->tap_key == key) {
        entry->tap_index++;
        if (tap_map[key][entry->tap_index] == 0) {
            entry->tap_index = 0;
        }
    } else {
        text_entry_commit(entry);
        entry->tap_key = (int8_t)key;
        entry->tap_index = 0;
    }
    put_char(entry, tap_map[key][entry->tap_index]);
    ui_timer_start(TEXT_ENTRY_TAP_MS);
}

static void move_left(text_entry_t *entry, bool delete)
{
    int len = strlen(entry->text);

    if (entry->pos == 0) {
        return;
    }
    // A full text: the cursor is on the last character, not after it, so that one goes first
    if (!delete || (len < entry->max) || (entry->pos != (entry->max - 1))) {
        entry->pos = (uint8_t)previous_char(entry->text, entry->pos);
    }
    if (delete) {
        remove_char(entry);
    }
}

static void move_right(text_entry_t *entry, bool insert)
{
    int len = strlen(entry->text);

    if (entry->pos >= len) {
        return;
    }
    if (insert) {
        insert_char(entry, ' ');
    }

    int next = entry->pos + char_length(entry->text, entry->pos);

    if (next <= (entry->max - 1)) {
        entry->pos = (uint8_t)next;
    }
}

static bool handle_number(text_entry_t *entry, const ui_event_t *ev)
{
    int len = strlen(entry->text);
    int digit = ui_key_digit(ev->key);

    if ((ev->type == UI_EVENT_KEY_PRESS) && (digit >= 0)) {
        if (len < entry->max) {
            entry->text[len++] = (char)('0' + digit);
            entry->text[len] = 0;
        }
    } else if (((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT)) && (ev->key == UI_KEY_LEFT)) {
        if (len > 0) {
            entry->text[--len] = 0;
        }
    } else {
        return false;
    }
    entry->pos = (uint8_t)len;
    return true;
}

bool text_entry_handle(text_entry_t *entry, const ui_event_t *ev)
{
    if (entry->numeric) {
        return handle_number(entry, ev);
    }

    if (ev->type == UI_EVENT_TIMER) {
        if (entry->tap_key < 0) {
            return false;
        }
        text_entry_commit(entry);
        return true;
    }
    if ((ev->type != UI_EVENT_KEY_PRESS) && (ev->type != UI_EVENT_KEY_LONG)) {
        return false;
    }

    int key = (ev->key == UI_KEY_STAR) ? TAP_KEY_STAR : ui_key_digit(ev->key);
    bool sk2 = (ev->modifiers & UI_MODIFIER_SK2) != 0;

    if (key >= 0) {
        if (ev->type == UI_EVENT_KEY_PRESS) {
            tap(entry, key);
        }
        return true;
    }

    // Any other key takes the tapped character first, as on the main firmware
    text_entry_commit(entry);

    if (ev->key == UI_KEY_LEFT) {
        if (ev->type == UI_EVENT_KEY_LONG) {
            entry->pos = 0;
        } else {
            move_left(entry, sk2);
        }
        return true;
    }
    if (ev->key == UI_KEY_RIGHT) {
        if (ev->type == UI_EVENT_KEY_LONG) {
            int len = strlen(entry->text);

            entry->pos = (uint8_t)((len < entry->max) ? len : previous_char(entry->text, len));
        } else {
            move_right(entry, sk2);
        }
        return true;
    }
    return false;
}

int16_t text_entry_cursor(const text_entry_t *entry, int prefix_bytes)
{
    char shown[SHOWN_SIZE];

    codeplug_text_name(entry->text, entry->pos, shown, sizeof(shown));
    return (int16_t)(1 + prefix_bytes + strlen(shown));
}
