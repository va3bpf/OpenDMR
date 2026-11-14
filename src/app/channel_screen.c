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
 * OpenDMR, screen: the channel screen of the main firmware (uiChannelMode.c drawChannelScreen(),
 * docs/ui-design.md 5.1): status bar, channel panel (zone and position, name, frequency, the TG or contact, or the
 * tone), S meter, bottom bar (mode, power, bandwidth). The rotary or UP / DOWN change the channel, the digits and the
 * left key go to a channel number of the zone, the left key opens the menu, the right key the VFO, the PTT transmits
 * (tx_screen.c). The shortcuts are the ones of keymap.c (OpenGD77: LEFT / RIGHT the TG or the squelch, SK2 + UP / DOWN
 * the zone, # the number entry, * the timeslot ...). The channel shown is the one the radio receives (radio.c), with
 * the timeslot and TG of tx_target.c; the squelch, power, mode and bandwidth changed here last until the channel
 * changes. The screen doesn't tune the radio: it gives its channel (radio_channel()) when the UI task asks, after each
 * event (ui_tune()), also while screens without their own channel (the menus) are over it.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "widgets.h"
#include "icons.h"
#include "codeplug_text.h"
#include "settings.h"
#include "codeplug.h"
#include "battery.h"
#include "datetime.h"
#include "channel_screen.h"
#include "tones.h"
#include "menus.h"
#include "quick_menu.h"
#include "channel_details.h"
#include "vfo_screen.h"
#include "lock_screen.h"
#include "radio.h"
#include "tx_screen.h"
#include "sound.h"
#include "dmr.h"
#include "systime.h"
#include "scan.h"
#include "dual_watch.h"
#include "qso_panel.h"
#include "util.h"
#include "keymap.h"
#include "quick_keys.h"
#include "tx_target.h"
#include "number_entry.h"
#include "fm_radio.h"
#include "contacts.h"
#include "aprs.h"

#define S_METER_Y 200 // under the channel panel, as the main firmware

#define POWER_LEVEL_MAX      8 // 10 W; 9 is the user power ("+W-")
#define POWER_USER           9
#define SQUELCH_MIN          1 // codeplug sql: 0 the band default, 1 .. 21: 0 .. 100 %
#define SQUELCH_MAX          21
#define SQUELCH_PERCENT_STEP 5
#define SQUELCH_SHOWN_MS     2000
#define FLAG2_TIMESLOT_2     0x40
#define FLAG4_BANDWIDTH_25K  0x02
#define FLAG1_FORCE_DMO      0x04 // libre_dmr_flag1
#define SIDE_KEYS            (UI_MODIFIER_SK1 | UI_MODIFIER_SK2)

// What is shown, for dm32dbg.py
volatile int channel_screen_zone = 0;
volatile int channel_screen_channel = 0;

static codeplug_zone_t zone;        // 176 bytes: not on the stack
static codeplug_channel_t channel;  // the channel shown; the channel details change it (channel_screen_edit())
static bool has_channel = false;    // a channel at the position (else none: the receiver off)
static codeplug_channel_t radio_ch; // the channel for the radio: reversed, the timeslot and TG of tx_target.c
static tx_target_t target;          // its TG (DMR)
static int zone_number = 0;
static int position = 0;
static bool green_down = false; // the left / right key went down on this screen: a release from a list isn't ours
static bool red_down = false;
static bool reversed = false;      // RX and TX frequencies swapped (reverse repeater)
static bool squelch_shown = false; // a squelch change is shown for a moment
static int direct_channel = 0;     // a channel number being typed
static uint32_t squelch_shown_until;

/* The scan (scan.c; uiChannelMode.c scanSearchForNextChannel()): the zone's channels in turn, skipping the ones
   marked (zone skip, or all-channels skip in the all channels zone) and the ones skipped this time (*) */

#define SCAN_NUISANCE_MAX   16
#define FLAG4_ZONE_SKIP     0x20
#define FLAG4_ALL_SKIP      0x10

static scan_t scan = {.swallow_key = UI_KEY_COUNT};
static int scan_nuisance[SCAN_NUISANCE_MAX];
static int scan_nuisance_count;

static int zone_size(void)
{
    return (zone.index >= 0) ? zone.channel_count : codeplug_channel_count();
}

// The channel number at a position of the zone (all channels: the position-th channel in use), 0: none
static int zone_channel(int at)
{
    if (zone.index >= 0) {
        return ((at >= 0) && (at < zone.channel_count)) ? zone.channels[at] : 0;
    }
    for (int number = CODEPLUG_CHANNELS_MIN; number <= CODEPLUG_CHANNELS_MAX; number++) {
        if (codeplug_channel_in_use(number) && (at-- == 0)) {
            return number;
        }
    }
    return 0;
}

// Where the last used channel of the zone is
static int zone_start_position(void)
{
    int last = codeplug_get_last_used_channel(zone.index);

    if (zone.index >= 0) {
        return (last < zone.channel_count) ? last : 0;
    }

    int at = 0;

    for (int number = CODEPLUG_CHANNELS_MIN; number < last; number++) {
        at += codeplug_channel_in_use(number) ? 1 : 0;
    }
    return at;
}

/* Distance sort (quick menu; uiChannelMode.c initSortedChannels()): a zone's channels by their distance from the
   location of the settings, the ones without a location (their "use location" flag, a place set) after, in their
   order. Not the "all channels" zone. */

#define BIT_SORT_DISTANCE    (1UL << 20) // settings.bitfield_options: BIT_SORT_CHANNEL_DISTANCE
#define FLAG1_USE_LOCATION   0x08
#define LOCATION_UNSET       0x7F000000UL
#define NO_LOCATION_DISTANCE 500000 // 0.1 km: further than anything

static double fixed24_degrees(uint8_t b0, uint8_t b1, uint8_t b2) // the channel's: sign 23, degrees 22:15, 1/10000
{
    uint32_t f = b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16);
    double value = ((f & 0x7FFFFF) >> 15) + ((f & 0x7FFF) / 10000.0);

    return (f & 0x800000) ? -value : value;
}

