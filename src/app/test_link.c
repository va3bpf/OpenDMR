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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * OpenDMR, the test interface over the programming cable (tools/debug/radio.py): keys as the keypad gives them, and
 * the state of the UI and the radio as text, so a test runs without screenshots and without the ELF's symbols.
 *
 *   'D' 'k' key(1) kind(1) modifiers(1)  a key event into the UI queue: kind 0 press, 1 long, 2 repeat, 3 release,
 *                                        4 a press and release, 5 a long press (press, long, release); key: ui.h
 *                                        UI_KEY_x (PTT too), modifiers UI_MODIFIER_x   -> 'D' 'k' ok(1)
 *   'D' 'q'                              the state, "name=value" lines             -> 'D' 'q' length(2) text
 *
 * The state: screen (and for a list: title, sel, count, row, value), home (vfo / channel), vfo, rx, tx, mode,
 * channel, name, zone, scan (off / on / paused: standing on a signal), receiving, transmitting, dbm, lockouts (temp channels, temp frequencies,
 * saved frequencies). Read from the cable task: a value may be one event old.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "list_screen.h"
#include "codeplug.h"
#include "codeplug_text.h"
#include "radio.h"
#include "settings.h"
#include "scan.h"
#include "channel_screen.h"
#include "vfo_screen.h"
#include "quick_menu.h"
#include "menus.h"
#include "test_link.h"

// The screens with a name in the state (the others: "other")
extern const ui_screen_t channel_screen, vfo_screen, dual_watch_screen, channel_details_screen, fm_radio_screen,
    gps_screen, rssi_screen, satellite_screen, last_heard_screen, number_entry_screen, pin_screen, cps_screen,
    hotspot_screen, key_settings_screen, message_box_screen, calibration_screen;

static const struct {
    const ui_screen_t *screen;
    const char *name;
} SCREENS[] = {
    {&channel_screen, "channel"},
    {&vfo_screen, "vfo"},
    {&list_screen, "list"},
    {&pick_list_screen, "pick_list"},
    {&dual_watch_screen, "dual_watch"},
    {&channel_details_screen, "channel_details"},
    {&fm_radio_screen, "fm_radio"},
    {&gps_screen, "gps"},
    {&rssi_screen, "rssi"},
    {&satellite_screen, "satellite"},
    {&last_heard_screen, "last_heard"},
    {&number_entry_screen, "number_entry"},
    {&pin_screen, "pin"},
    {&cps_screen, "cps"},
    {&hotspot_screen, "hotspot"},
    {&key_settings_screen, "key_settings"},
    {&message_box_screen, "message_box"},
    {&calibration_screen, "calibration"},
    {&firmware_info_screen, "firmware_info"},
    {&radio_info_screen, "radio_info"},
};

static const char *const SCAN_STATES[] = {"off", "on", "paused"}; // paused: standing on a signal

static bool key_event(uint8_t key, uint8_t kind, uint8_t modifiers)
{
    static const ui_event_type_t TYPES[] = {UI_EVENT_KEY_PRESS, UI_EVENT_KEY_LONG, UI_EVENT_KEY_REPEAT,
                                            UI_EVENT_KEY_RELEASE};
    ui_event_t ev = {.key = key, .modifiers = modifiers};

    if ((key >= UI_KEY_COUNT) || (kind > 5)) {
        return false;
    }
    if (kind < 4) {
        ev.type = TYPES[kind];
        ev.long_press = (kind == 3) ? false : (kind == 1);
        return ui_post(&ev);
    }
    ev.type = UI_EVENT_KEY_PRESS;
    bool ok = ui_post(&ev);
    if (kind == 5) {
        ev.type = UI_EVENT_KEY_LONG;
        ok = ui_post(&ev) && ok;
    }
    ev.type = UI_EVENT_KEY_RELEASE;
    ev.long_press = (kind == 5);
    return ui_post(&ev) && ok;
}

typedef struct {
    char *at;
    char *end;
} out_t;

static void put(out_t *o, const char *name, const char *format, ...) __attribute__((format(printf, 3, 4)));

