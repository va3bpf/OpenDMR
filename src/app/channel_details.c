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
 * OpenDMR, screen: the channel details (menuChannelDetails.c, docs/ui-design.md 5.6). The 26 items of the
 * channel (or VFO) of the home screen, in a list as the settings: UP / DOWN move, LEFT / RIGHT (and the rotary) change
 * the value, the left key writes the channel to the codeplug and goes home, the right key goes back without saving.
 * Rx, Tx and DMR ID: the keypad enters a number (8 digits, then the left key or the 8th digit sets it, LEFT deletes,
 * the right key cancels); LEFT / RIGHT on Rx / Tx set Tx to Rx plus a repeater shift. The name is edited in place
 * with text_entry.c (keypad letters, LEFT / RIGHT move, SK2 + LEFT deletes, SK2 + RIGHT inserts).
 *
 * The main firmware writes the channel to the codeplug only with SK2 held (else the change lasts until the channel is
 * left); here the left key always writes it, as there is no channel copy in RAM.
 * Not editable yet, shown as numbers: the TG list, the contact and the APRS configuration (their lists aren't read
 * from the codeplug yet).
 */

#include <stdio.h>
#include <string.h>
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "text.h"
#include "list_screen.h"
#include "settings.h"
#include "codeplug.h"
#include "codeplug_text.h"
#include "vfo_screen.h"
#include "channel_screen.h"
#include "tones.h"
#include "text_entry.h"
#include "channel_details.h"

enum {
    ITEM_NAME = 0,
    ITEM_RX,
    ITEM_TX,
    ITEM_MODE,
    ITEM_DMR_ID,
    ITEM_COLOUR_CODE,
    ITEM_TIMESLOT,
    ITEM_TG_LIST,
    ITEM_CONTACT,
    ITEM_RX_CSS,
    ITEM_TX_CSS,
    ITEM_BANDWIDTH,
    ITEM_STEP,
    ITEM_TOT,
    ITEM_RX_ONLY,
    ITEM_ZONE_SKIP,
    ITEM_ALL_SKIP,
    ITEM_VOX,
    ITEM_POWER,
    ITEM_SQUELCH,
    ITEM_BEEP,
    ITEM_ECO,
    ITEM_TA_TX_TS1,
    ITEM_TA_TX_TS2,
    ITEM_APRS,
    ITEM_FORCE_DMO,
    ITEM_COUNT
};

// Bits of the channel (codeplug.h of the main firmware)
#define FLAG1_OPTIONAL_DMR_ID 0x80 // libre_dmr_flag1
#define FLAG1_NO_BEEP         0x40
#define FLAG1_NO_ECO          0x20
#define FLAG1_FORCE_DMO       0x04
#define FLAG2_TIMESLOT_2      0x40
#define FLAG4_VOX             0x40
#define FLAG4_ZONE_SKIP       0x20
#define FLAG4_ALL_SKIP        0x10
#define FLAG4_RX_ONLY         0x04
#define FLAG4_BANDWIDTH_25K   0x02
#define TA_TX_TS1_SHIFT       0 // flag1: talker alias Tx, 2 bits a timeslot: off, APRS, text, both
#define TA_TX_TS2_SHIFT       2
#define TA_TX_MASK            0x03
#define TA_TX_BOTH            3
#define DMR_ID_DEFAULT        0x00001600 // rx_signaling, arts_interval, encrypt when there is no channel DMR ID
#define DMR_ID_MAX            16777215
#define COLOUR_CODE_MAX       15
#define SQUELCH_MAX           21 // 0: from the settings, 1 - 21: 0 - 100 % in 5 % steps
#define POWER_MAX             10 // 0: from the settings, 1 - 10: the levels (10: user power)
#define TOT_MAX               255
#define TOT_UNIT_S            15
#define VFO_STEP_SHIFT        4
#define CSS_FAST_STEP         5 // held LEFT / RIGHT

#define NAME_LENGTH    CODEPLUG_NAME_LENGTH
#define DIGITS         8
#define FREQ_DOT_AFTER 3
#define VALUE_SIZE     32

// Layout of the number entry (the main firmware's widgetDigitEntryDraw() at WIDGET_MAIN_TOP + 10)
#define ENTRY_LABEL_Y  (WIDGET_MAIN_TOP + 14)
#define ENTRY_DIGITS_Y (ENTRY_LABEL_Y + 26)
#define ENTRY_HINT_Y   (ENTRY_DIGITS_Y + 56)

