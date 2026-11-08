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
 * OpenDMR: the quick keys (quick_keys.h). From applicationMain.c (SK2 + a digit on the channel and VFO
 * screens), uiUtilities.c saveQuickkeyMenuIndex() / saveQuickkeyMenuLongValue() / saveQuickkeyContactIndex() and
 * codeplug.c codeplugSetQuickkeyFunctionID().
 *
 * Ten 16 bit values at 0x7524 of the codeplug's EEPROM part, one a digit. Bit 15 clear: a contact index (1 .. 1024).
 * Bit 15 set: a menu (bits 14 - 10, the MENU_x number of the main firmware) and its entry (bits 9 - 0); the zone list
 * with an entry goes to that zone (entry - 1), another menu just opens. 0x8000, 0xFFFF, 0: none. Storing overwrites.
 */

#include "codeplug.h"
#include "flash.h"
#include "sound.h"
#include "contacts.h"
#include "menus.h"
#include "quick_keys.h"

#define ADDR_QUICK_KEYS 0x7524
#define QUICK_KEY_MENU  0x8000
#define MENU_OF(v)      (((v) >> 10) & 0x1F)
#define ENTRY_OF(v)     ((v) & 0x3FF)
#define MENU_ZONE_LIST  2 // the main firmware's MENU_ZONE_LIST

// SK2 + a digit: its digit, else -1. The press (and its repeats) is taken, the release runs or stores
static int sk2_digit(const ui_event_t *ev, bool *release)
{
    int digit = ui_key_digit(ev->key);

    if ((digit < 0) || !(ev->modifiers & UI_MODIFIER_SK2) ||
        ((ev->type != UI_EVENT_KEY_PRESS) && (ev->type != UI_EVENT_KEY_REPEAT) && (ev->type != UI_EVENT_KEY_LONG) &&
         (ev->type != UI_EVENT_KEY_RELEASE))) {
        return -1;
    }
    *release = (ev->type == UI_EVENT_KEY_RELEASE);
    return digit;
}

static uint16_t read_key(int digit)
{
    uint16_t value;

    if (!flash_read(ADDR_QUICK_KEYS + (digit * sizeof(value)), (uint8_t *)&value, sizeof(value))) {
        return 0;
    }
    return value;
}

static void store(int digit, uint16_t value)
{
    ui_key_beep(flash_write(ADDR_QUICK_KEYS + (digit * sizeof(value)), (const uint8_t *)&value, sizeof(value))
                    ? SOUND_ACK
                    : SOUND_ERROR);
}

static bool run_contact(int index)
{
    static codeplug_contact_t contact; // not on the UI task stack

    if ((index < 1) || (index > CODEPLUG_CONTACTS_MAX) || !codeplug_get_contact(index, &contact)) {
        return false;
    }
    contacts_set_tx(contact.id, (contact.call_type == CODEPLUG_CALL_PRIVATE), codeplug_contact_timeslot(&contact));
    return true;
}

static bool run_menu(int menu, int entry)
{
    const ui_item_t *item;

    if ((menu == MENU_ZONE_LIST) && (entry > 0)) {
        if (entry > zone_list.count()) {
            return false;
        }
        zone_list.pick(entry - 1);
        return true;
    }
    item = menus_quick_item(menu);
    if (!item) {
        return false;
    }
    ui_list_open_item(item);
    return true;
}

void quick_keys_init(void)
{
    ui_list_quick_item = quick_keys_store_item;
    ui_list_quick_pick = quick_keys_store_pick;
}

bool quick_keys_home(const ui_event_t *ev)
{
    bool release;
    int digit = sk2_digit(ev, &release);
    uint16_t value;
    bool done;

    if (digit < 0) {
        return false;
    }
    if (!release || ev->long_press) {
        return true;
    }
    value = read_key(digit);
    if ((value == QUICK_KEY_MENU) || (value == 0xFFFF) || (value == 0)) {
        done = false;
    } else if (value & QUICK_KEY_MENU) {
        done = run_menu(MENU_OF(value), ENTRY_OF(value));
    } else {
        done = run_contact(value);
    }
    ui_key_beep(done ? SOUND_ACK : SOUND_ERROR);
    return true;
}

bool quick_keys_store_item(const ui_event_t *ev, const ui_item_t *item)
{
    bool release;
    int digit = sk2_digit(ev, &release);
    int menu;

    if (digit < 0) {
        return false;
    }
    if (release && !ev->long_press) {
        menu = menus_quick_id(item);
        if (menu < 0) {
            ui_key_beep(SOUND_ERROR);
        } else {
            store(digit, (uint16_t)(QUICK_KEY_MENU | (menu << 10)));
        }
    }
    return true;
}

bool quick_keys_store_pick(const ui_event_t *ev, const ui_pick_list_t *pick, int index)
{
    bool release;
    int digit = sk2_digit(ev, &release);

    if (digit < 0) {
        return false;
    }
    if (release && !ev->long_press) {
        if (pick == &zone_list) {
            store(digit, (uint16_t)(QUICK_KEY_MENU | (MENU_ZONE_LIST << 10) | ((index + 1) & 0x3FF)));
        } else {
            ui_key_beep(SOUND_ERROR);
        }
    }
    return true;
}

bool quick_keys_store_contact(const ui_event_t *ev, int index)
{
    bool release;
    int digit = sk2_digit(ev, &release);

    if (digit < 0) {
        return false;
    }
    if (release && !ev->long_press) {
        store(digit, (uint16_t)index);
    }
    return true;
}
