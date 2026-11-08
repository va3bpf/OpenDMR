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
 * OpenDMR, screens: the contacts (menuContactList.c, menuContactDetails.c, the contacts menu of
 * menuSystem.c, docs/ui-design.md 5.6).
 *
 * The contacts menu: DMR contacts, FM DTMF contacts, new contact. The contact list shows one call type at a time
 * (# changes it: group, private, all call) or the DTMF contacts; the left key opens the actions of the selected one
 * (select for transmit, edit, delete), or, in the quick list, selects it for transmit at once. Deleting asks first, and
 * refuses a talk group used by an RX group list. The details edit a contact in place in the rows: the name (tapping
 * letters, text_entry.c), the number, the call type, the timeslot override; the left key saves it in the codeplug.
 * The question and the results are message boxes (message_box.h); the results go back after RESULT_MS, or with a key.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "widgets.h"
#include "codeplug_text.h"
#include "settings.h"
#include "codeplug.h"
#include "text_entry.h"
#include "message_box.h"
#include "tx_target.h"
#include "tx_screen.h"
#include "quick_keys.h"
#include "sound.h"
#include "contacts.h"

#define RESULT_MS        2000 // menuContactListTimeout, menuContactDetailsTimeout
#define NAME_SIZE        48   // a 16 byte GB2312 name in UTF-8
#define VALUE_SIZE       40
#define DETAILS_NEW      ((void *)(uintptr_t)(CODEPLUG_CONTACTS_MAX + 1)) // the details' arg for a new contact

static const ui_screen_t actions_screen;

// setOverrideTGorPC(), tsSetFromContactOverride(): for the channel or VFO at home (tx_target.c)
void contacts_set_tx(uint32_t id, bool private_call, int timeslot)
{
    tx_target_set(id, private_call);
    if (timeslot > 0) {
        tx_target_set_timeslot(timeslot);
    }
    settings_save_if_changed();
}

// Sent on the channel of the home screen (FM only), then back to the list
static void send_dtmf(const uint8_t *code)
{
    if (!tx_screen_send_dtmf(code)) {
        ui_key_beep(SOUND_ERROR);
    }
}

// ---- The contact list ----

typedef struct {
    bool dtmf;
    bool quick; // the quick list: the left key selects for transmit
} list_kind_t;

static const list_kind_t dmr_list = {.dtmf = false};
static const list_kind_t dtmf_list = {.dtmf = true};
static const list_kind_t dmr_quick_list = {.dtmf = false, .quick = true};
static const list_kind_t dtmf_quick_list = {.dtmf = true, .quick = true};

static struct {
    const list_kind_t *kind;
    int call_type;
} list = {.kind = &dmr_list};

// The delete question and the results (message_box.h): static while they show
static char box_title[NAME_SIZE];
static message_box_t question;
static message_box_t result;

// The selected contact, for the actions and the delete (static: not on the UI task stack)
static codeplug_contact_t contact;
static codeplug_dtmf_contact_t dtmf_contact;

static int list_count(void)
{
    return list.kind->dtmf ? codeplug_dtmf_contact_count() : codeplug_contact_count(list.call_type);
}

static const char *list_title(void)
{
    static const text_id_t titles[CODEPLUG_CALL_TYPES] = {TEXT_GROUP_CALL, TEXT_PRIVATE_CALL, TEXT_ALL_CALL};

    return list.kind->dtmf ? "DTMF" : text(titles[list.call_type]);
}

// The contact at a position of the list into contact / dtmf_contact; false: none
static bool read_at(int position)
{
    if (list.kind->dtmf) {
        return codeplug_get_dtmf_contact(position, &dtmf_contact);
    }
    return codeplug_get_contact(codeplug_contact_at(position, list.call_type), &contact);
}

static void name_text(const char *name, char *buffer, size_t size)
{
    codeplug_text_name(name, CODEPLUG_NAME_LENGTH, buffer, size);
}

static void selected_name(char *buffer, size_t size)
{
    name_text(list.kind->dtmf ? dtmf_contact.name : contact.name, buffer, size);
}

// The selection back in the list after a change of its length (as reloadContactList())
static void list_clamp(void)
{
    int32_t *selected = ui_state();

    if (*selected >= list_count()) {
        *selected = 0;
    }
}

static void list_enter(void *arg)
{
    if (arg) {
        list.kind = arg;
        list.call_type = CODEPLUG_CALL_GROUP;
        return;
    }
    list_clamp(); // back from the actions, the details or a message: a contact may be gone
}

static void select_for_tx(void)
{
    if (list.kind->dtmf) {
        send_dtmf(dtmf_contact.code);
        return;
    }
    contacts_set_tx(contact.id, (contact.call_type == CODEPLUG_CALL_PRIVATE), codeplug_contact_timeslot(&contact));
    ui_home();
}

static bool list_handle_show(const ui_event_t *ev)
{
    int32_t *selected = ui_state();
    int count = list_count();

    if ((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT)) {
        if (((ev->key == UI_KEY_DOWN) || (ev->key == UI_KEY_UP)) && (count > 0)) {
            *selected = (((*selected + ((ev->key == UI_KEY_DOWN) ? 1 : -1)) % count) + count) % count;
            return true;
        }
    }
    if (!list.kind->dtmf && (count > 0) && (ui_key_digit(ev->key) >= 0) && (ev->modifiers & UI_MODIFIER_SK2)) {
        if ((ev->type == UI_EVENT_KEY_RELEASE) && !read_at(*selected)) {
            return false;
        }
        quick_keys_store_contact(ev, contact.index); // SK2 + digit: a quick key to it
        return false;
    }
    if (ev->type == UI_EVENT_KEY_PRESS) {
        if ((ev->key == UI_KEY_HASH) && !list.kind->dtmf) {
            list.call_type = (list.call_type + 1) % CODEPLUG_CALL_TYPES;
            *selected = 0;
            return true;
        }
        if ((ev->key == UI_KEY_GREEN) && (count > 0) && read_at(*selected)) {
            if (list.kind->quick) {
                select_for_tx();
            } else {
                ui_push(&actions_screen, NULL);
            }
            return false;
        }
    }
    list_handle_back(ev);
    return false;
}

static void list_draw_show(void)
{
    int32_t selected = *ui_state();
    int count = list_count();
    char position[24];

    snprintf(position, sizeof(position), "%d/%d", (int)(selected + 1), count);
    list_view_draw_title(list_title(), ((count > 0) ? position : NULL));
    bottom_bar_draw_soft_keys(text(TEXT_UI_SELECT), text(TEXT_UI_BACK));

    if (count == 0) {
        list_view_row_t row = {.icon = ICONS_COUNT, .name = text(TEXT_LIST_EMPTY), .dimmed = true};

        list_view_draw_row(0, &row);
        return;
    }

    // Read into the selected contact's buffers, read again from the selection when it is used
    for (int row = 0; row < LIST_VIEW_ROWS; row++) {
        int index = list_row_index(row, selected, count);
        char name[NAME_SIZE];

        if (index < 0) {
            break;
        }
        if (!read_at(index)) {
            continue;
        }
        selected_name(name, sizeof(name));

        list_view_row_t data = {.icon = ICONS_COUNT, .name = name, .selected = (index == selected)};

        list_view_draw_row(row, &data);
    }
}

const ui_screen_t contact_list_screen = {
    .enter = list_enter,
    .handle = list_handle_show,
    .draw = list_draw_show,
};

void contacts_open_quick_list(bool dtmf)
{
    ui_push(&contact_list_screen, (void *)(dtmf ? &dtmf_quick_list : &dmr_quick_list));
}

// ---- The actions of the selected contact (menuContactListSubMenu): the DTMF contacts have only the first ----

enum {
    ACTION_SELECT_TX,
    ACTION_EDIT,
    ACTION_DELETE,
    ACTIONS
};

static const text_id_t action_labels[ACTIONS] = {TEXT_SELECT_TX, TEXT_EDIT_CONTACT, TEXT_DELETE_CONTACT};

static int action_count(void)
{
    return list.kind->dtmf ? 1 : ACTIONS;
}

static void delete_answered(bool yes)
{
    if (yes) {
        codeplug_delete_contact(contact.index);
        result = (message_box_t){.title = list_title(), .line1 = text(TEXT_CONTACT_DELETED), .close_ms = RESULT_MS};
        ui_push(&message_box_screen, &result);
    }
}

static void do_action(int action)
{
    switch (action) {
        case ACTION_SELECT_TX:
            select_for_tx();
            break;
        case ACTION_EDIT:
            ui_replace(&contact_details_screen, (void *)(uintptr_t)contact.index);
            break;
        case ACTION_DELETE:
            name_text(contact.name, box_title, sizeof(box_title));
            if ((contact.call_type == CODEPLUG_CALL_GROUP) && codeplug_contact_in_rx_group(contact.index)) {
                result = (message_box_t){.title = box_title,
                                         .line1 = text(TEXT_CONTACT_USED),
                                         .line2 = text(TEXT_IN_TG_LIST),
                                         .close_ms = RESULT_MS};
                ui_replace(&message_box_screen, &result); // not deleted
            } else {
                question = (message_box_t){.title = box_title,
                                           .line1 = text(TEXT_DELETE_CONTACT_QM),
                                           .buttons = MESSAGE_BOX_YES_NO,
                                           .answered = delete_answered};
                ui_replace(&message_box_screen, &question);
            }
            break;
    }
}

static bool actions_handle(const ui_event_t *ev)
{
    int32_t *selected = ui_state();
    int count = action_count();

    if ((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT)) {
        if ((ev->key == UI_KEY_DOWN) || (ev->key == UI_KEY_UP)) {
            *selected = (((*selected + ((ev->key == UI_KEY_DOWN) ? 1 : -1)) % count) + count) % count;
            return true;
        }
        if ((ev->key == UI_KEY_GREEN) && (ev->type == UI_EVENT_KEY_PRESS)) {
            do_action(*selected);
            return false;
        }
    }
    list_handle_back(ev);
    return false;
}

static void actions_draw(void)
{
    char name[NAME_SIZE];
    int32_t selected = *ui_state();

    selected_name(name, sizeof(name));
    list_view_draw_title(name, NULL);
    for (int i = 0; i < action_count(); i++) {
        list_view_row_t row = {.icon = ICONS_COUNT, .name = text(action_labels[i]), .selected = (i == selected)};

        list_view_draw_row(i, &row);
    }
    bottom_bar_draw_soft_keys(text(TEXT_UI_SELECT), text(TEXT_UI_BACK));
}

static const ui_screen_t actions_screen = {
    .handle = actions_handle,
    .draw = actions_draw,
};

// ---- The contact details: new contact, edit contact ----

enum {
    ROW_NAME,
    ROW_NUMBER,
    ROW_TYPE,
    ROW_TIMESLOT,
    ROWS
};

typedef enum {
    DETAILS_EDIT,
    DETAILS_FULL, // no free place for a new contact
    DETAILS_FAILED,
} details_state_t;

static struct {
    codeplug_contact_t contact; // .index: where it is saved
    bool is_new;
    details_state_t state;
    char name[CODEPLUG_NAME_LENGTH + 1]; // the codeplug bytes (GB2312), NUL terminated
    char digits[CODEPLUG_CONTACT_ID_DIGITS + 1];
    text_entry_t name_entry;
    text_entry_t number_entry;
} details;

static void details_enter(void *arg)
{
    int index = (int)(uintptr_t)arg;

    if (!arg) {
        return; // nothing goes over it
    }

    memset(&details, 0, sizeof(details));
    details.is_new = (arg == DETAILS_NEW);
    if (details.is_new) {
        details.contact.call_type = CODEPLUG_CALL_GROUP;
        details.contact.ts_override = 0xFF; // no override
        details.contact.index = codeplug_contact_free_index();
        details.state = (details.contact.index > 0) ? DETAILS_EDIT : DETAILS_FULL;
    } else {
        details.state = codeplug_get_contact(index, &details.contact) ? DETAILS_EDIT : DETAILS_FAILED;
        for (int i = 0;
             (i < CODEPLUG_NAME_LENGTH) && ((uint8_t)details.contact.name[i] != 0xFF) && (details.contact.name[i] != 0);
             i++) {
            details.name[i] = details.contact.name[i];
        }
        if (details.contact.id > 0) {
            snprintf(details.digits, sizeof(details.digits), "%lu",
                     (unsigned long)(details.contact.id % 100000000UL)); // 8 digits at most (24 bits)
        }
    }
    text_entry_start(&details.name_entry, details.name, CODEPLUG_NAME_LENGTH, false);
    text_entry_start(&details.number_entry, details.digits, CODEPLUG_CONTACT_ID_DIGITS, true);
}

static void set_timeslot(int timeslot)
{
    details.contact.ts_override =
        (uint8_t)((details.contact.ts_override & ~(CODEPLUG_CONTACT_NO_TS_OVERRIDE | CODEPLUG_CONTACT_TS2)) |
                  ((timeslot == 0) ? CODEPLUG_CONTACT_NO_TS_OVERRIDE : ((timeslot == 2) ? CODEPLUG_CONTACT_TS2 : 0)));
}

// LEFT / RIGHT, the rotary: the type and the timeslot (no wrapping, as the original); true: changed
static bool details_change(int row, int direction)
{
    if (row == ROW_TYPE) {
        int type = details.contact.call_type + direction;

        if ((type < 0) || (type >= CODEPLUG_CALL_TYPES)) {
            return false;
        }
        details.contact.call_type = (uint8_t)type;
        return true;
    }
    if (row == ROW_TIMESLOT) {
        int timeslot = codeplug_contact_timeslot(&details.contact) + direction;

        if ((timeslot < 0) || (timeslot > 2)) {
            return false;
        }
        set_timeslot(timeslot);
        return true;
    }
    return false;
}

static void details_done(bool yes)
{
    (void)yes;
    ui_pop();
}

static void details_save(void)
{
    codeplug_contact_t *c = &details.contact;
    uint32_t id = (c->call_type == CODEPLUG_CALL_ALL) ? CODEPLUG_CONTACT_ID_MAX : strtoul(details.digits, NULL, 10);
    char name[CODEPLUG_NAME_LENGTH + 1];

    if ((id < CODEPLUG_CONTACT_ID_MIN) || (id > CODEPLUG_CONTACT_ID_MAX)) {
        return; // no number yet: nothing happens, as the original
    }
    c->id = id;

    // No name: "TG 91", "PC 1234567" (ASCII: the codeplug holds GB2312, not the UTF-8 of the texts)
    if (details.name[0] == 0) {
        snprintf(name, sizeof(name), "%s %lu", ((c->call_type == CODEPLUG_CALL_PRIVATE) ? "PC" : "TG"),
                 (unsigned long)id);
    } else {
        memcpy(name, details.name, sizeof(name));
    }
    memset(c->name, 0xFF, sizeof(c->name));
    memcpy(c->name, name, strlen(name));

    // The result, the contact's name as the title; back to the list after it (a duplicate: back here, to change it)
    bool duplicate = (codeplug_contact_find_same(c, c->index) != 0);
    bool saved = !duplicate && codeplug_save_contact(c->index, c);

    codeplug_text_name(c->name, CODEPLUG_NAME_LENGTH, box_title, sizeof(box_title));
    result = (message_box_t){
        .title = box_title,
        .line1 = text(duplicate ? TEXT_DUPLICATE : (saved ? TEXT_CONTACT_SAVED : TEXT_ERROR)),
        .line2 = (duplicate ? text(TEXT_CONTACT) : NULL),
        .close_ms = RESULT_MS,
        .answered = (duplicate ? NULL : details_done),
    };
    ui_push(&message_box_screen, &result);
}

static bool details_handle_edit(const ui_event_t *ev)
{
    int32_t *row = ui_state();

    if ((*row == ROW_NAME) && text_entry_handle(&details.name_entry, ev)) {
        return true;
    }
    if ((*row == ROW_NUMBER) && (details.contact.call_type != CODEPLUG_CALL_ALL) &&
        text_entry_handle(&details.number_entry, ev)) {
        return true;
    }

    if (ev->type == UI_EVENT_ROTARY) {
        bool changed = false;

        for (int n = 0; n < ((ev->steps < 0) ? -ev->steps : ev->steps); n++) {
            changed |= details_change(*row, ((ev->steps > 0) ? 1 : -1));
        }
        return changed;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT)) {
        switch (ev->key) {
            case UI_KEY_UP:
            case UI_KEY_DOWN:
                text_entry_commit(&details.name_entry);
                *row = (((*row + ((ev->key == UI_KEY_DOWN) ? 1 : -1)) % ROWS) + ROWS) % ROWS;
                return true;
            case UI_KEY_LEFT:
            case UI_KEY_RIGHT:
                return details_change(*row, ((ev->key == UI_KEY_RIGHT) ? 1 : -1));
            case UI_KEY_GREEN:
                if (ev->type == UI_EVENT_KEY_PRESS) {
                    text_entry_commit(&details.name_entry);
                    details_save();
                    return true;
                }
                break;
            default:
                break;
        }
    }
    list_handle_back(ev);
    return false;
}