// Repeater shifts of LEFT / RIGHT on Rx / Tx, in 100 kHz
static const uint8_t shifts[] = {0, 6, 10, 15, 16, 20, 46, 50, 70, 76, 90, 94};
#define SHIFT_MAX ((int)(sizeof(shifts) / sizeof(shifts[0])) - 1)

static codeplug_channel_t edit; // the copy being edited
static int ref;                 // channel_details_current_ref() when it was opened
static char name[NAME_LENGTH + 1];
static bool name_editable; // not for a VFO
static text_entry_t name_entry;
static int shift_index;
static digit_entry_t entry; // Rx, Tx, DMR ID
static int entry_item;

int channel_details_current_ref(void)
{
    if (vfo_screen_is_home()) {
        return -1 - vfo_screen_current();
    }
    return channel_screen_channel;
}

static bool is_vfo(void)
{
    return ref < 0;
}

static bool is_dmr(void)
{
    return edit.mode == CODEPLUG_MODE_DMR;
}

static int *selected(void)
{
    return (int *)ui_state();
}

static void format_css(const char *direction, uint16_t tone, char *label, size_t label_size, char *out, size_t size)
{
    bool dcs = ((tone != CODEPLUG_CSS_NONE) && (tone & TONES_DCS));

    snprintf(label, label_size, "%s %s", direction, (dcs ? "DCS" : "CTCSS"));
    if ((edit.mode != CODEPLUG_MODE_FM) || (edit.aprs_config_index != 0)) {
        snprintf(out, size, "%s", text(TEXT_N_A));
    } else if (tone == CODEPLUG_CSS_NONE) {
        snprintf(out, size, "%s", text(TEXT_NONE));
    } else {
        tones_format(tone, out, size);
    }
}

/* The DMR ID of the channel (codeplugChannelGetOptionalDMRID()) */

static uint32_t dmr_id(void)
{
    if (!(edit.libre_dmr_flag1 & FLAG1_OPTIONAL_DMR_ID)) {
        return 0;
    }
    return ((uint32_t)edit.rx_signaling << 16) | ((uint32_t)edit.arts_interval << 8) | edit.encrypt;
}

static void set_dmr_id(uint32_t id)
{
    bool valid = ((id >= 1) && (id <= DMR_ID_MAX));
    uint32_t stored = (valid ? id : DMR_ID_DEFAULT);

    edit.libre_dmr_flag1 =
        (uint8_t)((edit.libre_dmr_flag1 & ~FLAG1_OPTIONAL_DMR_ID) | (valid ? FLAG1_OPTIONAL_DMR_ID : 0));
    edit.rx_signaling = (uint8_t)(stored >> 16);
    edit.arts_interval = (uint8_t)(stored >> 8);
    edit.encrypt = (uint8_t)stored;
}

/* Values */

// Tx = Rx + the next repeater shift (applyShiftOffset())
static void apply_shift(int direction)
{
    int next = shift_index + direction;

    if ((next < -SHIFT_MAX) || (next > SHIFT_MAX)) {
        return;
    }

    int32_t shift = (int32_t)shifts[(next < 0) ? -next : next] * ((next < 0) ? -10000 : 10000); // 100 kHz in 10 Hz
    uint32_t tx = edit.rx_freq + (uint32_t)shift;

    if (codeplug_frequency_is_valid(tx)) {
        shift_index = next;
        edit.tx_freq = tx;
    }
}

static void set_bit(uint8_t *byte, uint8_t mask, bool on)
{
    *byte = (uint8_t)(on ? (*byte | mask) : (*byte & ~mask));
}

static void step_value(uint8_t *value, int direction, int min, int max)
{
    int next = *value + direction;

    if ((next >= min) && (next <= max)) {
        *value = (uint8_t)next;
    }
}

// The TG list, contact and APRS configuration items step through the ones in the codeplug, "none" (0) first

static void step_rx_group(int direction)
{
    codeplug_rx_group_t group;
    int number = edit.rx_group_list;

    do {
        number += direction;
    } while ((number > 0) && (number <= CODEPLUG_RX_GROUPS_MAX) && !codeplug_get_rx_group(number, &group));
    if ((number >= 0) && (number <= CODEPLUG_RX_GROUPS_MAX)) {
        edit.rx_group_list = (uint8_t)number;
    }
}

