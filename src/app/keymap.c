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
 * OpenDMR: the shortcuts of the channel and VFO screens (keymap.h). The defaults are the OpenGD77 keys
 * (uiChannelMode.c, uiVFOMode.c handleEvent()), with SK1 + * for FM / DMR: SK2 shares its line with the keypad
 * column of 1, 4, 7 and *, which isn't read while SK2 is down. In the same way SK1 holds down the column of 2, 5,
 * 8 and 0, so the APRS beacon is SK2 + 2 and not SK1 + 2 (which the keyboard can never read).
 */

#include <stdio.h>
#include <string.h>
#include "custom_data.h"
#include "keymap.h"

#define KEYMAP_DATA_TYPE 0x44540002 // a custom data block of this firmware ("DT", 2)
#define KEYMAP_VERSION   6 // 2: KEYMAP_FM_RADIO, 3: KEYMAP_SCAN, 4: KEYMAP_SCAN_DOWN, 5: the APRS keys, 6: SK2 + 2
#define KEYMAP_SLOTS     32 // room for more actions: a block keeps its size
#define SIDE_KEYS        (UI_MODIFIER_SK1 | UI_MODIFIER_SK2)
#define NO_KEY           {UI_KEY_COUNT, 0, 0, 0}

typedef struct {
    text_id_t name;
    uint8_t screens;
    bool repeats; // a short press held repeats it
    keymap_binding_t binding;
} action_t;

#define BOTH    (KEYMAP_SCREEN_CHANNEL | KEYMAP_SCREEN_VFO)
#define SK1     UI_MODIFIER_SK1
#define SK2     UI_MODIFIER_SK2

static const action_t actions[KEYMAP_ACTIONS] = {
    [KEYMAP_NONE] = {TEXT_NONE, 0, false, NO_KEY},
    [KEYMAP_NUMBER_ENTRY] = {TEXT_KEY_NUMBER_ENTRY, BOTH, false, {UI_KEY_HASH, 0, 0, 0}},
    [KEYMAP_CONTACT_LIST] = {TEXT_KEY_CONTACT_LIST, BOTH, false, {UI_KEY_HASH, SK2, 0, 0}},
    [KEYMAP_REVERSE_REPEATER] = {TEXT_KEY_REVERSE, KEYMAP_SCREEN_CHANNEL, false, {UI_KEY_HASH, 0, 1, 0}},
    [KEYMAP_TS_OR_BANDWIDTH] = {TEXT_KEY_TS_BANDWIDTH, BOTH, false, {UI_KEY_STAR, 0, 0, 0}},
    [KEYMAP_TS_CLEAR] = {TEXT_KEY_TS_CLEAR, BOTH, false, {UI_KEY_STAR, 0, 1, 0}},
    [KEYMAP_MODE] = {TEXT_KEY_MODE, BOTH, false, {UI_KEY_STAR, SK1, 0, 0}},
    [KEYMAP_TG_OR_SQUELCH_UP] = {TEXT_KEY_TG_SQUELCH_UP, BOTH, true, {UI_KEY_RIGHT, 0, 0, 0}},
    [KEYMAP_TG_OR_SQUELCH_DOWN] = {TEXT_KEY_TG_SQUELCH_DOWN, BOTH, true, {UI_KEY_LEFT, 0, 0, 0}},
    [KEYMAP_POWER_UP] = {TEXT_KEY_POWER_UP, BOTH, true, {UI_KEY_RIGHT, SK2, 0, 0}},
    [KEYMAP_POWER_DOWN] = {TEXT_KEY_POWER_DOWN, BOTH, true, {UI_KEY_LEFT, SK2, 0, 0}},
    [KEYMAP_POWER_USER] = {TEXT_KEY_POWER_USER, BOTH, false, {UI_KEY_RIGHT, SK2, 1, 0}},
    [KEYMAP_ZONE_UP] = {TEXT_KEY_ZONE_UP, KEYMAP_SCREEN_CHANNEL, true, {UI_KEY_UP, SK2, 0, 0}},
    [KEYMAP_ZONE_DOWN] = {TEXT_KEY_ZONE_DOWN, KEYMAP_SCREEN_CHANNEL, true, {UI_KEY_DOWN, SK2, 0, 0}},
    [KEYMAP_TX_UP] = {TEXT_KEY_TX_UP, KEYMAP_SCREEN_VFO, true, {UI_KEY_UP, SK2, 0, 0}},
    [KEYMAP_TX_DOWN] = {TEXT_KEY_TX_DOWN, KEYMAP_SCREEN_VFO, true, {UI_KEY_DOWN, SK2, 0, 0}},
    [KEYMAP_TG_FROM_CALL] = {TEXT_KEY_TG_FROM_CALL, BOTH, false, {UI_KEY_SK2, 0, 0, 0}},
    [KEYMAP_FM_RADIO] = {TEXT_FM_RADIO, BOTH, false, {UI_KEY_SK1, 0, 0, 0}},
    [KEYMAP_SCAN] = {TEXT_KEY_SCAN_UP, BOTH, false, {UI_KEY_UP, SK1, 0, 0}},
    [KEYMAP_SCAN_DOWN] = {TEXT_KEY_SCAN_DOWN, BOTH, false, {UI_KEY_DOWN, SK1, 0, 0}},
    [KEYMAP_APRS_TOGGLE] = {TEXT_KEY_APRS_TOGGLE, BOTH, false, {UI_KEY_1, SK1, 0, 0}},
    [KEYMAP_APRS_SEND] = {TEXT_KEY_APRS_SEND, BOTH, false, {UI_KEY_2, SK2, 0, 0}},
};

