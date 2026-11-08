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
 * OpenDMR, UI: the list screen (docs/ui-design.md 5.6, docs/ui-design.md 6.5). The title bar with the name
 * and the position, seven rows from the first item to the last, the selected one in the middle while the list scrolls
 * (UP / DOWN wrap from the last to the first), the soft keys. UP / DOWN move (held: repeat), LEFT / RIGHT change
 * the value of a settings item (as the OpenGD77 menus), so does the rotary (it never moves), the left key selects,
 * the right key goes back (held: home). Leaving the list saves the settings when an item changed them.
 */

#include <stdio.h>
#include "widgets.h"
#include "settings.h"
#include "sound.h"
#include "list_screen.h"
#include "custom_data.h"
#include "power.h"
#include "text.h"
#include <string.h>

#define VALUE_SIZE     24
#define PICK_NAME_SIZE 48 // a 16 byte GB2312 name in UTF-8

static const ui_list_t *list(void)
{
    return (const ui_list_t *)ui_arg();
}

bool (*ui_list_quick_item)(const ui_event_t *ev, const ui_item_t *item);
bool (*ui_list_quick_pick)(const ui_event_t *ev, const ui_pick_list_t *pick, int index);

void ui_list_open_item(const ui_item_t *item)
{
    if (item->type == UI_ITEM_LIST) {
        ui_push(&list_screen, (void *)item->target);
    } else if (item->target) {
        ui_push((const ui_screen_t *)item->target, item->arg);
    } else {
        ui_push(&not_yet_screen, (void *)(uintptr_t)item->label);
    }
}

static int number_get(const ui_number_t *number)
{
    return number->is_signed ? *(const int8_t *)number->value : *(const uint8_t *)number->value;
}

static void number_change(const ui_number_t *number, int direction)
{
    int value = number_get(number);
    bool small = (number->small_below != 0) &&
                 ((direction > 0) ? (value < number->small_below) : (value <= number->small_below));
    int next = value + (direction * (small ? 1 : number->step));

    next = (next < number->min) ? number->min : ((next > number->max) ? number->max : next);
    if (next == value) {
        return;
    }
    if (number->is_signed) {
        *(int8_t *)number->value = (int8_t)next;
    } else {
        *(uint8_t *)number->value = (uint8_t)next;
    }
    if (number->changed) {
        number->changed();
    }
}

static void choice_change(const ui_choice_t *choice, int direction, bool wrap)
{
    int next = *choice->value + direction;

    if (wrap) {
        next = ((next % choice->count) + choice->count) % choice->count;
    } else if ((next < 0) || (next >= choice->count)) {
        return;
    }
    *choice->value = (uint8_t)next;
    if (choice->changed) {
        choice->changed();
    }
}

static void bit_set(const ui_bit_t *bit, bool on)
{
    bool set = (on != bit->inverted);

    if (((*bit->word & bit->mask) != 0) == set) {
        return;
    }
    *bit->word = set ? (*bit->word | bit->mask) : (*bit->word & ~bit->mask);
    if (bit->changed) {
        bit->changed();
    }
}

static bool bit_is_on(const ui_bit_t *bit)
{
    return ((*bit->word & bit->mask) != 0) != bit->inverted;
}

// LEFT (-1) / RIGHT (+1) on an item; true: it changed something to show
static bool change_value(const ui_item_t *item, int direction, uint8_t modifiers)
{
    switch (item->type) {
        case UI_ITEM_BIT:
            bit_set(item->target, (direction > 0));
            break;
        case UI_ITEM_CHOICE:
            choice_change(item->target, direction, false);
            break;
        case UI_ITEM_NUMBER:
            number_change(item->target, direction);
            break;
        case UI_ITEM_CUSTOM:
            if (((const ui_custom_t *)item->target)->change) {
                ((const ui_custom_t *)item->target)->change(direction, modifiers);
            }
            break;
        default:
            return false;
    }
    settings_changed();
    return true;
}

// The left key on an item; true: it changed something to show
static bool select_item(const ui_item_t *item)
{
    switch (item->type) {
        case UI_ITEM_SCREEN:
        case UI_ITEM_LIST:
            ui_list_open_item(item);
            return false;
        case UI_ITEM_BIT:
            bit_set(item->target, !bit_is_on(item->target));
            break;
        case UI_ITEM_CHOICE:
            choice_change(item->target, 1, true);
            break;
        case UI_ITEM_CUSTOM:
            if (!((const ui_custom_t *)item->target)->select) {
                return false;
            }
            ((const ui_custom_t *)item->target)->select();
            break;
        default:
            return false;
    }
    settings_changed();
    return true;
}