static void step_contact(int direction)
{
    int count = codeplug_contact_count(-1);
    int position = -1; // "none"

    for (int i = 0; i < count; i++) {
        if (codeplug_contact_at(i, -1) == edit.contact) {
            position = i;
            break;
        }
    }
    position += direction;
    if ((position >= -1) && (position < count)) {
        edit.contact = (uint16_t)((position < 0) ? 0 : codeplug_contact_at(position, -1));
    }
}

static void step_aprs(int direction)
{
    codeplug_aprs_config_t config;
    int index = edit.aprs_config_index;

    do {
        index += direction;
    } while ((index > 0) && (index <= CODEPLUG_APRS_CONFIGS_MAX) && !codeplug_get_aprs_config(index, &config));
    if ((index >= 0) && (index <= CODEPLUG_APRS_CONFIGS_MAX)) {
        edit.aprs_config_index = (uint8_t)index;
    }
}

// LEFT (-1) / RIGHT (+1) on an item, as menuChannelDetails.c handleEvent()
static void change(int item, int direction, bool fast, uint8_t modifiers)
{
    bool on = (direction > 0);
    bool fm = !is_dmr();
    bool aprs = (edit.aprs_config_index != 0);

    switch (item) {
        case ITEM_RX:
        case ITEM_TX:
            apply_shift(direction);
            break;
        case ITEM_MODE: // RIGHT: FM, LEFT: DMR (DMR is 12.5 kHz)
            edit.mode = (on ? CODEPLUG_MODE_FM : CODEPLUG_MODE_DMR);
            if (!on) {
                set_bit(&edit.flag4, FLAG4_BANDWIDTH_25K, false);
            }
            break;
        case ITEM_COLOUR_CODE:
            if (!fm) {
                step_value(&edit.tx_colour, direction, 0, COLOUR_CODE_MAX);
            }
            break;
        case ITEM_TIMESLOT:
            if (!fm) {
                set_bit(&edit.flag2, FLAG2_TIMESLOT_2, on);
            }
            break;
        case ITEM_RX_CSS:
        case ITEM_TX_CSS:
            if (fm && !aprs) {
                {
                    uint16_t *tone = ((item == ITEM_RX_CSS) ? &edit.rx_tone : &edit.tx_tone);

                    *tone = tones_step(*tone, direction * (fast ? CSS_FAST_STEP : 1));
                }
            }
            break;
        case ITEM_BANDWIDTH:
            if (fm) {
                set_bit(&edit.flag4, FLAG4_BANDWIDTH_25K, on);
            }
            break;
        case ITEM_STEP: {
            int step = (edit.vfo_flag5 >> VFO_STEP_SHIFT) + direction;

            step = (step < 0) ? 0 : ((step >= VFO_SCREEN_STEPS) ? (VFO_SCREEN_STEPS - 1) : step);
            edit.vfo_flag5 = (uint8_t)((edit.vfo_flag5 & 0x0F) | (step << VFO_STEP_SHIFT));
            break;
        }
        case ITEM_TOT:
            step_value(&edit.tot, direction, 0, TOT_MAX);
            break;
        case ITEM_RX_ONLY:
            set_bit(&edit.flag4, FLAG4_RX_ONLY, on);
            break;
        case ITEM_ZONE_SKIP:
            set_bit(&edit.flag4, FLAG4_ZONE_SKIP, on);
            break;
        case ITEM_ALL_SKIP:
            set_bit(&edit.flag4, FLAG4_ALL_SKIP, on);
            break;
        case ITEM_VOX:
            set_bit(&edit.flag4, FLAG4_VOX, on);
            break;
        case ITEM_POWER:
            if (!is_vfo()) {
                step_value(&edit.libre_dmr_power, direction, 0, POWER_MAX);
            }
            break;
        case ITEM_SQUELCH:
            if (fm) {
                step_value(&edit.sql, direction, 0, SQUELCH_MAX);
            }
            break;
        case ITEM_BEEP: // the bits are "no beep", "no eco"
            set_bit(&edit.libre_dmr_flag1, FLAG1_NO_BEEP, !on);
            break;
        case ITEM_ECO:
            set_bit(&edit.libre_dmr_flag1, FLAG1_NO_ECO, !on);
            break;
        case ITEM_TA_TX_TS1:
        case ITEM_TA_TX_TS2:
            if (!fm) {
                int shift = ((item == ITEM_TA_TX_TS1) ? TA_TX_TS1_SHIFT : TA_TX_TS2_SHIFT);
                uint8_t value = (uint8_t)((edit.flag1 >> shift) & TA_TX_MASK);

                step_value(&value, direction, 0, TA_TX_BOTH);
                edit.flag1 = (uint8_t)((edit.flag1 & ~(TA_TX_MASK << shift)) | (value << shift));
            }
            break;
        case ITEM_FORCE_DMO:
            if (!fm) {
                set_bit(&edit.libre_dmr_flag1, FLAG1_FORCE_DMO, on);
            }
            break;
        case ITEM_TG_LIST:
            if (!fm) {
                step_rx_group(direction);
            }
            break;
        case ITEM_CONTACT:
            if (!fm) {
                step_contact(direction);
            }
            break;
        case ITEM_APRS:
            if (fm) {
                step_aprs(direction);
            }
            break;
        default:
            break;
    }
}

