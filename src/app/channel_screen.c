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
 * the timeslot and TG of tx_target.c. The squelch, power, mode, bandwidth, timeslot and TG changed here with the keys
 * are for now: never written, gone with another channel, the VFO, the channel details or power off (temp); only the
 * channel details change the channel. The screen doesn't tune the radio: it gives its channel (radio_channel()) when
 * the UI task asks, after each event (ui_tune()), also while screens without their own channel (the menus) are over it.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "widgets.h"
#include "icons.h"
#include "codeplug_text.h"
#include "settings.h"
#include "extra_options.h"
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
#include "last_heard.h"
#include "last_heard_format.h"
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

#define S_METER_Y 250 // the compact one over the bottom bar, as on the VFO screen

#define POWER_LEVEL_MAX      8 // 10 W; 9 is the user power ("+W-")
#define POWER_USER           9
#define SQUELCH_MIN          1 // codeplug sql: 0 the band default, 1 .. 21: 0 .. 100 %
#define SQUELCH_MAX          21
#define SQUELCH_PERCENT_STEP 5
#define FLAG2_TIMESLOT_2     0x40
#define FLAG4_BANDWIDTH_25K  0x02
#define FLAG4_RX_ONLY        0x04 // services/radio_internal.h
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
static int direct_channel = 0;     // a channel number being typed
static last_heard_entry_t recent_entry; // 80 bytes: not on the UI task stack
static char recent_name[CHANNEL_PANEL_RECENT][LAST_HEARD_TEXT_SIZE];
static char recent_detail[CHANNEL_PANEL_RECENT][16];
static char recent_when[CHANNEL_PANEL_RECENT][16];

// The squelch, power, mode and bandwidth changed with the keys on this screen: the channel's for now, never written
// (only the channel details write a channel). Gone with another channel, the VFO, the channel details or power off,
// as the TG and timeslot picked here (tx_target_channel_forget()).
#define TEMP_SQL   0x01
#define TEMP_POWER 0x02
#define TEMP_MODE  0x04
#define TEMP_WIDE  0x08
static struct {
    int number; // the channel they belong to; 0: none
    uint8_t what; // TEMP_x
    uint8_t sql;
    uint8_t power;
    uint8_t mode;
    bool wide;
} temp;

/* The scan (scan.c; uiChannelMode.c scanSearchForNextChannel()): the zone's channels in turn, skipping the ones
   marked (zone skip, or all-channels skip in the all channels zone) and the ones skipped with * (scan_skipped_channels,
   until cleared in the quick menu or power off) */

#define FLAG4_ZONE_SKIP     0x20
#define FLAG4_ALL_SKIP      0x10

static scan_t scan = {.swallow_key = UI_KEY_COUNT};

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

void channel_screen_forget_changes(void)
{
    temp.number = 0;
    temp.what = 0;
    tx_target_channel_forget();
}

void channel_screen_forget_power(void)
{
    temp.what &= (uint8_t)~TEMP_POWER;
}

// A change made with the keys, kept for this channel (temp)
static void keep(uint8_t what)
{
    temp.number = channel_screen_channel;
    temp.what |= what;
    temp.sql = channel.sql;
    temp.power = channel.libre_dmr_power;
    temp.mode = channel.mode;
    temp.wide = ((channel.flag4 & FLAG4_BANDWIDTH_25K) != 0);
}

// The channel at the position read again (it may have been edited), with the changes made here for it (temp); another
// channel: they are gone
static void load(void)
{
    int number = zone_channel(position);

    channel_screen_channel = number;
    // where the zone is: the channel shown again after power on (written at power off; not the scan's steps)
    if ((number != 0) && !scan.active) {
        codeplug_set_last_used_channel(zone.index, (zone.index >= 0) ? position : number);
    }
    has_channel = (number != 0) && codeplug_get_channel(number, &channel);
    if (!has_channel) {
        memset(&channel, 0, sizeof(channel));
        strcpy(channel.name, "--");
    }
    if (number != temp.number) {
        channel_screen_forget_changes();
        temp.number = number;
        return;
    }
    if (temp.what & TEMP_SQL) {
        channel.sql = temp.sql;
    }
    if (temp.what & TEMP_POWER) {
        channel.libre_dmr_power = temp.power;
    }
    if (temp.what & TEMP_MODE) {
        channel.mode = temp.mode;
    }
    if (temp.what & TEMP_WIDE) {
        channel.flag4 = (uint8_t)(temp.wide ? (channel.flag4 | FLAG4_BANDWIDTH_25K) : (channel.flag4 & ~FLAG4_BANDWIDTH_25K));
    }
}