// The value on the right of the row; NULL: none
static const char *format_value(const ui_item_t *item, char *buffer, size_t size)
{
    switch (item->type) {
        case UI_ITEM_BIT:
            return text(bit_is_on(item->target) ? TEXT_ON : TEXT_OFF);
        case UI_ITEM_CHOICE: {
            const ui_choice_t *choice = item->target;

            return (*choice->value < choice->count) ? text(choice->options[*choice->value]) : "?";
        }
        case UI_ITEM_NUMBER: {
            const ui_number_t *number = item->target;
            int value = number_get(number);

            if ((value == 0) && number->zero_is_off) {
                return text(TEXT_OFF);
            }
            snprintf(buffer, size, "%d%s", value, (number->unit ? number->unit : ""));
            return buffer;
        }
        case UI_ITEM_CUSTOM:
            buffer[0] = '\0';
            ((const ui_custom_t *)item->target)->format(buffer, size);
            return buffer;
        default:
            return NULL;
    }
}

/* The places of the menus: an id (a hash of the English title, the same from one build to the next) and the item.
   Always kept while the radio is on; over a restart only with the option (general options, off by default): then in
   a custom data block of the codeplug, written at the power off when one moved (no flash write while using them) */

#define PLACES_MAX       32
#define PLACES_DATA_TYPE 0x4D454E55 // "MENU"

typedef struct {
    uint16_t id; // 0: free
    uint8_t selected;
    uint8_t reserved;
} place_t;

static struct {
    uint8_t kept; // over a restart
    uint8_t reserved[3];
    place_t places[PLACES_MAX];
} block;

#define places (block.places)

static bool places_changed = false;

static void places_save(void)
{
    if (places_changed) {
        places_changed = false;
        (void)custom_data_write(PLACES_DATA_TYPE, (const uint8_t *)&block, sizeof(block));
    }
}

void ui_list_places_load(void)
{
    if (!custom_data_read(PLACES_DATA_TYPE, (uint8_t *)&block, sizeof(block)) || (block.kept != 1)) {
        memset(&block, 0, sizeof(block)); // not kept: the menus start at their first item
    }
    power_off_save_register(places_save);
}

bool ui_list_places_kept(void)
{
    return block.kept == 1;
}

void ui_list_places_keep(bool keep)
{
    if (keep != ui_list_places_kept()) {
        block.kept = keep ? 1 : 0;
        places_changed = true; // saved at the power off: on with the places, or off
    }
}

// FNV-1a, folded to 16 bits; never 0
uint16_t ui_list_id(const char *name)
{
    uint32_t hash = 2166136261UL;

    while (*name) {
        hash = (hash ^ (uint8_t)*name++) * 16777619UL;
    }
    hash = (hash >> 16) ^ (hash & 0xFFFF);
    return (uint16_t)(hash ? hash : 1);
}

int32_t ui_list_recall(uint16_t id, int count)
{
    for (int i = 0; i < PLACES_MAX; i++) {
        if (places[i].id == id) {
            return (places[i].selected < count) ? places[i].selected : 0;
        }
    }
    return 0;
}

void ui_list_remember(uint16_t id, int32_t selected)
{
    int slot = -1;

    for (int i = 0; i < PLACES_MAX; i++) {
        if (places[i].id == id) {
            slot = i;
            break;
        }
        if ((places[i].id == 0) && (slot < 0)) {
            slot = i;
        }
    }
    if ((slot >= 0) && ((places[slot].id != id) || (places[slot].selected != (uint8_t)selected))) {
        places[slot].id = id;
        places[slot].selected = (uint8_t)selected;
        places_changed |= ui_list_places_kept(); // written only when they are kept over a restart
    }
}

static uint16_t list_id(const ui_list_t *l)
{
    return ui_list_id(text_en[l->title]);
}

// Pushed: where the menu was left
static void enter(void *arg)
{
    if (arg) {
        *ui_state() = ui_list_recall(list_id((const ui_list_t *)arg), ((const ui_list_t *)arg)->count);
    }
}

static void leave(void)
{
    ui_list_remember(list_id(list()), *ui_state());
    settings_save_if_changed();
    if (list()->leave) {
        list()->leave();
    }
}

// What all the lists do: UP / DOWN move (wrapping; on the first item its own key beep, as
// menuSystemCheckForFirstEntryAudible()), the right key goes back (held: home); true: moved
static bool navigate(const ui_event_t *ev, int32_t *selected, int count)
{
    int step = 0;

    if (((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT)) && (count > 0)) {
        step = (ev->key == UI_KEY_DOWN) ? 1 : ((ev->key == UI_KEY_UP) ? -1 : 0);
    } else if ((ev->type == UI_EVENT_KEY_LONG) && (ev->key == UI_KEY_RED)) {
        ui_home();
    } else if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_RED) && !ev->long_press) {
        ui_pop();
    }

    if (step != 0) {
        *selected = (((*selected + step) % count) + count) % count;
        if (*selected == 0) {
            ui_key_beep(SOUND_KEY_FIRST_ITEM);
        }
        return true;
    }
    return false;
}

int list_row_index(int row, int32_t selected, int count)
{
    int top;

    if (count > LIST_VIEW_ROWS) {
        top = (int)selected - (LIST_VIEW_ROWS / 2);
        top = (top < 0) ? 0 : ((top > (count - LIST_VIEW_ROWS)) ? (count - LIST_VIEW_ROWS) : top);
        return top + row;
    }
    return (row < count) ? row : -1;
}

