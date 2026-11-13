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
 * OpenDMR, screens: the channel and VFO quick menus (uiChannelMode.c and uiVFOMode.c uiXModeQuickMenu(),
 * docs/ui-design.md 5.7), opened by the left key on the home screen (held: the main menu); the channel details first.
 * LEFT / RIGHT (and the rotary) change the setting itself, in use at once: the radio follows after the event (the home
 * screen's channel, ui_tune()); the main firmware changed a copy, applied by the left key. The left key does the
 * action of its line and goes back, the right key goes back (held: home); the settings are saved when the menu is
 * left. "VFO --> Channel" and "VFO --> New Chan" ask first.
 *
 * The filters, CC scan and TS filter are the settings the radio and the DMR engine use; talkaround is the channel
 * screen's (it transmits on the receive frequency); the left key on the tone scan line starts the VFO's tone scan
 * (vfo_screen.c); distance sort orders the channels of a zone by distance, not "all channels" (channel_screen.c).
 */

#include <stdio.h>
#include <string.h>
#include "util.h"
#include "widgets.h"
#include "text.h"
#include "list_screen.h"
#include "settings.h"
#include "codeplug.h"
#include "codeplug_text.h"
#include "vfo_screen.h"
#include "channel_details.h"
#include "quick_menu.h"

// Bits of settings.bitfield_options (the main firmware's BIT_x)
#define BIT_DUAL_WATCH    (1UL << 10) // BIT_DUAL_WATCH
#define BIT_DISTANCE_SORT (1UL << 20) // BIT_SORT_CHANNEL_DISTANCE

#define CC_FILTER        0x01 // settings.dmr_cc_ts_filter
#define TS_FILTER        0x02
#define ANALOG_FILTERS   2 // none, CTCSS|DCS
#define DMR_FILTERS      4 // none, TG, Ct, TGL
#define TONE_SCAN_TYPES  4 // all, CTCSS, DCS, iDCS
#define FLAG2_TIMESLOT_2 0x40
#define NAME_OFFSET      CODEPLUG_NAME_LENGTH // channel <-> VFO copies keep the name (the first 16 bytes)
#define STORED_SIZE      56
#define VALUE_SIZE       48

typedef enum {
    QM_CHANNEL_DETAILS, // the channel details screen of the channel or VFO
    QM_CHANNEL_TO_VFO,
    QM_VFO_TO_CHANNEL,
    QM_FILTER,
    QM_DMR_FILTER,
    QM_CC_SCAN,
    QM_TS_FILTER,
    QM_TALKAROUND,
    QM_DISTANCE_SORT,
    QM_DUAL_WATCH,
    QM_DUAL_ADD,
    QM_VFO_AB,
    QM_SWAP,
    QM_RX_TO_TX,
    QM_TX_TO_RX,
    QM_VFO_TO_NEW,
    QM_TONE_SCAN,
    QM_SCAN_LOW,  // the VFO's scan limits (vfo_screen.c scan): the RX frequency
    QM_SCAN_HIGH,
} item_t;

typedef struct {
    const uint8_t *items;
    uint8_t count;
} menu_t;

// The orders of CHANNEL_SCREEN_QUICK_MENU_ITEMS and VFO_SCREEN_QUICK_MENU_ITEMS (DM-32)
static const uint8_t channel_items[] = {QM_CHANNEL_DETAILS, QM_CHANNEL_TO_VFO, QM_VFO_TO_CHANNEL, QM_FILTER,
                                        QM_DMR_FILTER,      QM_CC_SCAN,        QM_TS_FILTER,      QM_TALKAROUND,
                                        QM_DISTANCE_SORT,   QM_DUAL_WATCH,     QM_DUAL_ADD};
static const uint8_t vfo_items[] = {QM_CHANNEL_DETAILS, QM_VFO_AB,    QM_SWAP,       QM_RX_TO_TX,  QM_TX_TO_RX,
                                    QM_FILTER,          QM_DMR_FILTER, QM_CC_SCAN,   QM_TS_FILTER, QM_VFO_TO_NEW,
                                    QM_TONE_SCAN,       QM_DUAL_WATCH, QM_SCAN_LOW,  QM_SCAN_HIGH, QM_DUAL_ADD};
static const menu_t channel_menu = {channel_items, sizeof(channel_items)};
static const menu_t vfo_menu = {vfo_items, sizeof(vfo_items)};

static const char *const dmr_filter_names[DMR_FILTERS] = {NULL, "TG", "Ct", "TGL"};
static const char *const tone_scan_names[TONE_SCAN_TYPES] = {NULL, "CTCSS", "DCS", "iDCS"};

static uint8_t tone_scan_type = 0; // kept between the menus, as the main firmware's toneScanCSS
static bool talkaround = false;
static int add_target = 1;         // the dual watch path "add" replaces; the next add replaces the other one
static int ref;                    // channel_details_current_ref() when the menu was opened
static int confirming = -1;        // the item asking "please confirm", -1: none
static int opened_vfo;             // the VFO in use when the menu was opened
static codeplug_channel_t channel; // not on the UI task stack

bool quick_menu_talkaround(void)
{
    return talkaround;
}

static const menu_t *menu(void)
{
    return (const menu_t *)ui_arg();
}

static const ui_screen_t quick_menu_screen;

static void open(const menu_t *m)
{
    ref = channel_details_current_ref();
    opened_vfo = vfo_screen_current();
    confirming = -1;
    ui_push(&quick_menu_screen, (void *)m);
    *ui_state() = ui_list_recall(ui_list_id((m == &vfo_menu) ? "VFO quick menu" : "Channel quick menu"), m->count);
}

void quick_menu_open_channel(void)
{
    open(&channel_menu);
}

void quick_menu_open_vfo(void)
{
    open(&vfo_menu);
}

static bool in_all_channels(void)
{
    return settings.current_zone == (codeplug_zone_count() - 1);
}

// The channel of the channel screen, into the static channel; false: none
static bool load_channel(void)
{
    return (ref > 0) && codeplug_get_channel(ref, &channel);
}

// A dual watch path: 0 is the default, VFO A for the first path, VFO B for the second (uiDualWatch.c pathRef())
static int16_t path_ref(int path)
{
    int16_t r = settings.dual_watch_path[path];

    return (r == 0) ? (int16_t)(-1 - path) : r;
}

static void path_name(int16_t r, char *out, size_t size)
{
    if ((r > 0) && codeplug_channel_in_use(r) && codeplug_get_channel(r, &channel)) {
        codeplug_text_name(channel.name, CODEPLUG_NAME_LENGTH, out, size);
    } else {
        snprintf(out, size, "VFO %c", ((r == -2) ? 'B' : 'A'));
    }
}

static bool tone_scan_possible(void)
{
    return vfo_screen_vfo(vfo_screen_current())->mode == CODEPLUG_MODE_FM;
}

static bool talkaround_possible(void)
{
    return load_channel() && (channel.tx_freq != channel.rx_freq);
}

static uint8_t dmr_filter(void)
{
    return (settings.dmr_destination_filter < DMR_FILTERS) ? settings.dmr_destination_filter : 0;
}

// LEFT (-1) / RIGHT (+1): the setting itself (saved when the menu is left)
static void change(item_t item, int direction)
{
    bool on = (direction > 0);

    switch (item) {
        case QM_FILTER:
            settings.analog_filter_level = (uint8_t)(on ? (ANALOG_FILTERS - 1) : 0);
            break;
        case QM_DMR_FILTER:
            if (on ? (dmr_filter() < (DMR_FILTERS - 1)) : (dmr_filter() > 0)) {
                settings.dmr_destination_filter = (uint8_t)(dmr_filter() + direction);
            }
            break;
        case QM_CC_SCAN: // CC scan on: the CC filter off
            settings.dmr_cc_ts_filter = (uint8_t)(on ? (settings.dmr_cc_ts_filter & ~CC_FILTER)
                                                     : (settings.dmr_cc_ts_filter | CC_FILTER));
            break;
        case QM_TS_FILTER:
            settings.dmr_cc_ts_filter = (uint8_t)(on ? (settings.dmr_cc_ts_filter | TS_FILTER)
                                                     : (settings.dmr_cc_ts_filter & ~TS_FILTER));
            break;
        case QM_TALKAROUND:
            talkaround = (on && talkaround_possible());
            return;
        case QM_DISTANCE_SORT:
            settings_set_option(BIT_DISTANCE_SORT, on && !in_all_channels());
            break;
        case QM_VFO_AB:
            settings.current_vfo_number = (uint8_t)(on ? 1 : 0);
            break;
        case QM_TONE_SCAN:
            if (tone_scan_possible() && (on ? (tone_scan_type < (TONE_SCAN_TYPES - 1)) : (tone_scan_type > 0))) {
                tone_scan_type = (uint8_t)(tone_scan_type + direction);
            }
            return;
        case QM_DUAL_ADD:
            add_target = 1 - add_target;
            return;
        default:
            return;
    }
    settings_changed();
}

// The frequencies of the VFO in use (after the VFO line was applied)
// The VFO's RX frequency as its scan low or high limit; the other one if they would cross (scan_range())
static void scan_limit(item_t item)
{
    int number = vfo_screen_current();
    uint32_t rx = vfo_screen_vfo(number)->rx_freq;
    uint32_t *low = &settings.vfo_scan_low[number];
    uint32_t *high = &settings.vfo_scan_high[number];

    if (item == QM_SCAN_LOW) {
        *low = rx;
    } else {
        *high = rx;
    }
    if ((*low != 0) && (*high != 0) && (*low > *high)) {
        uint32_t t = *low;

        *low = *high;
        *high = t;
    }
    settings_changed();
}

static void vfo_frequencies(item_t item)
{
    int number = vfo_screen_current();
    codeplug_channel_t *v = vfo_screen_vfo(number);
    uint32_t rx = v->rx_freq;

    if (item == QM_SWAP) {
        v->rx_freq = v->tx_freq;
        v->tx_freq = rx;
    } else if (item == QM_RX_TO_TX) {
        v->tx_freq = v->rx_freq;
    } else {
        v->rx_freq = v->tx_freq;
    }
    vfo_screen_save(number);
}

// The channel screen's channel into the VFO in use, without its name, then the VFO screen
static void channel_to_vfo(void)
{
    int number = vfo_screen_current();

    if (!load_channel()) {
        return;
    }
    memcpy((uint8_t *)vfo_screen_vfo(number) + NAME_OFFSET, (uint8_t *)&channel + NAME_OFFSET,
           STORED_SIZE - NAME_OFFSET);
    vfo_screen_save(number);
    vfo_screen_show();
}

// The VFO in use into the channel of the channel screen, without its name (validateOverwriteChannel())
static void vfo_to_channel(void)
{
    if (!load_channel()) {
        return;
    }
    memcpy((uint8_t *)&channel + NAME_OFFSET, (uint8_t *)vfo_screen_vfo(vfo_screen_current()) + NAME_OFFSET,
           STORED_SIZE - NAME_OFFSET);
    codeplug_write_channel(ref, &channel);
}

// The VFO in use as a new channel "New channel N" (validateNewChannel()), then the channel screen. It is in "all
// channels" only: it isn't added to the zone of the channel screen yet.
static void vfo_to_new_channel(void)
{
    char name[CODEPLUG_NAME_LENGTH + 8];
    int number = CODEPLUG_CHANNELS_MIN;

    while ((number <= CODEPLUG_CHANNELS_MAX) && codeplug_channel_in_use(number)) {
        number++;
    }
    memcpy(&channel, vfo_screen_vfo(vfo_screen_current()), sizeof(channel));
    channel.libre_dmr_power = (uint8_t)(settings.tx_power_level + 1); // a new channel: the default power
    snprintf(name, sizeof(name), "%s %d", text(TEXT_NEW_CHANNEL), number);
    memset(channel.name, 0xFF, CODEPLUG_NAME_LENGTH);
    memcpy(channel.name, name, MIN(strlen(name), (size_t)CODEPLUG_NAME_LENGTH));
    if (codeplug_write_new_channel(&channel) > 0) {
        vfo_screen_leave();
    }
}

// The left key on an item; the menu is left after it
static void select_item(item_t item)
{
    if (((item == QM_VFO_TO_CHANNEL) || (item == QM_VFO_TO_NEW)) && (confirming < 0)) {
        confirming = (int)item;
        return;
    }
    confirming = -1;

    // OK on the VFO line goes to the other VFO when it was not changed (no orange button on the DM-32)
    if ((item == QM_VFO_AB) && (vfo_screen_current() == opened_vfo)) {
        settings.current_vfo_number = (uint8_t)(1 - opened_vfo);
        settings_changed();
    }

    switch (item) {
        case QM_CHANNEL_DETAILS:
            settings_save_if_changed();
            ui_replace(&channel_details_screen, NULL);
            return;
        case QM_CHANNEL_TO_VFO:
            settings_save_if_changed();
            channel_to_vfo();
            return;
        case QM_VFO_TO_CHANNEL:
            vfo_to_channel();
            break;
        case QM_VFO_TO_NEW:
            settings_save_if_changed();
            vfo_to_new_channel();
            return;
        case QM_SWAP:
        case QM_RX_TO_TX:
        case QM_TX_TO_RX:
            vfo_frequencies(item);
            break;
        case QM_DUAL_WATCH:
            settings_set_option(BIT_DUAL_WATCH, !(settings.bitfield_options & BIT_DUAL_WATCH));
            break;
        case QM_SCAN_LOW:
        case QM_SCAN_HIGH:
            scan_limit(item);
            break;
        case QM_DUAL_ADD: // uiDualWatchAddPath(): the channel, or the VFO in use now
            settings.dual_watch_path[add_target] = (int16_t)channel_details_current_ref();
            add_target = 1 - add_target;
            break;
        case QM_TONE_SCAN:
            if (tone_scan_possible()) {
                settings_save_if_changed();
                ui_pop();
                vfo_screen_tone_scan(tone_scan_type);
                return;
            }
            break;
        default:
            break;
    }
    settings_save_if_changed();
    ui_pop();
}

static bool handle(const ui_event_t *ev)
{
    const menu_t *m = menu();
    int32_t *selected = ui_state();
    item_t item = (item_t)m->items[*selected];
    bool press = ((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT));

    // Yes / no: the left key, the right key
    if (confirming >= 0) {
        if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN)) {
            select_item((item_t)confirming);
        } else if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_RED)) {
            confirming = -1;
            return true;
        }
        return false;
    }

    if (ev->type == UI_EVENT_ROTARY) {
        for (int n = 0; n < ((ev->steps < 0) ? -ev->steps : ev->steps); n++) {
            change(item, ((ev->steps > 0) ? 1 : -1));
        }
        return true;
    }
    if (press && ((ev->key == UI_KEY_LEFT) || (ev->key == UI_KEY_RIGHT))) {
        change(item, ((ev->key == UI_KEY_RIGHT) ? 1 : -1));
        return true;
    }
    if (press && ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN))) {
        *selected = (*selected + ((ev->key == UI_KEY_DOWN) ? 1 : (m->count - 1))) % m->count;
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN)) { // the release of the long press: nothing
        select_item(item);
        return true;
    }
    list_handle_back(ev);
    return false;
}