static double fixed32_degrees(uint32_t f) // the settings': sign 31, degrees 30:23, 1/100000
{
    double value = ((f & 0x7FFFFFFF) >> 23) + ((f & 0x7FFFFF) / 100000.0);

    return (f & 0x80000000) ? -value : value;
}

// distanceToLocation() in 0.1 km
static int32_t channel_distance(int number, int order)
{
    codeplug_channel_t c;

    if (codeplug_get_channel(number, &c) && (c.libre_dmr_flag1 & FLAG1_USE_LOCATION)) {
        uint32_t lat = c.location_lat0 | ((uint32_t)c.location_lat1 << 8) | ((uint32_t)c.location_lat2 << 16);
        uint32_t lon = c.location_lon0 | ((uint32_t)c.location_lon1 << 8) | ((uint32_t)c.location_lon2 << 16);

        if ((lat != 0) && (lon != 0)) {
            const double rad = 0.017453292519943295;
            double lat1 = fixed32_degrees(settings.location_lat) * rad;
            double lat2 = fixed24_degrees(c.location_lat0, c.location_lat1, c.location_lat2) * rad;
            double dlon = (fixed24_degrees(c.location_lon0, c.location_lon1, c.location_lon2) -
                           fixed32_degrees(settings.location_lon)) * rad;
            double x = acos(CLAMP((sin(lat1) * sin(lat2)) + (cos(lat1) * cos(lat2) * cos(dlon)), -1.0, 1.0));

            return (int32_t)(x * 6371.0 * 10.0);
        }
    }
    return NO_LOCATION_DISTANCE + order;
}