static const char *yes_no(bool yes)
{
    return text(yes ? TEXT_YES : TEXT_NO);
}

static void format_frequency(uint32_t frequency, char *out, size_t size)
{
    digit_entry_format_frequency(frequency, out, size);
    snprintf(out + strlen(out), size - strlen(out), "MHz");
}

// The label and the value of an item (menuChannelDetails.c updateScreen())
static void format_item(int item, char *label, size_t label_size, char *out, size_t size)
{
    static const text_id_t ta_values[] = {TEXT_OFF, TEXT_APRS, TEXT_TA_TEXT, TEXT_BOTH};
    static const text_id_t labels[ITEM_COUNT] = {
        [ITEM_NAME] = TEXT_NAME,
        [ITEM_MODE] = TEXT_MODE,
        [ITEM_DMR_ID] = TEXT_DMR_ID,
        [ITEM_COLOUR_CODE] = TEXT_COLOUR_CODE,
        [ITEM_TIMESLOT] = TEXT_TIMESLOT,
        [ITEM_TG_LIST] = TEXT_TG_LIST,
        [ITEM_CONTACT] = TEXT_CONTACT,
        [ITEM_BANDWIDTH] = TEXT_BANDWIDTH,
        [ITEM_STEP] = TEXT_STEPFREQ,
        [ITEM_TOT] = TEXT_TOT,
        [ITEM_RX_ONLY] = TEXT_RX_ONLY,
        [ITEM_ZONE_SKIP] = TEXT_ZONE_SKIP,
        [ITEM_ALL_SKIP] = TEXT_ALL_SKIP,
        [ITEM_POWER] = TEXT_CHANNEL_POWER,
        [ITEM_SQUELCH] = TEXT_SQUELCH,
        [ITEM_BEEP] = TEXT_BEEP,
        [ITEM_ECO] = TEXT_ECO,
        [ITEM_TA_TX_TS1] = TEXT_TRANSMITTALKERALIASTS1,
        [ITEM_TA_TX_TS2] = TEXT_TRANSMITTALKERALIASTS2,
        [ITEM_APRS] = TEXT_APRS,
        [ITEM_FORCE_DMO] = TEXT_DMR_FORCE_DMO,
    };
    bool fm = !is_dmr();
    const char *na = text(TEXT_N_A);

    snprintf(label, label_size, "%s", text(labels[item]));
    out[0] = '\0';

    switch (item) {
        case ITEM_NAME:
            codeplug_text_name(name, NAME_LENGTH, out, size);
            if (is_vfo() && (out[0] == '\0')) {
                snprintf(out, size, "VFO %c", ((ref == -1) ? 'A' : 'B'));
            }
            break;
        case ITEM_RX:
            snprintf(label, label_size, "%s", text(TEXT_RX));
            format_frequency(edit.rx_freq, out, size);
            break;
        case ITEM_TX:
            snprintf(label, label_size, "%s", text(TEXT_TX));
            format_frequency(edit.tx_freq, out, size);
            break;
        case ITEM_MODE:
            snprintf(out, size, "%s", (fm ? "FM" : "DMR"));
            break;
        case ITEM_DMR_ID:
            if (fm) {
                snprintf(out, size, "%s", na);
            } else if (dmr_id() == 0) {
                snprintf(out, size, "%s", text(TEXT_NONE));
            } else {
                snprintf(out, size, "%lu", (unsigned long)dmr_id());
            }
            break;
        case ITEM_COLOUR_CODE:
            if (fm) {
                snprintf(out, size, "%s", na);
            } else {
                snprintf(out, size, "%u", edit.tx_colour);
            }
            break;
        case ITEM_TIMESLOT:
            if (fm) {
                snprintf(out, size, "%s", na);
            } else {
                snprintf(out, size, "%u", ((edit.flag2 & FLAG2_TIMESLOT_2) ? 2 : 1));
            }
            break;
        case ITEM_TG_LIST:
        case ITEM_CONTACT: {
            unsigned number = ((item == ITEM_TG_LIST) ? edit.rx_group_list : edit.contact);

            if (fm) {
                snprintf(out, size, "%s", na);
            } else if (number == 0) {
                snprintf(out, size, "%s", text(TEXT_NONE));
            } else if (item == ITEM_TG_LIST) {
                codeplug_rx_group_t group;

                if (codeplug_get_rx_group((int)number, &group)) {
                    codeplug_text_name(group.name, CODEPLUG_NAME_LENGTH, out, size);
                } else {
                    snprintf(out, size, "%u", number);
                }
            } else {
                codeplug_contact_t contact;

                if (codeplug_get_contact((int)number, &contact)) {
                    codeplug_text_name(contact.name, CODEPLUG_NAME_LENGTH, out, size);
                } else {
                    snprintf(out, size, "%u", number);
                }
            }
            break;
        }
        case ITEM_RX_CSS:
            format_css(text(TEXT_RX), edit.rx_tone, label, label_size, out, size);
            break;
        case ITEM_TX_CSS:
            format_css(text(TEXT_TX), edit.tx_tone, label, label_size, out, size);
            break;
        case ITEM_BANDWIDTH:
            snprintf(out, size, "%s", (!fm ? na : ((edit.flag4 & FLAG4_BANDWIDTH_25K) ? "25kHz" : "12.5kHz")));
            break;
        case ITEM_STEP: {
            unsigned step = vfo_screen_steps[(edit.vfo_flag5 >> VFO_STEP_SHIFT) % VFO_SCREEN_STEPS];

            snprintf(out, size, "%u.%02ukHz", step / 100, step % 100);
            break;
        }
        case ITEM_TOT:
            if (edit.tot == 0) {
                snprintf(out, size, "%s", text(TEXT_OFF));
            } else {
                snprintf(out, size, "%us", (unsigned)(edit.tot * TOT_UNIT_S));
            }
            break;
        case ITEM_RX_ONLY:
            snprintf(out, size, "%s", yes_no(edit.flag4 & FLAG4_RX_ONLY));
            break;
        case ITEM_ZONE_SKIP:
            snprintf(out, size, "%s", yes_no(edit.flag4 & FLAG4_ZONE_SKIP));
            break;
        case ITEM_ALL_SKIP:
            snprintf(out, size, "%s", yes_no(edit.flag4 & FLAG4_ALL_SKIP));
            break;
        case ITEM_VOX:
            snprintf(label, label_size, "%s", text(TEXT_VOX));
            snprintf(
                out, size, "%s",
                ((fm && (edit.aprs_config_index != 0)) ? na : text((edit.flag4 & FLAG4_VOX) ? TEXT_ON : TEXT_OFF)));
            break;
        case ITEM_POWER:
            if (is_vfo()) {
                snprintf(out, size, "%s", na);
            } else if (edit.libre_dmr_power == 0) {
                snprintf(out, size, "%s", text(TEXT_FROM_MASTER));
            } else {
                snprintf(out, size, "%s", bottom_bar_power_label((edit.libre_dmr_power - 1) % POWER_MAX));
            }
            break;
        case ITEM_SQUELCH:
            if (!fm) {
                snprintf(out, size, "%s", na);
            } else if (edit.sql == 0) {
                snprintf(out, size, "%s", text(TEXT_FROM_MASTER));
            } else {
                snprintf(out, size, "%u%%", 5U * (edit.sql - 1U));
            }
            break;
        case ITEM_BEEP:
            snprintf(out, size, "%s", yes_no(!(edit.libre_dmr_flag1 & FLAG1_NO_BEEP)));
            break;
        case ITEM_ECO:
            snprintf(out, size, "%s", yes_no(!(edit.libre_dmr_flag1 & FLAG1_NO_ECO)));
            break;
        case ITEM_TA_TX_TS1:
        case ITEM_TA_TX_TS2: {
            int shift = ((item == ITEM_TA_TX_TS1) ? TA_TX_TS1_SHIFT : TA_TX_TS2_SHIFT);

            snprintf(out, size, "%s", (fm ? na : text(ta_values[(edit.flag1 >> shift) & TA_TX_MASK])));
            break;
        }
        case ITEM_APRS:
            if (!fm) {
                snprintf(out, size, "%s", na);
            } else if (edit.aprs_config_index == 0) {
                snprintf(out, size, "%s", text(TEXT_NONE));
            } else {
                codeplug_aprs_config_t config;

                if (codeplug_get_aprs_config(edit.aprs_config_index, &config)) {
                    codeplug_text_name(config.name, sizeof(config.name), out, size);
                } else {
                    snprintf(out, size, "%u", edit.aprs_config_index);
                }
            }
            break;
        case ITEM_FORCE_DMO:
            snprintf(out, size, "%s", (fm ? na : yes_no(edit.libre_dmr_flag1 & FLAG1_FORCE_DMO)));
            break;
        default:
            break;
    }
}