static struct {
    uint32_t version;
    keymap_binding_t bindings[KEYMAP_SLOTS];
} stored;

static uint8_t press_modifiers[UI_KEY_COUNT]; // the side keys held when the key went down
static bool side_combined[2];                 // SK1, SK2: another key was pressed while it was held

static bool is_side(uint8_t key)
{
    return (key == UI_KEY_SK1) || (key == UI_KEY_SK2);
}

void keymap_defaults(void)
{
    stored.version = KEYMAP_VERSION;
    for (int i = 0; i < KEYMAP_SLOTS; i++) {
        stored.bindings[i] = (i < KEYMAP_ACTIONS) ? actions[i].binding : (keymap_binding_t)NO_KEY;
    }
}

void keymap_load(void)
{
    if (!custom_data_read(KEYMAP_DATA_TYPE, (uint8_t *)&stored, sizeof(stored))) {
        keymap_defaults();
        return;
    }
    if ((stored.version == 0) || (stored.version > KEYMAP_VERSION)) {
        keymap_defaults();
        return;
    }
    // Saved before an action came: its default keys, unless an action has them
    if (stored.version < 2) {
        (void)keymap_set(KEYMAP_FM_RADIO, actions[KEYMAP_FM_RADIO].binding);
    }
    if (stored.version < 3) {
        (void)keymap_set(KEYMAP_SCAN, actions[KEYMAP_SCAN].binding);
    }
    if (stored.version < 4) {
        (void)keymap_set(KEYMAP_SCAN_DOWN, actions[KEYMAP_SCAN_DOWN].binding);
    }
    if (stored.version < 5) {
        (void)keymap_set(KEYMAP_APRS_TOGGLE, actions[KEYMAP_APRS_TOGGLE].binding);
        (void)keymap_set(KEYMAP_APRS_SEND, actions[KEYMAP_APRS_SEND].binding);
    }
    if (stored.version < 6) {
        // SK1 + 2 could never be read (2 is in the column SK1 holds down): the beacon now is SK2 + 2
        (void)keymap_set(KEYMAP_APRS_SEND, actions[KEYMAP_APRS_SEND].binding);
    }
    stored.version = KEYMAP_VERSION;
}

bool keymap_save(void)
{
    return custom_data_write(KEYMAP_DATA_TYPE, (const uint8_t *)&stored, sizeof(stored));
}

static keymap_action_t find(uint8_t key, uint8_t modifiers, bool long_press, uint8_t screen)
{
    for (int i = 1; i < KEYMAP_ACTIONS; i++) {
        const keymap_binding_t *b = &stored.bindings[i];

        if ((actions[i].screens & screen) && (b->key == key) && (b->modifiers == modifiers) &&
            ((b->long_press != 0) == long_press)) {
            return (keymap_action_t)i;
        }
    }
    return KEYMAP_NONE;
}