static bool details_handle(const ui_event_t *ev)
{
    if (details.state == DETAILS_EDIT) {
        return details_handle_edit(ev);
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN)) {
        ui_pop();
    } else {
        list_handle_back(ev);
    }
    return false;
}

static void details_draw_rows(void)
{
    static const icons_id_t icons[ROWS] = {ICONS_CONTACT, ICONS_KEYPAD, ICONS_TALKGROUP, ICONS_CHANNEL};
    static const text_id_t types[CODEPLUG_CALL_TYPES] = {TEXT_GROUP, TEXT_PRIVATE, TEXT_ALL};
    static const text_id_t prefixes[CODEPLUG_CALL_TYPES] = {TEXT_TG, TEXT_PC, TEXT_ALL};
    const codeplug_contact_t *c = &details.contact;
    int32_t selected = *ui_state();
    char name[NAME_SIZE];
    char number[VALUE_SIZE];
    char timeslot[8];

    codeplug_text_name(details.name, CODEPLUG_NAME_LENGTH, name, sizeof(name));

    bool editing_number = (selected == ROW_NUMBER) && (c->call_type != CODEPLUG_CALL_ALL);

    if (c->call_type == CODEPLUG_CALL_ALL) {
        snprintf(number, sizeof(number), "%s %lu", text(TEXT_ALL), (unsigned long)CODEPLUG_CONTACT_ID_MAX);
    } else {
        snprintf(number, sizeof(number), "%s %s", text(prefixes[c->call_type]),
                 ((details.digits[0] || editing_number) ? details.digits : text(TEXT_NONE)));
    }
    if (codeplug_contact_timeslot(c) == 0) {
        snprintf(timeslot, sizeof(timeslot), "%s", text(TEXT_NONE));
    } else {
        snprintf(timeslot, sizeof(timeslot), "%d", codeplug_contact_timeslot(c));
    }

    const list_view_row_t rows[ROWS] = {
        {.name = text(TEXT_NAME),
         .value = name,
         .cursor = ((selected == ROW_NAME) ? text_entry_cursor(&details.name_entry, 0) : 0)},
        {.name = text(TEXT_UI_NUMBER),
         .value = number,
         .cursor =
             ((editing_number && (strlen(details.digits) < CODEPLUG_CONTACT_ID_DIGITS)) ? (int16_t)(strlen(number) + 1)
                                                                                        : 0)},
        {.name = text(TEXT_TYPE), .value = text(types[(c->call_type < CODEPLUG_CALL_TYPES) ? c->call_type : 0])},
        {.name = text(TEXT_TIMESLOT), .value = timeslot},
    };

    for (int row = 0; row < ROWS; row++) {
        list_view_row_t data = rows[row];

        data.icon = icons[row];
        data.selected = (row == selected);
        list_view_draw_row(row, &data);
    }
}

