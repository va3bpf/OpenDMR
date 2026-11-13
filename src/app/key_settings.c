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
 * OpenDMR, screen: the key settings (options, "按键设置"): the shortcuts of keymap.c, one row an action with its
 * keys. The left key on a row waits for the new keys: the side keys held with a key, a short or long press (the key's
 * release or its long press time); SK1 or SK2 alone too (short). The right key there keeps the old ones. The menu, back and
 * PTT keys can't be taken, nor UP / DOWN and the digits without a side key (they move and enter on the screens).
 * Keys another action had are taken from it (shown). The last row puts the defaults (OpenGD77) back. Saved when the
 * screen is left.
 */

#include <stdio.h>
#include "widgets.h"
#include "list_screen.h"
#include "sound.h"
#include "keymap.h"
#include "key_settings.h"

#define ROWS        (KEYMAP_ACTIONS + 1) // the actions (from 1), then the defaults
#define SIDE_KEYS   (UI_MODIFIER_SK1 | UI_MODIFIER_SK2)

static struct {
    bool capturing;
    bool changed;
    bool green_down;
    uint8_t key;       // the key being pressed; UI_KEY_COUNT: none yet
    uint8_t modifiers; // the side keys held with it
    keymap_action_t lost;
} state;

static void enter(void *arg)
{
    if (arg != NULL) {
        state.capturing = false;
        state.changed = false;
        state.lost = KEYMAP_NONE;
    }
}

static void leave(void)
{
    if (state.changed) {
        state.changed = false;
        keymap_save();
    }
}

static bool is_side(uint8_t key)
{
    return (key == UI_KEY_SK1) || (key == UI_KEY_SK2);
}

static void take(int32_t selected, bool long_press)
{
    keymap_binding_t binding = {.key = state.key, .modifiers = state.modifiers, .long_press = long_press};
    bool movement = (state.key == UI_KEY_UP) || (state.key == UI_KEY_DOWN) || (ui_key_digit(state.key) >= 0);

    state.capturing = false;
    if ((state.key == UI_KEY_GREEN) || (state.key == UI_KEY_PTT) || (movement && (state.modifiers == 0))) {
        ui_key_beep(SOUND_ERROR);
        return;
    }
    state.lost = keymap_set((keymap_action_t)selected, binding);
    state.changed = true;
    ui_key_beep(SOUND_ACK);
}

static bool capture(const ui_event_t *ev, int32_t selected)
{
    uint8_t own = (ev->key == UI_KEY_SK1) ? UI_MODIFIER_SK1 : ((ev->key == UI_KEY_SK2) ? UI_MODIFIER_SK2 : 0);

    switch (ev->type) {
        case UI_EVENT_KEY_PRESS:
            if (is_side(ev->key) && (state.key == UI_KEY_COUNT)) {
                state.key = ev->key; // alone, unless a key follows
                state.modifiers = 0;
            } else if (!is_side(ev->key)) {
                state.key = ev->key;
                state.modifiers = (uint8_t)(ev->modifiers & SIDE_KEYS & ~own);
            }
            return true;
        case UI_EVENT_KEY_LONG: // not a side key alone: it is held long before the key that comes with it
            if ((ev->key == state.key) && !is_side(ev->key)) {
                take(selected, true);
            }
            return true;
        case UI_EVENT_KEY_RELEASE:
            if ((ev->key != state.key) || ev->long_press) {
                return false;
            }
            if (ev->key == UI_KEY_RED) {
                state.capturing = false; // keeps the old keys
            } else {
                take(selected, false);
            }
            return true;
        default:
            return false;
    }
}

static bool handle(const ui_event_t *ev)
{
    int32_t *selected = ui_state();

    if (*selected == 0) {
        *selected = 1; // no row for KEYMAP_NONE
    }
    if (state.capturing) {
        return capture(ev, *selected);
    }
    if ((ev->type == UI_EVENT_ROTARY) ||
        (((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT)) &&
         ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN)))) {
        int step = (ev->type == UI_EVENT_ROTARY) ? ev->steps : ((ev->key == UI_KEY_DOWN) ? 1 : -1);

        *selected = ((((*selected - 1) + step) % (ROWS - 1)) + (ROWS - 1)) % (ROWS - 1) + 1;
        state.lost = KEYMAP_NONE;
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN)) {
        state.green_down = true;
        return false;
    }
    if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_GREEN) && state.green_down) {
        state.green_down = false;
        state.lost = KEYMAP_NONE;
        if (*selected == (ROWS - 1)) {
            keymap_defaults();
            state.changed = true;
            ui_key_beep(SOUND_ACK);
        } else {
            state.capturing = true;
            state.key = UI_KEY_COUNT;
        }
        return true;
    }
    list_handle_back(ev);
    return false;
}

static void draw(void)
{
    int32_t selected = (*ui_state() == 0) ? 1 : *ui_state();
    char position[16];

    snprintf(position, sizeof(position), "%d/%d", (int)selected, ROWS - 1);
    list_view_draw_title(text(TEXT_KEY_SETTINGS), position);

    for (int row = 0; row < LIST_VIEW_ROWS; row++) {
        int at = list_row_index(row, selected - 1, ROWS - 1);
        char value[32];

        if (at < 0) {
            break;
        }

        list_view_row_t data = {.icon = ICONS_COUNT, .selected = ((at + 1) == selected)};

        if ((at + 1) == (ROWS - 1)) {
            data.name = text(TEXT_KEY_DEFAULTS);
        } else {
            keymap_action_t action = (keymap_action_t)(at + 1);

            data.name = text(keymap_action_name(action));
            if (data.selected && state.capturing) {
                data.value = text(TEXT_KEY_PRESS_NEW);
            } else {
                keymap_format(keymap_binding(action), value, sizeof(value));
                data.value = value;
            }
            data.dimmed = (keymap_binding(action).key >= UI_KEY_COUNT);
        }
        list_view_draw_row(row, &data);
    }

    if (state.lost != KEYMAP_NONE) {
        char line[64];

        snprintf(line, sizeof(line), "%s: %s", text(TEXT_KEY_TAKEN_FROM), text(keymap_action_name(state.lost)));
        bottom_bar_draw_soft_keys(line, NULL);
    } else if (state.capturing) {
        bottom_bar_draw_soft_keys(NULL, text(TEXT_UI_CANCEL));
    } else {
        bottom_bar_draw_soft_keys(text(TEXT_UI_EDIT), text(TEXT_UI_BACK));
    }
}

const ui_screen_t key_settings_screen = {
    .enter = enter,
    .leave = leave,
    .handle = handle,
    .draw = draw,
};
