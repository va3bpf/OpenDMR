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

#ifndef DM32_KEYMAP_H
#define DM32_KEYMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "text.h"
#include "ui.h"

// The shortcuts of the channel and VFO screens, defined in one table (keymap.c): an action, the key, the side keys
// held with it, short or long press. The defaults are the OpenGD77 keys; the key settings (key_settings.c) change them,
// kept in a custom data block of the codeplug. UP / DOWN and the rotary alone, the digits, the menu and back keys (the
// left and right keys under the screen) and PTT aren't in it.

typedef enum {
    KEYMAP_NONE = 0,
    KEYMAP_NUMBER_ENTRY,       // #: TG / PC / contact / own ID entry (DMR)
    KEYMAP_CONTACT_LIST,       // SK2 + #: the contact quick list
    KEYMAP_REVERSE_REPEATER,   // # held: RX and TX frequencies swapped (channel)
    KEYMAP_TS_OR_BANDWIDTH,    // *: DMR timeslot, FM 12.5 / 25 kHz
    KEYMAP_TS_CLEAR,           // * held: the timeslot of the contact or channel again
    KEYMAP_MODE,               // SK1 + *: FM / DMR (SK2 + * can't be read on the DM-32)
    KEYMAP_TG_OR_SQUELCH_UP,   // RIGHT: DMR next TG of the RX group, FM squelch up
    KEYMAP_TG_OR_SQUELCH_DOWN, // LEFT
    KEYMAP_POWER_UP,           // SK2 + RIGHT
    KEYMAP_POWER_DOWN,         // SK2 + LEFT
    KEYMAP_POWER_USER,         // SK2 + RIGHT held: the user power
    KEYMAP_ZONE_UP,            // SK2 + UP (channel)
    KEYMAP_ZONE_DOWN,          // SK2 + DOWN (channel)
    KEYMAP_TX_UP,              // SK2 + UP (VFO): the TX frequency one step up (UP / DOWN: the RX frequency)
    KEYMAP_TX_DOWN,            // SK2 + DOWN (VFO)
    KEYMAP_TG_FROM_CALL,       // SK2 while a call is shown: transmit to its TG and timeslot
    KEYMAP_FM_RADIO,           // SK1 alone: the FM broadcast radio (as the main firmware on the DM-32)
    KEYMAP_SCAN,               // SK1 + UP: scan the zone, or the VFO's range (OpenGD77: UP held, here it steps)
    KEYMAP_SCAN_DOWN,          // SK1 + DOWN: the same, downwards
    KEYMAP_APRS_TOGGLE,        // SK1 + 1: APRS beaconing on / off (OpenGD77)
    KEYMAP_APRS_SEND,          // SK1 + 3: an APRS beacon now (2 is in SK1's column, SK2 + 2 a quick key)
    KEYMAP_ACTIONS
} keymap_action_t;

#define KEYMAP_SCREEN_CHANNEL 0x01
#define KEYMAP_SCREEN_VFO     0x02

typedef struct {
    uint8_t key;       // UI_KEY_x; UI_KEY_COUNT: none
    uint8_t modifiers; // UI_MODIFIER_x held with it (not the key itself)
    uint8_t long_press;
    uint8_t reserved;
} keymap_binding_t;

void keymap_load(void); // the stored bindings, else the defaults

// The action of a key event on a screen (KEYMAP_SCREEN_x); KEYMAP_NONE: not a shortcut. A short press acts on the
// press (and repeats when held) when the same keys held aren't also a long press shortcut, else on the release; SK1
// or SK2 alone on its release, when no other key was pressed with it. The screens give it all their key events.
keymap_action_t keymap_action(const ui_event_t *ev, uint8_t screen);

text_id_t keymap_action_name(keymap_action_t action);
uint8_t keymap_action_screens(keymap_action_t action);
keymap_binding_t keymap_binding(keymap_action_t action);
void keymap_format(keymap_binding_t binding, char *out, size_t size); // "SK2 + #", "# (long)"; "-": none
// Sets an action's keys; another action of the same screens on these keys loses them (KEYMAP_NONE: none did)
keymap_action_t keymap_set(keymap_action_t action, keymap_binding_t binding);
void keymap_defaults(void);
bool keymap_save(void);

#endif