static const char *on_off(bool on)
{
    return text(on ? TEXT_ON : TEXT_OFF);
}

// The name and the value of a line (updateQuickMenuScreen()); the value NULL: none
static const char *format_item(item_t item, char *name, size_t name_size, char *out, size_t size)
{
    static const text_id_t labels[] = {
        [QM_CHANNEL_DETAILS] = TEXT_CHANNEL_DETAILS,
        [QM_CHANNEL_TO_VFO] = TEXT_CHANNELTOVFO,
        [QM_VFO_TO_CHANNEL] = TEXT_VFOTOCHANNEL,
        [QM_FILTER] = TEXT_FILTER,
        [QM_DMR_FILTER] = TEXT_DMR_FILTER,
        [QM_CC_SCAN] = TEXT_DMR_CC_SCAN,
        [QM_TS_FILTER] = TEXT_DMR_TS_FILTER,
        [QM_TALKAROUND] = TEXT_TALKAROUND,
        [QM_DISTANCE_SORT] = TEXT_DISTANCE_SORT,
        [QM_DUAL_WATCH] = TEXT_DUAL_WATCH,
        [QM_DUAL_ADD] = TEXT_UI_DW_ADD,
        [QM_VFO_TO_NEW] = TEXT_VFOTONEWCHANNEL,
        [QM_TONE_SCAN] = TEXT_TONE_SCAN,
        [QM_SCAN_LOW] = TEXT_SCAN_LOW,
        [QM_SCAN_HIGH] = TEXT_SCAN_HIGH,
    };
    const char *value = NULL;

    name[0] = '\0';
    if (item < (sizeof(labels) / sizeof(labels[0]))) {
        snprintf(name, name_size, "%s", text(labels[item]));
    }

    switch (item) {
        case QM_VFO_AB:
            snprintf(name, name_size, "VFO %c", ((vfo_screen_current() == 0) ? 'A' : 'B'));
            break;
        case QM_SWAP:
            snprintf(name, name_size, "Tx <--> Rx");
            break;
        case QM_RX_TO_TX:
            snprintf(name, name_size, "Rx --> Tx");
            break;
        case QM_TX_TO_RX:
            snprintf(name, name_size, "Tx --> Rx");
            break;
        case QM_FILTER:
            value = ((settings.analog_filter_level != 0) ? "CTCSS|DCS" : text(TEXT_NONE));
            break;
        case QM_DMR_FILTER:
            value = (dmr_filter() ? dmr_filter_names[dmr_filter()] : text(TEXT_NONE));
            break;
        case QM_CC_SCAN:
            value = on_off(!(settings.dmr_cc_ts_filter & CC_FILTER));
            break;
        case QM_TS_FILTER:
            value = on_off(settings.dmr_cc_ts_filter & TS_FILTER);
            break;
        case QM_TALKAROUND:
            value = (talkaround_possible() ? on_off(talkaround) : text(TEXT_N_A));
            break;
        case QM_DISTANCE_SORT:
            value = (in_all_channels() ? text(TEXT_N_A) : on_off(settings.bitfield_options & BIT_DISTANCE_SORT));
            break;
        case QM_DUAL_WATCH:
            value = on_off(settings.bitfield_options & BIT_DUAL_WATCH);
            break;
        case QM_DUAL_ADD:
            // Only the path it replaces: the main firmware's "Replace VFO B" overlaps the name in English
            path_name(path_ref(add_target), out, size);
            value = out;
            break;
        case QM_TONE_SCAN:
            value = (!tone_scan_possible() ? text(TEXT_N_A)
                                           : (tone_scan_type ? tone_scan_names[tone_scan_type] : text(TEXT_ALL)));
            break;
        case QM_SCAN_LOW:
        case QM_SCAN_HIGH: { // the limit now
            uint32_t f = ((item == QM_SCAN_LOW) ? settings.vfo_scan_low : settings.vfo_scan_high)[vfo_screen_current()];

            if (f == 0) {
                value = text(TEXT_NONE);
            } else {
                snprintf(out, size, "%lu.%03lu", (unsigned long)(f / 100000), (unsigned long)((f % 100000) / 100));
                value = out;
            }
            break;
        }
        default:
            break;
    }
    return value;
}