// For the channel details: the channel as it is in the codeplug (the changes made with the keys gone), changed there
// in place and written by them
codeplug_channel_t *channel_screen_edit(void)
{
    if (has_channel) {
        channel_screen_forget_changes();
        load();
    }
    return has_channel ? &channel : NULL;
}

static scan_kind_t scan_kind(void)
{
    return (channel.mode == CODEPLUG_MODE_DMR) ? SCAN_DMR : (channel.mode == CODEPLUG_MODE_P25) ? SCAN_P25
         : (channel.mode == CODEPLUG_MODE_NXDN) ? SCAN_NXDN
         : (channel.mode == CODEPLUG_MODE_YSF) ? SCAN_YSF : SCAN_ANALOG;
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
    return scan_skipped_has(&scan_skipped_channels, (uint32_t)number);
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
    (void)dual_watch_show(); // dual watch on: shown over this screen, its selected path this channel
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
    load();
}

// The position of a channel number in the zone; -1: not in it
static int position_of(int number)
{
    if (zone.index < 0) { // all channels: the channels in use before it
        int at = 0;

        if (!codeplug_channel_in_use(number)) {
            return -1;
        }
        for (int n = CODEPLUG_CHANNELS_MIN; n < number; n++) {
            at += codeplug_channel_in_use(n) ? 1 : 0;
        }
        return at;
    }
    for (int at = 0; at < zone.channel_count; at++) {
        if (zone.channels[at] == number) {
            return at;
        }
    }
    return -1;
}

bool channel_screen_select(int number)
{
    int at;

    if ((number < CODEPLUG_CHANNELS_MIN) || !codeplug_channel_in_use(number)) {
        return false;
    }
    // In the zone shown (or the settings' one, the screen not shown yet), else in all channels. The last used channel
    // of the zone is written by load(): the screen's enter, which may read the zone again, finds it there
    at = position_of(number);
    if ((at < 0) && (settings.current_zone >= 0)) {
        select_zone(settings.current_zone);
        at = position_of(number);
    }
    if (at < 0) {
        select_zone(codeplug_zone_count() - 1);
        at = position_of(number);
    }
    if (at < 0) {
        return false;
    }
    settings.current_zone = (int16_t)zone_number; // the screen's enter keeps this zone
    settings_changed();
    position = at;
    reversed = false;
    direct_channel = 0;
    load();
    return true;
}

void channel_screen_set_squelch(uint8_t sql)
{
    if (has_channel) {
        channel.sql = (uint8_t)CLAMP(sql, 0, SQUELCH_MAX);
        keep(TEMP_SQL);
    }
}