static void details_draw(void)
{
    if (details.state == DETAILS_EDIT) {
        list_view_draw_title(text(details.is_new ? TEXT_NEW_CONTACT : TEXT_EDIT_CONTACT), NULL);
        details_draw_rows();
        bottom_bar_draw_soft_keys(text(TEXT_UI_SAVE), text(TEXT_UI_BACK));
        return;
    }

    // No place for a new contact, or it could not be read
    list_view_draw_title(text(details.is_new ? TEXT_NEW_CONTACT : TEXT_EDIT_CONTACT), NULL);
    list_view_draw_text(text((details.state == DETAILS_FULL) ? TEXT_LIST_FULL : TEXT_ERROR), NULL, THEME_COLOUR_TEXT);
    bottom_bar_draw_soft_keys(text(TEXT_UI_OK), NULL);
}

const ui_screen_t contact_details_screen = {
    .enter = details_enter,
    .handle = details_handle,
    .draw = details_draw,
};

// ---- The contacts menu (menuSystem.c contactMenuItems) ----

static const ui_item_t contacts_items[] = {
    {TEXT_DMR_CONTACTS, ICONS_CONTACT, UI_ITEM_SCREEN, &contact_list_screen, (void *)&dmr_list},
    {TEXT_DTMF_CONTACT_LIST, ICONS_KEYPAD, UI_ITEM_SCREEN, &contact_list_screen, (void *)&dtmf_list},
    {TEXT_NEW_CONTACT, ICONS_CONTACT_ADD, UI_ITEM_SCREEN, &contact_details_screen, DETAILS_NEW},
};

const ui_list_t contacts_menu = {
    .title = TEXT_CONTACTS,
    .items = contacts_items,
    .count = sizeof(contacts_items) / sizeof(contacts_items[0]),
};