static void sort_by_distance(void)
{
    static int32_t distances[CODEPLUG_ZONE_CHANNELS];

    if (!(settings.bitfield_options & BIT_SORT_DISTANCE) || (settings.location_lat == LOCATION_UNSET) ||
        (zone.index < 0)) {
        return;
    }
    for (int i = 0; i < zone.channel_count; i++) {
        distances[i] = channel_distance(zone.channels[i], i);
    }
    for (int i = 1; i < zone.channel_count; i++) { // insertion sort: stable, 80 at most
        int32_t d = distances[i];
        uint16_t n = zone.channels[i];
        int j = i;

        for (; (j > 0) && (distances[j - 1] > d); j--) {
            distances[j] = distances[j - 1];
            zone.channels[j] = zone.channels[j - 1];
        }
        distances[j] = d;
        zone.channels[j] = n;
    }
}

static void select_zone(int number)
{
    int count = codeplug_zone_count();
    int start;

    zone_number = ((number % count) + count) % count;
    codeplug_get_zone(zone_number, &zone);
    position = zone_start_position();
    start = zone_channel(position);
    sort_by_distance();
    for (int i = 0; (zone.index >= 0) && (i < zone.channel_count); i++) {
        if (zone.channels[i] == start) {
            position = i; // the same channel, at its place in the sorted zone
            break;
        }
    }
    channel_screen_zone = zone_number;
}

// The channel for the radio, from the channel shown (radio_ch, and target): reversed (DMR: direct, as the main
// firmware), the timeslot and TG of tx_target.c. radio_channel() gives it to the UI task
static void compute_radio_channel(void)
{
    if (!has_channel) {
        memset(&radio_ch, 0, sizeof(radio_ch));
        memset(&target, 0, sizeof(target));
        return;
    }
    memcpy(&radio_ch, &channel, sizeof(radio_ch));
    if (reversed) {
        radio_ch.rx_freq = channel.tx_freq;
        radio_ch.tx_freq = channel.rx_freq;
        radio_ch.libre_dmr_flag1 |= FLAG1_FORCE_DMO;
    } else if (quick_menu_talkaround()) {
        radio_ch.tx_freq = channel.rx_freq; // talkaround (quick menu): direct on the repeater's output
        radio_ch.libre_dmr_flag1 |= FLAG1_FORCE_DMO;
    }
    tx_target_apply(&radio_ch, &target);
}

// The channel of the screen (ui_screen_t.channel): asked after each event, the same again changes nothing there
static const codeplug_channel_t *radio_channel(void)
{
    compute_radio_channel();
    return has_channel ? &radio_ch : NULL;
}

codeplug_channel_t *channel_screen_edit(void)
{
    return has_channel ? &channel : NULL;
}

// The channel at the position read again (it may have been edited; a change made here is undone)
static void load(void)
{
    int number = zone_channel(position);

    channel_screen_channel = number;
    has_channel = (number != 0) && codeplug_get_channel(number, &channel);
    if (!has_channel) {
        memset(&channel, 0, sizeof(channel));
        strcpy(channel.name, "--");
    }
}

static scan_kind_t scan_kind(void)
{
    return (channel.mode == CODEPLUG_MODE_DMR) ? SCAN_DMR : (channel.mode == CODEPLUG_MODE_P25) ? SCAN_P25 : SCAN_ANALOG;
}

static uint32_t scan_dwell(void)
{
    return scan_dwell_ms(scan_kind());
}

static bool scan_skipped(int at)
{
    int number = zone_channel(at);
    codeplug_channel_t c;

    if ((number == 0) || !codeplug_get_channel(number, &c) ||
        (c.flag4 & ((zone.index >= 0) ? FLAG4_ZONE_SKIP : FLAG4_ALL_SKIP))) {
        return true;
    }
    for (int i = 0; i < scan_nuisance_count; i++) {
        if (scan_nuisance[i] == number) {
            return true;
        }
    }
    return false;
}

// The next channel to scan, -1: none left (all skipped)
static int scan_next(void)
{
    int size = zone_size();

    for (int i = 1; i <= size; i++) {
        int at = (((position + (i * scan.direction)) % size) + size) % size;

        if ((at != position) && !scan_skipped(at)) {
            return at;
        }
    }
    return -1;
}