int channel_screen_typed(void)
{
    return direct_channel;
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

// increasePowerLevel(), decreasePowerLevel(): the channel's power for now (temp; the channel details change its own).
// A channel without one (0) starts from the radio's default power
static void power_step(int direction, bool to_user)
{
    int level = (channel.libre_dmr_power != 0) ? (channel.libre_dmr_power - 1) : settings.tx_power_level;

    if (to_user) {
        level = POWER_USER;
    } else {
        level = CLAMP(((level > POWER_LEVEL_MAX) ? POWER_LEVEL_MAX : level) + direction, 0, POWER_LEVEL_MAX);
    }
    if (channel.libre_dmr_power != (uint8_t)(level + 1)) {
        channel.libre_dmr_power = (uint8_t)(level + 1);
        keep(TEMP_POWER);
    }
    if (level == 0) {
        ui_key_beep(SOUND_KEY_FIRST_ITEM);
    }
}

// uiChannelMode.c: from the band's default when the channel has none (0); the channel's for now (temp)
static void squelch_step(int direction)
{
    int sql = (channel.sql != 0) ? channel.sql : settings.squelch_defaults[radio_band_of(channel.rx_freq)];

    channel.sql = (uint8_t)CLAMP(sql + direction, SQUELCH_MIN, SQUELCH_MAX);
    keep(TEMP_SQL);
    if (channel.sql == SQUELCH_MIN) {
        ui_key_beep(SOUND_KEY_FIRST_ITEM);
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
    bool analog = codeplug_mode_is_analog(channel.mode); // as the VFO: P25, NXDN, M17 have TGs too, not DTMF

    switch (action) {
        case KEYMAP_NUMBER_ENTRY:
            ui_push(&number_entry_screen, analog ? NUMBER_ENTRY_DTMF : NUMBER_ENTRY_TX_TARGET);
            return false;
        case KEYMAP_CONTACT_LIST:
            contacts_open_quick_list(analog);
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
                keep(TEMP_WIDE);
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
            keep(TEMP_MODE);
            ui_key_beep((channel.mode == CODEPLUG_MODE_FM) ? SOUND_KEY_FIRST_ITEM : SOUND_KEY);
            break;
        case KEYMAP_TG_OR_SQUELCH_UP:
        case KEYMAP_TG_OR_SQUELCH_DOWN: {
            int step = (action == KEYMAP_TG_OR_SQUELCH_UP) ? 1 : -1;

            if (analog) {
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
        case KEYMAP_LOCK:
            lock_screen_lock(true, lock_screen_is_ptt_locked());
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

// * held on a scan's channel: its skip flag set in the codeplug (Zone Skip, or All Skip in all channels)
static bool skip_for_good(int number)
{
    codeplug_channel_t c;

    if ((number == 0) || !codeplug_get_channel(number, &c)) {
        return false;
    }
    c.flag4 |= ((zone.index >= 0) ? FLAG4_ZONE_SKIP : FLAG4_ALL_SKIP);
    return codeplug_write_channel(number, &c);
}

int channel_screen_scanning(void)
{
    return scan_on_signal(&scan) ? 2 : (scan.active ? 1 : 0);
}

bool channel_screen_lock_out(int number)
{
    return skip_for_good(number);
}

static bool handle_key(const ui_event_t *ev)
{
    keymap_action_t action;

    switch (scan_key(&scan, ev)) {
        case SCAN_KEY_SKIP:
            ui_key_beep(scan_skipped_add(&scan_skipped_channels, (uint32_t)zone_channel(position)) ? SOUND_KEY
                                                                                                  : SOUND_ERROR);
            (void)keymap_action(ev, KEYMAP_SCREEN_CHANNEL);
            return true;
        case SCAN_KEY_SKIP_SAVED:
            ui_key_beep(skip_for_good(zone_channel(position)) ? SOUND_ACK : SOUND_ERROR);
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

            if (scan.active) {
                redraw = scan_step();
                if (scan.active) {
                    ui_timer_start(SCAN_TICK_MS);
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

// The power of the bottom bar: the channel's, else the settings'; "Rx only" when it can't transmit
static const char *power_text(void)
{
    int power = (channel.libre_dmr_power != 0) ? (channel.libre_dmr_power - 1) : settings.tx_power_level;

    return (codeplug_mode_can_transmit(channel.mode) && codeplug_frequency_can_transmit(channel.tx_freq))
               ? bottom_bar_power_label((power < 10) ? power : 9)
               : text(TEXT_RX_ONLY); // AM, USB, LSB; the aviation band
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
    char tx_freq[24];
    char line[24];
    char cc_text[16];
    char detail[24];
    char squelch[24];
    uint32_t now = systime_get_ms();
    dmr_call_t call;
    p25_call_t p25;
    nxdn_call_t nxdn;
    ysf_call_t ysf;
    m17_call_t m17;

    if ((channel.mode == CODEPLUG_MODE_DMR) && dmr_call(&call)) {
        format_names(zone_text, sizeof(zone_text), name, sizeof(name));
        qso_panel_draw(&call, name, codeplug_colour_code(&channel), power_text());
        return;
    }
    if ((channel.mode == CODEPLUG_MODE_P25) && p25_radio_call(&p25) && (p25.src != 0)) {
        format_names(zone_text, sizeof(zone_text), name, sizeof(name));
        qso_panel_draw_p25(&p25, name, power_text());
        return;
    }
    if ((channel.mode == CODEPLUG_MODE_NXDN) && nxdn_radio_call(&nxdn) && (nxdn.src != 0)) {
        format_names(zone_text, sizeof(zone_text), name, sizeof(name));
        qso_panel_draw_nxdn(&nxdn, name, power_text());
        return;
    }
    if ((channel.mode == CODEPLUG_MODE_M17) && m17_radio_call(&m17) && m17.src[0]) {
        format_names(zone_text, sizeof(zone_text), name, sizeof(name));
        qso_panel_draw_m17(&m17, name, power_text());
        return;
    }
    if ((channel.mode == CODEPLUG_MODE_YSF) && ysf_radio_call(&ysf)) {
        format_names(zone_text, sizeof(zone_text), name, sizeof(name));
        qso_panel_draw_ysf(&ysf, name, power_text());
        return;
    }
    meter.active = true;
    meter.open = radio_is_receiving();
    s_meter_set_signal(&meter, radio_signal_dbm());
    status.state = meter.open ? STATUS_BAR_RECEIVING : STATUS_BAR_IDLE;
    status.scanning = scan.active;
    status.signal_bars = s_meter_bars(&meter);

    format_names(zone_text, sizeof(zone_text), name, sizeof(name));
    if (scan.active && scan_skipped_channels.count) { // nuisance delete: how many are left out
        size_t n = strlen(zone_text);

        snprintf(&zone_text[n], sizeof(zone_text) - n, "  %s %u", text(TEXT_SCAN_SKIPPED),
                 (unsigned)scan_skipped_channels.count);
    }
    if (direct_channel > 0) {
        snprintf(name, sizeof(name), "-> %d", direct_channel);
    }
    snprintf(freq, sizeof(freq), "%lu.%04lu", (unsigned long)(radio_ch.rx_freq / 100000), // "MHz": channel_panel.c
             (unsigned long)((radio_ch.rx_freq % 100000) / 10));
    if (radio_ch.flag4 & FLAG4_RX_ONLY) {
        snprintf(tx_freq, sizeof(tx_freq), "--");
    } else {
        snprintf(tx_freq, sizeof(tx_freq), "%lu.%04lu", (unsigned long)(radio_ch.tx_freq / 100000),
                 (unsigned long)((radio_ch.tx_freq % 100000) / 10));
    }
    panel.zone = zone_text;
    panel.name = name;
    panel.frequency = freq;
    panel.tx_frequency = tx_freq;
    panel.line_icon = ICONS_COUNT;

    if (channel.mode == CODEPLUG_MODE_DMR) {
        // the timeslot and the colour code in the bottom bar (DMR's bandwidth is always 12.5K): the status bar
        // keeps room for the unread messages, the TG line for a long name
        snprintf(cc_text, sizeof(cc_text), "TS%d  CC%d", target.timeslot, codeplug_colour_code(&channel));
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
        bottom.item2 = NULL;
        bottom.right = cc_text; // right aligned
    } else if (channel.mode == CODEPLUG_MODE_P25) {
        // P25: the talk group transmitted to (tx_target_apply()), the NAC on its line (not in the status bar: it is
        // long, and the status bar has the GPS and scan icons too)
        codeplug_text_nac(codeplug_p25_nac(&channel), cc_text, sizeof(cc_text));
        panel.line_icon = ICONS_TALKGROUP;
        panel.line_right = cc_text;
        if (target.id != 0) {
            panel.line_text = target.name;
            snprintf(detail, sizeof(detail), "%s %lu", text(TEXT_TG), (unsigned long)(target.id & 0xFFFF));
            if (strcmp(detail, target.name) != 0) {
                panel.line_detail = detail;
            }
        }
        bottom.mode = "P25";
        bottom.item2 = "12.5K";
    } else if (channel.mode == CODEPLUG_MODE_NXDN) {
        // NXDN48: as P25, the group or unit transmitted to (tx_target_apply()), the RAN on its line
        bool pc = ((target.id >> 24) == TX_TARGET_PC_FLAG);

        snprintf(cc_text, sizeof(cc_text), "RAN %u", (unsigned)codeplug_nxdn_ran(&channel));
        panel.line_icon = pc ? ICONS_CONTACT : ICONS_TALKGROUP;
        panel.line_right = cc_text;
        if (target.id != 0) {
            panel.line_text = target.name;
            snprintf(detail, sizeof(detail), "%s %lu", text(pc ? TEXT_PC : TEXT_TG), (unsigned long)(target.id & 0xFFFF));
            if (strcmp(detail, target.name) != 0) {
                panel.line_detail = detail;
            }
        }
        bottom.mode = "NXDN";
        bottom.item2 = "6.25K";
    } else if (channel.mode == CODEPLUG_MODE_M17) {
        // M17: to ALL (m17_radio.h), the CAN on its line
        snprintf(cc_text, sizeof(cc_text), "CAN %u", (unsigned)codeplug_m17_can(&channel));
        panel.line_icon = ICONS_TALKGROUP;
        panel.line_text = text(TEXT_ALL);
        panel.line_right = cc_text;
        bottom.mode = "M17";
        bottom.item2 = "12.5K";
    } else if (channel.mode == CODEPLUG_MODE_YSF) {
        // YSF Fusion: the destination (a contact's callsign, or ALL) on the line, the DG-ID on the right
        snprintf(cc_text, sizeof(cc_text), "DG-ID %u", (unsigned)codeplug_ysf_dgid(&channel));
        panel.line_icon = ICONS_TALKGROUP;
        panel.line_text = target.name[0] ? target.name : "ALL";
        panel.line_right = cc_text;
        bottom.mode = "YSF";
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
        // the squelch always on the line, right, after its icon, as the VFO screen shows it
        int sql = (channel.sql != 0) ? channel.sql : settings.squelch_defaults[radio_band_of(channel.rx_freq)];

        snprintf(squelch, sizeof(squelch), "%d%%", (sql - SQUELCH_MIN) * SQUELCH_PERCENT_STEP);
        panel.line_right = squelch;
        panel.line_right_squelch = true;
    }
    if (reversed) { // on the right; DMR's timeslot and colour code there: in the middle
        if (bottom.right == NULL) {
            bottom.right = text(TEXT_UI_REVERSED);
        } else { // reversed on the right as always; the timeslot and colour code at a place of their own
            bottom.item2 = bottom.right;
            bottom.item2_fixed = true;
            bottom.right = text(TEXT_UI_REVERSED);
        }
    }

    // The last heard callers, only on DMR and P25 channels and only of that protocol (a P25 entry has no timeslot).
    // The line above is the channel's TX contact, so each row carries the caller's own TG (or PC).
    if (!extra_option(EXTRA_OPTION_HIDE_RECENT_CALLERS) &&
        ((channel.mode == CODEPLUG_MODE_DMR) || (channel.mode == CODEPLUG_MODE_P25))) {
        bool p25 = (channel.mode == CODEPLUG_MODE_P25);

        for (int i = 0; (i < last_heard_count()) && (panel.recent_count < CHANNEL_PANEL_RECENT); i++) {
            if (!last_heard_get(i, &recent_entry)) {
                break;
            }
            if (p25 ? (recent_entry.timeslot != LAST_HEARD_TS_UNKNOWN) : (recent_entry.timeslot > 1)) {
                continue; // only the callers of the channel's protocol (DMR: a timeslot; not NXDN's)
            }
            last_heard_format_caller(&recent_entry, recent_name[panel.recent_count],
                                     sizeof(recent_name[panel.recent_count]));
            last_heard_format_elapsed(&recent_entry, now, recent_when[panel.recent_count],
                                      sizeof(recent_when[panel.recent_count]));
            if ((recent_entry.tg_or_pc >> 24) == LAST_HEARD_PC_FLAG) {
                snprintf(recent_detail[panel.recent_count], sizeof(recent_detail[panel.recent_count]), "%s",
                         text(TEXT_PC));
            } else {
                snprintf(recent_detail[panel.recent_count], sizeof(recent_detail[panel.recent_count]), "%s %lu",
                         text(TEXT_TG), (unsigned long)(recent_entry.tg_or_pc & CODEPLUG_CONTACT_ID_MAX));
            }
            panel.recent[panel.recent_count] = recent_name[panel.recent_count];
            panel.recent_detail[panel.recent_count] = recent_detail[panel.recent_count];
            panel.recent_when[panel.recent_count] = recent_when[panel.recent_count];
            panel.recent_count++;
        }
    }

    bottom.item1 = power_text();

    status_bar_draw(&status);
    channel_panel_draw(&panel);
    meter.compact = true; // as on the VFO screen and the caller panel
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