static void put(out_t *o, const char *name, const char *format, ...)
{
    va_list args;
    int n;

    if (o->at >= o->end) {
        return;
    }
    n = snprintf(o->at, (size_t)(o->end - o->at), "%s=", name);
    o->at += (n > 0) ? n : 0;
    if (o->at >= o->end) {
        return;
    }
    va_start(args, format);
    n = vsnprintf(o->at, (size_t)(o->end - o->at), format, args);
    va_end(args);
    o->at += (n > 0) ? n : 0;
    if (o->at < o->end) {
        *o->at++ = '\n';
    }
}

static uint32_t state(char *text, uint32_t size)
{
    out_t o = {text, text + size};
    const ui_screen_t *top = ui_top_screen();
    const char *screen = "other";
    char buffer[96];

    for (size_t i = 0; i < (sizeof(SCREENS) / sizeof(SCREENS[0])); i++) {
        if (SCREENS[i].screen == top) {
            screen = SCREENS[i].name;
        }
    }
    if (quick_menu_describe(buffer, sizeof(buffer))) {
        screen = "quick_menu";
    } else if (!ui_list_describe(buffer, sizeof(buffer))) {
        buffer[0] = '\0';
    }
    put(&o, "screen", "%s", screen);
    if (buffer[0] && ((o.end - o.at) > (int)strlen(buffer))) {
        memcpy(o.at, buffer, strlen(buffer)); // its own lines (title, sel, count, row, value)
        o.at += strlen(buffer);
    }

    bool vfo = vfo_screen_is_home();
    put(&o, "home", "%s", vfo ? "vfo" : "channel");
    if (vfo) {
        const codeplug_channel_t *v = vfo_screen_vfo(vfo_screen_current());

        put(&o, "vfo", "%c", (vfo_screen_current() == 0) ? 'A' : 'B');
        put(&o, "rx", "%lu", (unsigned long)v->rx_freq);
        put(&o, "tx", "%lu", (unsigned long)v->tx_freq);
        put(&o, "mode", "%s", codeplug_mode_name(v->mode));
        put(&o, "scan", "%s", SCAN_STATES[vfo_screen_scanning()]);
    } else {
        codeplug_channel_t c;
        int number = channel_screen_channel;

        put(&o, "channel", "%d", number);
        if (codeplug_get_channel(number, &c)) {
            codeplug_text_name(c.name, CODEPLUG_NAME_LENGTH, buffer, sizeof(buffer));
            put(&o, "name", "%s", buffer);
            put(&o, "rx", "%lu", (unsigned long)c.rx_freq);
            put(&o, "tx", "%lu", (unsigned long)c.tx_freq);
            put(&o, "mode", "%s", codeplug_mode_name(c.mode));
        }
        put(&o, "zone", "%d", (int)settings.current_zone);
        put(&o, "scan", "%s", SCAN_STATES[channel_screen_scanning()]);
    }
    put(&o, "receiving", "%d", radio_is_receiving() ? 1 : 0);
    put(&o, "transmitting", "%d", radio_is_transmitting() ? 1 : 0);
    put(&o, "radio", "%s", radio_state_name());
    put(&o, "dbm", "%d", radio_signal_dbm());
    put(&o, "signal", "%u", (unsigned)radio_signal_raw()); // FD6818 0x67 bits 8:0 halved, the last reading
    put(&o, "noise", "%u", (unsigned)radio_noise());       // FD6818 0x65, the last reading
    put(&o, "lockouts", "%u,%u,%d", (unsigned)scan_skipped_channels.count, (unsigned)scan_skipped_frequencies.count,
        scan_saved_count());
    return (uint32_t)(o.at - text);
}

uint32_t test_link_command(volatile uint8_t *request, volatile uint8_t *reply, uint32_t reply_size)
{
    switch (request[1]) {
        case 'k':
            reply[2] = key_event(request[2], request[3], request[4]) ? 1 : 0;
            return 3;
        case 'q': {
            uint32_t n = state((char *)&reply[4], reply_size - 4);

            reply[2] = (uint8_t)(n >> 8);
            reply[3] = (uint8_t)n;
            return 4 + n;
        }
        default:
            return 0; // not one of these
    }
}