static void scan_begin(int direction)
{
    int at;

    if (zone_size() < 2) {
        ui_key_beep(SOUND_ERROR);
        return;
    }
    scan.direction = (int8_t)direction;
    scan_nuisance_count = 0;
    at = scan_next();
    if (at < 0) {
        ui_key_beep(SOUND_ERROR);
        return;
    }
    reversed = false;
    direct_channel = 0;
    if (scan_skipped(position)) {
        position = at; // starts on a channel it scans
        load();
    }
    scan_start(&scan, scan_dwell(), direction);
}

// On the timer while scanning; true: redraw
static bool scan_step(void)
{
    bool was_active = scan.active;
    int at;

    if (!scan_tick(&scan, scan_kind())) {
        return was_active && !scan.active; // stopped (stop mode)
    }
    at = scan_next();
    if (at < 0) {
        scan_stop(&scan);
        return true;
    }
    position = at;
    squelch_shown = false;
    load();
    scan_moved(&scan, scan_dwell());
    return true;
}

// The zone line ("Zone1  3/16") and the channel name, in UTF-8
static void format_names(char *zone_text, size_t zone_size_bytes, char *name, size_t name_size)
{
    char zone_name[48];

    if (zone.index >= 0) {
        codeplug_text_name(zone.name, CODEPLUG_NAME_LENGTH, zone_name, sizeof(zone_name));
        snprintf(zone_text, zone_size_bytes, "%s  %d/%d", zone_name, position + 1, zone_size());
    } else {
        snprintf(zone_text, zone_size_bytes, "%d/%d", position + 1, zone_size());
    }
    codeplug_text_name(channel.name, CODEPLUG_NAME_LENGTH, name, name_size);
}

// At the start, and back from the menus: the zone of the settings (the zone list may have changed it)
static void enter(void *arg)
{
    static bool started = false;
    static bool sorted = false;
    int current = (settings_valid() && (settings.current_zone >= 0)) ? settings.current_zone : 0;

    (void)arg;
    if (dual_watch_show()) {
        return; // dual watch on: it is shown over this screen
    }
    green_down = false;
    red_down = false;
    direct_channel = 0;
    bool sort = (settings.bitfield_options & BIT_SORT_DISTANCE) != 0;

    if (!started || (current != zone_number) || (sort != sorted)) {
        started = true;
        sorted = sort; // the distance sort turned on or off in the quick menu: the zone read again
        select_zone(current);
    }
    load(); // the channel may have been written (channel details, VFO --> channel) or the zone changed (zone list)
}

static void leave(void)
{
    scan_stop(&scan); // a screen over it: the scan ends
}

static void channel_changed(int new_position)
{
    position = new_position;
    if (position == 0) {
        ui_key_beep(SOUND_KEY_FIRST_ITEM);
    }
    reversed = false;
    squelch_shown = false;
    load();
}

static void zone_step(int step)
{
    select_zone(zone_number + step);
    settings.current_zone = (int16_t)zone_number; // saved with the next settings save
    settings_changed();
    channel_changed(position);
    if (zone_number == 0) {
        ui_key_beep(SOUND_KEY_FIRST_ITEM); // as uiChannelMode.c: the first zone, the first channel
    }
}

// increasePowerLevel(), decreasePowerLevel(): the channel's own power, kept in the codeplug. A channel without one
// (0: written so) starts from the radio's default power and has its own from then on
static void power_step(int direction, bool to_user)
{
    int level = (channel.libre_dmr_power != 0) ? (channel.libre_dmr_power - 1) : settings.tx_power_level;
    codeplug_channel_t stored;

    if (to_user) {
        level = POWER_USER;
    } else {
        level = CLAMP(((level > POWER_LEVEL_MAX) ? POWER_LEVEL_MAX : level) + direction, 0, POWER_LEVEL_MAX);
    }
    if (channel.libre_dmr_power != (uint8_t)(level + 1)) {
        channel.libre_dmr_power = (uint8_t)(level + 1);
        if ((channel_screen_channel > 0) && codeplug_get_channel(channel_screen_channel, &stored)) {
            stored.libre_dmr_power = channel.libre_dmr_power; // only the power: other changes here stay passing
            codeplug_write_channel(channel_screen_channel, &stored);
        }
    }
    if (level == 0) {
        ui_key_beep(SOUND_KEY_FIRST_ITEM);
    }
}