static void draw(void)
{
    const menu_t *m = menu();
    int32_t selected = *ui_state();
    char position[16];

    if (confirming >= 0) {
        list_view_draw_title(text(TEXT_QUICK_MENU), NULL);
        list_view_draw_text(text((confirming == QM_VFO_TO_NEW) ? TEXT_NEW_CHANNEL : TEXT_OVERWRITE_QM),
                            text(TEXT_PLEASE_CONFIRM), THEME_COLOUR_TEXT);
        bottom_bar_draw_soft_keys(text(TEXT_YES), text(TEXT_NO));
        return;
    }

    snprintf(position, sizeof(position), "%d/%d", (int)(selected + 1), m->count);
    list_view_draw_title(text(TEXT_QUICK_MENU), position);

    for (int row = 0; row < LIST_VIEW_ROWS; row++) {
        int index = list_row_index(row, selected, m->count);
        if (index < 0) {
            break;
        }
        char name[VALUE_SIZE];
        char value[VALUE_SIZE];
        const char *shown = format_item((item_t)m->items[index], name, sizeof(name), value, sizeof(value));

        list_view_row_t data = {
            .icon = ICONS_COUNT,
            .name = name,
            .value = shown,
            .selected = (index == selected),
        };

        list_view_draw_row(row, &data);
    }

    bottom_bar_draw_soft_keys(text(TEXT_UI_SELECT), text(TEXT_UI_BACK));
}

static void leave(void)
{
    settings_save_if_changed(); // the changes are in use already; kept whichever way the menu is left
    ui_list_remember(ui_list_id((menu() == &vfo_menu) ? "VFO quick menu" : "Channel quick menu"), *ui_state());
}

static const ui_screen_t quick_menu_screen = {
    .leave = leave,
    .handle = handle,
    .draw = draw,
};