void list_handle_back(const ui_event_t *ev)
{
    navigate(ev, NULL, 0);
}

static void draw_title(text_id_t title, int32_t selected, int count)
{
    char position[24];

    snprintf(position, sizeof(position), "%d/%d", (int)(selected + 1), count);
    list_view_draw_title(text(title), (count > 0) ? position : NULL);
}

static bool handle(const ui_event_t *ev)
{
    const ui_list_t *l = list();
    int32_t *selected = ui_state();

    if (!l || (l->count == 0)) {
        navigate(ev, selected, 0);
        return false;
    }
    if (l->keys && l->keys(ev)) {
        return false;
    }
    if (ui_list_quick_item && ui_list_quick_item(ev, &l->items[*selected])) {
        return false;
    }

    if (ev->type == UI_EVENT_ROTARY) {
        bool changed = false;

        for (int n = 0; n < ((ev->steps < 0) ? -ev->steps : ev->steps); n++) {
            changed |= change_value(&l->items[*selected], ((ev->steps > 0) ? 1 : -1), ev->modifiers);
        }
        return changed;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT)) {
        if ((ev->key == UI_KEY_LEFT) || (ev->key == UI_KEY_RIGHT)) {
            return change_value(&l->items[*selected], ((ev->key == UI_KEY_RIGHT) ? 1 : -1), ev->modifiers);
        }
        if ((ev->key == UI_KEY_GREEN) && (ev->type == UI_EVENT_KEY_PRESS)) {
            return select_item(&l->items[*selected]);
        }
    }
    return navigate(ev, selected, l->count);
}

static void draw(void)
{
    const ui_list_t *l = list();
    int32_t selected = *ui_state();

    if (!l) {
        return;
    }

    draw_title(l->title, selected, l->count);

    for (int row = 0; row < LIST_VIEW_ROWS; row++) {
        int index = list_row_index(row, selected, l->count);
        char value[VALUE_SIZE];

        if (index < 0) {
            break;
        }

        list_view_row_t data = {
            .icon = l->items[index].icon,
            .name = text(l->items[index].label),
            .value = format_value(&l->items[index], value, sizeof(value)),
            .selected = (index == selected),
        };

        list_view_draw_row(row, &data);
    }

    bottom_bar_draw_soft_keys(text(TEXT_UI_SELECT), text(TEXT_UI_BACK));
}

const ui_screen_t list_screen = {
    .enter = enter,
    .leave = leave,
    .handle = handle,
    .draw = draw,
};

// A list of names from the data (zones, contacts ...): pick one with the left key
static const ui_pick_list_t *pick_list(void)
{
    return (const ui_pick_list_t *)ui_arg();
}

static void pick_enter(void *arg)
{
    if (arg) {
        int current = pick_list()->current ? pick_list()->current() : 0;
        int count = pick_list()->count();

        *ui_state() = ((current >= 0) && (current < count)) ? current : 0;
    }
}

static bool pick_handle(const ui_event_t *ev)
{
    const ui_pick_list_t *p = pick_list();
    int32_t *selected = ui_state();
    int count = p->count();

    if ((count > 0) && ui_list_quick_pick && ui_list_quick_pick(ev, p, *selected)) {
        return false;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN) && (count > 0)) {
        p->pick(*selected);
        return false;
    }
    return navigate(ev, selected, count);
}

static void pick_draw(void)
{
    const ui_pick_list_t *p = pick_list();
    int32_t selected = *ui_state();
    int count = p->count();

    draw_title(p->title, selected, count);

    for (int row = 0; row < LIST_VIEW_ROWS; row++) {
        int index = list_row_index(row, selected, count);
        char name[PICK_NAME_SIZE];

        if (index < 0) {
            break;
        }
        p->name(index, name, sizeof(name));

        list_view_row_t data = {
            .icon = p->icon,
            .name = name,
            .selected = (index == selected),
        };

        list_view_draw_row(row, &data);
    }

    bottom_bar_draw_soft_keys(text(TEXT_UI_SELECT), text(TEXT_UI_BACK));
}

const ui_screen_t pick_list_screen = {
    .enter = pick_enter,
    .handle = pick_handle,
    .draw = pick_draw,
};

// A page for what is not there yet: the title, "not yet", back
// (back on the release of the right key, as the lists: its press already went to this screen's caller otherwise)
static bool not_yet_handle(const ui_event_t *ev)
{
    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN)) {
        ui_pop();
    } else {
        list_handle_back(ev);
    }
    return false;
}

static void not_yet_draw(void)
{
    list_view_draw_title(text((text_id_t)(uintptr_t)ui_arg()), NULL);
    list_view_draw_text(text(TEXT_NOT_YET), NULL, THEME_COLOUR_TEXT_LABEL);
    bottom_bar_draw_soft_keys(NULL, text(TEXT_UI_BACK));
}

const ui_screen_t not_yet_screen = {
    .handle = not_yet_handle,
    .draw = not_yet_draw,
};