/* Number entry: Rx, Tx (xxx.xxxxx MHz), DMR ID */

static bool is_number_item(int item)
{
    return (item == ITEM_RX) || (item == ITEM_TX) || ((item == ITEM_DMR_ID) && is_dmr());
}

// The number entered: true when it was set
static bool entry_set(uint32_t value)
{
    if (entry_item == ITEM_DMR_ID) {
        if (value > DMR_ID_MAX) {
            return false;
        }
        set_dmr_id(value);
    } else if (!codeplug_frequency_is_valid(value)) {
        return false;
    } else if (entry_item == ITEM_RX) {
        edit.rx_freq = value;
    } else {
        edit.tx_freq = value;
    }
    return true;
}

// Keys while a number is entered (all of them); true: handled
static bool handle_entry(const ui_event_t *ev)
{
    if (!digit_entry_active(&entry)) {
        return false;
    }
    digit_entry_handle(&entry, ev, entry_set);
    return true;
}

/* The screen */

static void enter(void *arg)
{
    (void)arg;
    ref = channel_details_current_ref();
    *selected() = ITEM_NAME;
    digit_entry_stop(&entry);
    shift_index = 0;
    memset(&edit, 0, sizeof(edit));

    if (ref < 0) {
        memcpy(&edit, vfo_screen_vfo(-1 - ref), sizeof(edit));
    } else if ((ref == 0) || !codeplug_get_channel(ref, &edit)) {
        ref = 0;
        return;
    }

    codeplug_name_to_string(edit.name, name, NAME_LENGTH);
    name_editable = !is_vfo();
    text_entry_start(&name_entry, name, NAME_LENGTH, false);
}