// uiChannelMode.c: from the band's default when the channel has none (0)
static void squelch_step(int direction)
{
    int sql = (channel.sql != 0) ? channel.sql : settings.squelch_defaults[radio_band_of(channel.rx_freq)];

    channel.sql = (uint8_t)CLAMP(sql + direction, SQUELCH_MIN, SQUELCH_MAX);
    if (channel.sql == SQUELCH_MIN) {
        ui_key_beep(SOUND_KEY_FIRST_ITEM);
    }
    squelch_shown = true;
    squelch_shown_until = systime_get_ms() + SQUELCH_SHOWN_MS;
    if (!scan.active) {
        ui_timer_start(SQUELCH_SHOWN_MS);
    }
}

// SK2 while a call is shown: transmit to its TG (a private call: to the caller) on its timeslot
static void reply_to_call(void)
{
    dmr_call_t call;

    if ((channel.mode != CODEPLUG_MODE_DMR) || !dmr_call(&call)) {
        return;
    }

    bool pc = ((call.tg_or_pc >> 24) == TX_TARGET_PC_FLAG);
    uint32_t id = (pc ? call.source : call.tg_or_pc) & CODEPLUG_CONTACT_ID_MAX;
    uint32_t packed = id | (pc ? ((uint32_t)TX_TARGET_PC_FLAG << 24) : 0);

    if ((packed == target.id) && ((call.timeslot + 1) == target.timeslot)) {
        return;
    }
    tx_target_set(id, pc);
    tx_target_set_timeslot(call.timeslot + 1);
    ui_key_beep(SOUND_ACK);
}

static bool do_action(keymap_action_t action)
{
    bool dmr = (channel.mode == CODEPLUG_MODE_DMR);

    switch (action) {
        case KEYMAP_NUMBER_ENTRY:
            ui_push(&number_entry_screen, (dmr) ? NUMBER_ENTRY_TX_TARGET : NUMBER_ENTRY_DTMF);
            return false;
        case KEYMAP_CONTACT_LIST:
            contacts_open_quick_list(!dmr);
            return false;
        case KEYMAP_REVERSE_REPEATER:
            reversed = !reversed;
            break;
        case KEYMAP_TS_OR_BANDWIDTH:
            if (dmr) {
                tx_target_toggle_timeslot(&channel);
                compute_radio_channel();
                ui_key_beep((radio_ch.flag2 & FLAG2_TIMESLOT_2) ? SOUND_KEY_FIRST_ITEM : SOUND_KEY); // back to TS1
            } else if (channel.mode == CODEPLUG_MODE_FM) { // AM and SSB have their own filters
                channel.flag4 ^= FLAG4_BANDWIDTH_25K;
                ui_key_beep((channel.flag4 & FLAG4_BANDWIDTH_25K) ? SOUND_KEY_FIRST_ITEM : SOUND_KEY);
            }
            break;
        case KEYMAP_TS_CLEAR:
            if (dmr) {
                tx_target_clear_timeslot();
            }
            break;
        case KEYMAP_MODE: // FM, AM, USB, LSB, DMR
            channel.mode = codeplug_mode_step(channel.mode, 1);
            ui_key_beep((channel.mode == CODEPLUG_MODE_FM) ? SOUND_KEY_FIRST_ITEM : SOUND_KEY);
            break;
        case KEYMAP_TG_OR_SQUELCH_UP:
        case KEYMAP_TG_OR_SQUELCH_DOWN: {
            int step = (action == KEYMAP_TG_OR_SQUELCH_UP) ? 1 : -1;

            if (!dmr) {
                squelch_step(step);
            } else if (tx_target_step_group(&channel, step) == 0) {
                ui_key_beep(SOUND_KEY_FIRST_ITEM);
            }
            break;
        }
        case KEYMAP_POWER_UP:
        case KEYMAP_POWER_DOWN:
            power_step((action == KEYMAP_POWER_UP) ? 1 : -1, false);
            break;
        case KEYMAP_POWER_USER:
            power_step(0, true);
            break;
        case KEYMAP_ZONE_UP:
        case KEYMAP_ZONE_DOWN:
            zone_step((action == KEYMAP_ZONE_UP) ? 1 : -1);
            return true;
        case KEYMAP_TG_FROM_CALL:
            reply_to_call();
            break;
        case KEYMAP_FM_RADIO:
            ui_push(&fm_radio_screen, NULL);
            break;
        case KEYMAP_SCAN:
        case KEYMAP_SCAN_DOWN:
            scan_begin((action == KEYMAP_SCAN) ? 1 : -1);
            break;
        case KEYMAP_APRS_TOGGLE:
            aprs_toggle();
            return true;
        case KEYMAP_APRS_SEND:
            aprs_send();
            return true;
        default:
            return false;
    }
    return true;
}