keymap_action_t keymap_action(const ui_event_t *ev, uint8_t screen)
{
    uint8_t key = ev->key;
    uint8_t own = (key == UI_KEY_SK1) ? UI_MODIFIER_SK1 : ((key == UI_KEY_SK2) ? UI_MODIFIER_SK2 : 0);

    if (key >= UI_KEY_COUNT) {
        return KEYMAP_NONE;
    }
    if (ev->type == UI_EVENT_KEY_PRESS) {
        press_modifiers[key] = (uint8_t)(ev->modifiers & SIDE_KEYS & ~own);
        if (is_side(key)) {
            side_combined[key - UI_KEY_SK1] = false;
        }
        side_combined[0] |= (press_modifiers[key] & UI_MODIFIER_SK1) != 0; // the side keys held with this one
        side_combined[1] |= (press_modifiers[key] & UI_MODIFIER_SK2) != 0;
    }

    uint8_t modifiers = press_modifiers[key];
    keymap_action_t short_action = find(key, modifiers, false, screen);
    bool has_long = (find(key, modifiers, true, screen) != KEYMAP_NONE);

    switch (ev->type) {
        case UI_EVENT_KEY_PRESS:
            return (!has_long && !is_side(key)) ? short_action : KEYMAP_NONE;
        case UI_EVENT_KEY_REPEAT:
            return (!has_long && !is_side(key) && actions[short_action].repeats) ? short_action : KEYMAP_NONE;
        case UI_EVENT_KEY_LONG:
            return find(key, modifiers, true, screen);
        case UI_EVENT_KEY_RELEASE:
            if (ev->long_press) {
                return KEYMAP_NONE;
            }
            if (is_side(key)) {
                return side_combined[key - UI_KEY_SK1] ? KEYMAP_NONE : short_action;
            }
            return has_long ? short_action : KEYMAP_NONE;
        default:
            return KEYMAP_NONE;
    }
}

text_id_t keymap_action_name(keymap_action_t action)
{
    return actions[action].name;
}

uint8_t keymap_action_screens(keymap_action_t action)
{
    return actions[action].screens;
}

keymap_binding_t keymap_binding(keymap_action_t action)
{
    return stored.bindings[action];
}

static const char *key_name(uint8_t key)
{
    static const char *const names[UI_KEY_COUNT] = {
        [UI_KEY_GREEN] = "OK", [UI_KEY_RED] = "BACK", [UI_KEY_HASH] = "#",   [UI_KEY_STAR] = "*",
        [UI_KEY_0] = "0",      [UI_KEY_1] = "1",      [UI_KEY_2] = "2",      [UI_KEY_3] = "3",
        [UI_KEY_4] = "4",      [UI_KEY_5] = "5",      [UI_KEY_6] = "6",      [UI_KEY_7] = "7",
        [UI_KEY_8] = "8",      [UI_KEY_9] = "9",      [UI_KEY_SK1] = "SK1",  [UI_KEY_SK2] = "SK2",
        [UI_KEY_PTT] = "PTT",
    };

    switch (key) {
        case UI_KEY_UP:
            return text(TEXT_KEY_NAME_UP);
        case UI_KEY_DOWN:
            return text(TEXT_KEY_NAME_DOWN);
        case UI_KEY_LEFT:
            return text(TEXT_KEY_NAME_LEFT);
        case UI_KEY_RIGHT:
            return text(TEXT_KEY_NAME_RIGHT);
        default:
            return (key < UI_KEY_COUNT) ? names[key] : "-";
    }
}

void keymap_format(keymap_binding_t binding, char *out, size_t size)
{
    if (binding.key >= UI_KEY_COUNT) {
        snprintf(out, size, "-");
        return;
    }
    snprintf(out, size, "%s%s%s%s%s", ((binding.modifiers & UI_MODIFIER_SK1) ? "SK1+" : ""),
             ((binding.modifiers & UI_MODIFIER_SK2) ? "SK2+" : ""), key_name(binding.key),
             (binding.long_press ? " " : ""), (binding.long_press ? text(TEXT_KEY_LONG) : ""));
}

keymap_action_t keymap_set(keymap_action_t action, keymap_binding_t binding)
{
    keymap_action_t lost = KEYMAP_NONE;

    if (binding.key < UI_KEY_COUNT) {
        for (int i = 1; i < KEYMAP_ACTIONS; i++) {
            const keymap_binding_t *b = &stored.bindings[i];

            if ((i != action) && (actions[i].screens & actions[action].screens) && (b->key == binding.key) &&
                (b->modifiers == binding.modifiers) && ((b->long_press != 0) == (binding.long_press != 0))) {
                stored.bindings[i] = (keymap_binding_t)NO_KEY;
                lost = (keymap_action_t)i;
            }
        }
    }
    stored.bindings[action] = binding;
    return lost;
}