static void leave(void)
{
    ui_timer_stop();
}

static void save(void)
{
    if (is_vfo()) {
        memcpy(vfo_screen_vfo(-1 - ref), &edit, sizeof(edit));
        vfo_screen_save(-1 - ref);
        return;
    }
    if (name_editable) {
        memset(edit.name, 0xFF, NAME_LENGTH);
        memcpy(edit.name, name, strlen(name));
    }
    codeplug_write_channel(ref, &edit);
}

static bool handle(const ui_event_t *ev)
{
    int item = *selected();
    int digit = ui_key_digit(ev->key);
    bool press = ((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT));

    if (ref == 0) {
        list_handle_back(ev);
        return false;
    }
    if (handle_entry(ev)) {
        return true;
    }
    if ((item == ITEM_NAME) && name_editable && !(ev->modifiers & UI_MODIFIER_SK2 && (ui_key_digit(ev->key) >= 0)) &&
        text_entry_handle(&name_entry, ev)) {
        return true;
    }

    if (ev->type == UI_EVENT_ROTARY) {
        if (item == ITEM_NAME) {
            return false; // the rotary changes values, the name isn't one
        }
        for (int n = 0; n < ((ev->steps < 0) ? -ev->steps : ev->steps); n++) {
            change(item, ((ev->steps > 0) ? 1 : -1), false, 0);
        }
        return true;
    }

    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN)) {
        text_entry_commit(&name_entry);
        if (name_editable && (name[0] == '\0')) { // no empty name, as the CPS
            *selected() = ITEM_NAME;
            return true;
        }
        save();
        ui_home();
        return false;
    }

    if ((ev->type == UI_EVENT_KEY_PRESS) && (digit >= 0) && !(ev->modifiers & UI_MODIFIER_SK2)) {
        if (is_number_item(item) && ((item == ITEM_DMR_ID) || (digit != 0))) { // no frequency under 100 MHz
            bool id = (item == ITEM_DMR_ID);

            entry = (digit_entry_t){.max = DIGITS, .dot_after = (id ? 0 : FREQ_DOT_AFTER), .zero_fill = !id};
            entry_item = item;
            return digit_entry_handle(&entry, ev, entry_set);
        }
        return false;
    }

    if (press && ((ev->key == UI_KEY_LEFT) || (ev->key == UI_KEY_RIGHT))) {
        change(item, ((ev->key == UI_KEY_RIGHT) ? 1 : -1), (ev->type == UI_EVENT_KEY_REPEAT), ev->modifiers);
        return true;
    }

    if (press && ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN))) {
        text_entry_commit(&name_entry);
        *selected() = (item + ((ev->key == UI_KEY_DOWN) ? 1 : (ITEM_COUNT - 1))) % ITEM_COUNT;
        return true;
    }

    list_handle_back(ev); // back without saving; held: home
    return false;
}