// The digits: a channel number of the zone, gone to with the left key (uiChannelMode.c directChannelNumber); the
// right key drops it
static bool direct_channel_key(const ui_event_t *ev)
{
    int digit = ui_key_digit(ev->key);

    if ((ev->type == UI_EVENT_KEY_PRESS) && (digit >= 0) && !(ev->modifiers & SIDE_KEYS)) {
        direct_channel = (direct_channel * 10) + digit;
        if ((direct_channel == 0) || (direct_channel > zone_size())) {
            direct_channel = 0;
            ui_key_beep(SOUND_ERROR);
        }
        return true;
    }
    if ((direct_channel == 0) || (ev->type != UI_EVENT_KEY_RELEASE)) {
        return false;
    }
    if ((ev->key == UI_KEY_GREEN) && green_down) {
        green_down = false;
        channel_changed(direct_channel - 1);
        direct_channel = 0;
        return true;
    }
    if ((ev->key == UI_KEY_RED) && red_down) {
        red_down = false;
        direct_channel = 0;
        return true;
    }
    return false;
}

static bool handle_key(const ui_event_t *ev)
{
    keymap_action_t action;

    switch (scan_key(&scan, ev)) {
        case SCAN_KEY_SKIP:
            if (scan_nuisance_count < SCAN_NUISANCE_MAX) {
                scan_nuisance[scan_nuisance_count++] = zone_channel(position);
            }
            // fall through
        case SCAN_KEY_USED:
        case SCAN_KEY_STOPPED:
            (void)keymap_action(ev, KEYMAP_SCREEN_CHANNEL); // keeps the side key state
            return true;
        default:
            break;
    }
    action = keymap_action(ev, KEYMAP_SCREEN_CHANNEL);

    if (ev->type == UI_EVENT_KEY_PRESS) {
        green_down |= (ev->key == UI_KEY_GREEN);
        red_down |= (ev->key == UI_KEY_RED);
    }
    if (action != KEYMAP_NONE) {
        direct_channel = 0;
        return do_action(action);
    }
    if (quick_keys_home(ev)) {
        return true;
    }
    if (direct_channel_key(ev)) {
        return true;
    }

    switch (ev->type) {
        case UI_EVENT_KEY_PRESS:
        case UI_EVENT_KEY_REPEAT:
            if ((ev->key == UI_KEY_PTT) && (ev->type == UI_EVENT_KEY_PRESS)) {
                char zone_text[64];
                char name[48];

                format_names(zone_text, sizeof(zone_text), name, sizeof(name));
                tx_screen_open(zone_text, name, &radio_ch);
                return false;
            }
            if (((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN)) && !(ev->modifiers & SIDE_KEYS) &&
                (zone_size() > 0)) {
                int step = (ev->key == UI_KEY_UP) ? 1 : -1;

                channel_changed((((position + step) % zone_size()) + zone_size()) % zone_size());
                return true;
            }
            return false;
        case UI_EVENT_KEY_LONG: // left key held: the main menu
            if ((ev->key == UI_KEY_GREEN) && green_down) {
                green_down = false;
                ui_push(&list_screen, (void *)&main_menu);
            } else if (ev->key == UI_KEY_RED) {
                red_down = false;
            }
            return false;
        case UI_EVENT_KEY_RELEASE: // left key: the menu of the channel (with SK2: its details), right key: the VFO
            if ((ev->key == UI_KEY_GREEN) && green_down) {
                green_down = false;
                if (ev->modifiers & UI_MODIFIER_SK2) {
                    ui_push(&channel_details_screen, NULL);
                } else {
                    quick_menu_open_channel();
                }
            } else if ((ev->key == UI_KEY_RED) && red_down) {
                red_down = false;
                vfo_screen_show();
            }
            return false;
        default:
            return false;
    }
}

