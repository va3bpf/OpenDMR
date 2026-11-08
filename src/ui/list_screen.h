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

#ifndef DM32_LIST_SCREEN_H
#define DM32_LIST_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "icons.h"
#include "text.h"
#include "ui.h"

// Menus and settings as tables (docs/ui-design.md 6.5): the list screen shows a ui_list_t, pushed with
// ui_push(&list_screen, (void *)&a_list). UP / DOWN move, LEFT / RIGHT change the value of the selected item
// (decrease / increase), so does the rotary (it never moves); the left key opens or switches it, the right key goes
// back. A change marks the settings dirty: they are saved when the list is left.

typedef enum {
    UI_ITEM_SCREEN, // opens target (a ui_screen_t) with arg; target NULL: the "not yet" page
    UI_ITEM_LIST,   // opens the list target (a ui_list_t)
    UI_ITEM_BIT,    // target: a ui_bit_t
    UI_ITEM_CHOICE, // target: a ui_choice_t
    UI_ITEM_NUMBER, // target: a ui_number_t
    UI_ITEM_CUSTOM, // target: a ui_custom_t
} ui_item_type_t;

// A bit of a word of the settings, shown on / off
typedef struct {
    uint32_t *word;
    uint32_t mask;
    bool inverted;         // the bit set means off (e.g. "all LEDs disabled")
    void (*changed)(void); // to apply it at once; may be NULL
} ui_bit_t;

// One of a few values (0 .. count - 1) of a byte of the settings
typedef struct {
    uint8_t *value;
    const text_id_t *options;
    uint8_t count;
    void (*changed)(void);
} ui_choice_t;

// An 8-bit number of the settings
typedef struct {
    void *value; // uint8_t, or int8_t when is_signed
    bool is_signed;
    int16_t min;
    int16_t max;
    int16_t step;
    int16_t small_below; // at or below this, steps of 1; 0: always step
    const char *unit;    // after the number, e.g. "%", "s"; NULL: none
    bool zero_is_off;    // 0 is shown as "off"
    void (*changed)(void);
} ui_number_t;

// Anything else: shown by format(), changed by change() (-1 / +1, with the side buttons held), switched by select()
typedef struct {
    void (*format)(char *buffer, size_t size);
    void (*change)(int direction, uint8_t modifiers); // may be NULL
    void (*select)(void);                             // may be NULL
} ui_custom_t;

typedef struct {
    text_id_t label;
    icons_id_t icon; // ICONS_COUNT: none
    ui_item_type_t type;
    const void *target;
    void *arg;
} ui_item_t;

typedef struct {
    text_id_t title;
    const ui_item_t *items;
    uint8_t count;
    bool (*keys)(const ui_event_t *ev); // more keys of this list, before the list's own; true: taken; may be NULL
    void (*leave)(void);                // when the list is left, after the settings are saved; may be NULL
} ui_list_t;

// A list of names from the data (zones, contacts ...), pushed with ui_push(&pick_list_screen, (void *)&a_pick_list):
// UP / DOWN move, the left key picks, the right key goes back
typedef struct {
    text_id_t title;
    icons_id_t icon; // of every row; ICONS_COUNT: none
    int (*count)(void);
    int (*current)(void); // where the selection starts; may be NULL
    void (*name)(int index, char *buffer, size_t size);
    void (*pick)(int index);
} ui_pick_list_t;

extern const ui_screen_t list_screen;      // arg: the ui_list_t
extern const ui_screen_t pick_list_screen; // arg: the ui_pick_list_t
// Opens an item as the left key on it does (a screen or a list)
void ui_list_open_item(const ui_item_t *item);

// The lists' SK2 + digit (the quick keys, set by the app); true: the event was taken. NULL: none
extern bool (*ui_list_quick_item)(const ui_event_t *ev, const ui_item_t *item);
extern bool (*ui_list_quick_pick)(const ui_event_t *ev, const ui_pick_list_t *pick, int index);

extern const ui_screen_t not_yet_screen;   // arg: the text id of the title, as (void *)(uintptr_t)

// The right key of the pages under the lists: back on its release (its press went to the screen before), held: home
void list_handle_back(const ui_event_t *ev);
// The item shown on a row of LIST_VIEW_ROWS: the first item on the top row, the last on the bottom one, the selected
// one in the middle between (the highlight moves at the ends); -1: none
int list_row_index(int row, int32_t selected, int count);

// The last place in each menu: a menu opens where it was left, also after a restart (kept in a custom data block,
// written when the radio is switched off). id: the menu's (ui_list_id() of a name that stays), count: its items now
uint16_t ui_list_id(const char *name);
int32_t ui_list_recall(uint16_t id, int count);
void ui_list_remember(uint16_t id, int32_t selected);
void ui_list_places_load(void); // at start
bool ui_list_places_kept(void);     // the option: kept over a restart (off by default)
void ui_list_places_keep(bool keep);

#endif