// The number being entered, as the VFO screen: entered digits, unlit zeros, the next one underlined
static void draw_entry(int item)
{
    bool id = (item == ITEM_DMR_ID);

    list_view_draw_title(text(TEXT_CHANNEL_DETAILS), NULL);
    draw_text(WIDGET_MARGIN, ENTRY_LABEL_Y,
              (id ? text(TEXT_DMR_ID) : text((item == ITEM_TX) ? TEXT_UI_TX : TEXT_UI_RX)), &font_roboto17,
              FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);

    digit_entry_draw(&entry, WIDGET_MARGIN, ENTRY_DIGITS_Y);
    if (!id) {
        draw_text(WIDGET_MARGIN, ENTRY_HINT_Y, "MHz", &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    }
    bottom_bar_draw_soft_keys(text(TEXT_UI_OK), text(TEXT_UI_DELETE));
}

static void draw(void)
{
    int current = *selected();
    char position[16];

    if (ref == 0) {
        list_view_draw_title(text(TEXT_CHANNEL_DETAILS), NULL);
        list_view_draw_text(text(TEXT_NONE), NULL, THEME_COLOUR_TEXT_LABEL);
        bottom_bar_draw_soft_keys(NULL, text(TEXT_UI_BACK));
        return;
    }
    if (digit_entry_active(&entry)) {
        draw_entry(current);
        return;
    }

    snprintf(position, sizeof(position), "%d/%d", current + 1, ITEM_COUNT);
    list_view_draw_title(text(TEXT_CHANNEL_DETAILS), position);

    for (int row = 0; row < LIST_VIEW_ROWS; row++) {
        int item = list_row_index(row, current, ITEM_COUNT);
        if (item < 0) {
            break;
        }
        char label[VALUE_SIZE];
        char value[VALUE_SIZE * 2];

        format_item(item, label, sizeof(label), value, sizeof(value));

        list_view_row_t data = {
            .icon = ICONS_COUNT,
            .name = label,
            .value = value,
            .selected = (item == current),
            .cursor =
                (((item == ITEM_NAME) && (item == current) && name_editable) ? text_entry_cursor(&name_entry, 0) : 0),
        };

        list_view_draw_row(row, &data);
    }

    bottom_bar_draw_soft_keys(text(TEXT_UI_SELECT), text(TEXT_UI_BACK));
}

const ui_screen_t channel_details_screen = {
    .enter = enter,
    .leave = leave,
    .handle = handle,
    .draw = draw,
};