static bool handle(const ui_event_t *ev)
{
    if (lock_screen_filter(ev)) {
        return false;
    }

    switch (ev->type) {
        case UI_EVENT_ROTARY:
            scan_stop(&scan);
            if (zone_size() > 0) {
                channel_changed((((position + ev->steps) % zone_size()) + zone_size()) % zone_size());
            }
            return true;
        case UI_EVENT_KEY_PRESS:
        case UI_EVENT_KEY_REPEAT:
        case UI_EVENT_KEY_LONG:
        case UI_EVENT_KEY_RELEASE:
            return handle_key(ev);
        case UI_EVENT_TIMER: {
            bool redraw = false;

            if (squelch_shown && ((int32_t)(systime_get_ms() - squelch_shown_until) >= 0)) {
                squelch_shown = false;
                redraw = true;
            }
            if (scan.active) {
                redraw |= scan_step();
                if (scan.active) {
                    ui_timer_start(SCAN_TICK_MS);
                } else if (squelch_shown) {
                    ui_timer_start(squelch_shown_until - systime_get_ms());
                }
            }
            return redraw;
        }
        case UI_EVENT_DATA:
            if ((ev->what & UI_DATA_AUTOLOCK) && !lock_screen_is_keypad_locked()) {
                lock_screen_lock(true, true); // the auto lock (keypad and PTT, as the main firmware)
                return false;
            }
            return true; // the status bar shows the battery and the time, the S meter the signal
        default:
            return false;
    }
}

static void draw(void)
{
    status_bar_data_t status = {0};
    channel_panel_data_t panel = {0};
    bottom_bar_data_t bottom = {0};
    s_meter_data_t meter = {0};
    char zone_text[64];
    char name[48];
    char freq[24];
    char line[24];
    char ts_text[8];
    char cc_text[16];
    char detail[24];
    char squelch[24];
    dmr_call_t call;
    p25_call_t p25;

    if ((channel.mode == CODEPLUG_MODE_DMR) && dmr_call(&call)) {
        format_names(zone_text, sizeof(zone_text), name, sizeof(name));
        qso_panel_draw(&call, name);
        return;
    }
    if ((channel.mode == CODEPLUG_MODE_P25) && p25_radio_call(&p25) && (p25.src != 0)) {
        format_names(zone_text, sizeof(zone_text), name, sizeof(name));
        qso_panel_draw_p25(&p25, name);
        return;
    }
    meter.active = true;
    meter.open = radio_is_receiving();
    s_meter_set_signal(&meter, radio_signal());
    status.state = meter.open ? STATUS_BAR_RECEIVING : STATUS_BAR_IDLE;
    status.scanning = scan.active;
    status.signal_bars = (int8_t)((meter.s_units * ICONS_SIGNAL_BARS) / 9);

    format_names(zone_text, sizeof(zone_text), name, sizeof(name));
    if (direct_channel > 0) {
        snprintf(name, sizeof(name), "-> %d", direct_channel);
    }
    snprintf(freq, sizeof(freq), "%lu.%04lu MHz", (unsigned long)(radio_ch.rx_freq / 100000),
             (unsigned long)((radio_ch.rx_freq % 100000) / 10));
    panel.zone = zone_text;
    panel.name = name;
    panel.frequency = freq;
    panel.line_icon = ICONS_COUNT;

    if (channel.mode == CODEPLUG_MODE_DMR) {
        snprintf(ts_text, sizeof(ts_text), "TS%d", target.timeslot);
        snprintf(cc_text, sizeof(cc_text), "CC%d", channel.tx_colour);
        status.item1 = ts_text;
        status.item1_bold = true;
        status.item2 = cc_text;
        if (target.id != 0) {
            bool pc = ((target.id >> 24) == TX_TARGET_PC_FLAG);

            panel.line_icon = pc ? ICONS_CONTACT : ICONS_TALKGROUP;
            panel.line_text = target.name;
            snprintf(detail, sizeof(detail), "%s %lu", text(pc ? TEXT_PC : TEXT_TG),
                     (unsigned long)(target.id & CODEPLUG_CONTACT_ID_MAX));
            if (strcmp(detail, target.name) != 0) {
                panel.line_detail = detail;
            }
        }
        bottom.mode = "DMR";
        bottom.item2 = "12.5K";
    } else if (channel.mode == CODEPLUG_MODE_P25) {
        // P25: the NAC where DMR has the colour code; the talk group transmitted to (tx_target_apply())
        codeplug_text_nac(codeplug_p25_nac(&channel), cc_text, sizeof(cc_text));
        status.item2 = cc_text;
        if (target.id != 0) {
            panel.line_icon = ICONS_TALKGROUP;
            panel.line_text = target.name;
            snprintf(detail, sizeof(detail), "%s %lu", text(TEXT_TG), (unsigned long)(target.id & 0xFFFF));
            if (strcmp(detail, target.name) != 0) {
                panel.line_detail = detail;
            }
        }
        bottom.mode = "P25";
        bottom.item2 = "12.5K";
    } else {
        uint16_t tone = (channel.rx_tone != CODEPLUG_CSS_NONE) ? channel.rx_tone : channel.tx_tone;

        if (tone != CODEPLUG_CSS_NONE) {
            tones_format(tone, line, sizeof(line));
            panel.line_icon = ICONS_TONE;
            panel.line_text = line;
        }
        bottom.mode = codeplug_mode_name(channel.mode);
        bottom.item2 = (((channel.mode == CODEPLUG_MODE_FM) && (channel.flag4 & FLAG4_BANDWIDTH_25K)) ? "25K" : "12.5K");
        if (codeplug_mode_is_ssb(channel.mode)) {
            bottom.item2 = "3K";
        }
        if (squelch_shown) {
            int sql = (channel.sql != 0) ? channel.sql : settings.squelch_defaults[radio_band_of(channel.rx_freq)];

            snprintf(squelch, sizeof(squelch), "%s %d%%", text(TEXT_SQUELCH),
                     (sql - SQUELCH_MIN) * SQUELCH_PERCENT_STEP);
            bottom.right = squelch;
        }
    }
    if (reversed && (bottom.right == NULL)) {
        bottom.right = text(TEXT_UI_REVERSED);
    }

    int power = (channel.libre_dmr_power != 0) ? (channel.libre_dmr_power - 1) : settings.tx_power_level;

    bottom.item1 = (codeplug_mode_can_transmit(channel.mode) && codeplug_frequency_can_transmit(channel.tx_freq))
                       ? bottom_bar_power_label((power < 10) ? power : 9)
                       : text(TEXT_RX_ONLY); // AM, USB, LSB; the aviation band

    status_bar_draw(&status);
    channel_panel_draw(&panel);
    s_meter_draw(S_METER_Y, &meter);
    bottom_bar_draw(&bottom);
}

const ui_screen_t channel_screen = {
    .enter = enter,
    .leave = leave,
    .handle = handle,
    .draw = draw,
    .channel = radio_channel,
};
