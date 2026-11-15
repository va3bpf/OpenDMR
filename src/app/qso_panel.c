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
 * OpenDMR, the DMR caller panel (uiUtilities.c renderQSODataDM32(), DM-32 drawing; docs/manual dmr-rx): receiving and
 * the ID, the callsign; the name, the city and state (an option), the country (the database of tools/ids/id_db.py;
 * the country from the ID's first digits when it is not in it), the position when the call carries an ETSI GPS Info LC
 * (the distance and direction from ours, or its coordinates when ours is not known: position.c), as many as are known,
 * as large as they fit; the talk group over the S meter.
 */

#include <stdio.h>
#include <string.h>
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "icons.h"
#include "text.h"
#include "radio.h"
#include "codeplug.h"
#include "last_heard.h"
#include "dmr_ids.h"
#include "position.h"
#include "extra_options.h"
#include "qso_panel.h"

// Fixed lines, whatever is known (the position comes a second or two into a call: nothing moves when it does)
#define TOP_Y        44  // the channel or VFO name as the VFO screen's "VFO A" (vfo_screen.c TOP_Y), the ID on the right
#define CALLSIGN_Y   63
#define SEPARATOR_Y  216
#define TG_Y         221
#define S_METER_Y    250 // the compact one, as on the channel and VFO screens
#define ALL_CALL     0xFFFFFF

enum { DETAIL_NAME = 0, DETAIL_PLACE, DETAIL_COUNTRY, DETAIL_POSITION, DETAILS };

static const int32_t DETAIL_Y[DETAILS] = {115, 142, 166, 190};

static const font_t *const DETAIL_FONTS[DETAILS] = {&font_roboto22, &font_roboto20, &font_roboto20, &font_roboto20};

static const theme_colour_t DETAIL_COLOURS[DETAILS] = {THEME_COLOUR_TEXT, THEME_COLOUR_TEXT_LABEL,
                                                       THEME_COLOUR_TEXT_LABEL, THEME_COLOUR_TEXT_SECOND};

static uint32_t info_id = 0; // the caller of info (the database is read once a caller)
static bool info_found = false;
static dmr_ids_info_t info;
static char country[64];

// The caller's text of the last heard list ("callsign name ..." of the contacts or the DMR IDs, else the talker
// alias; the alias first with the TA priorities of the settings), "" when not known
static void caller_text(uint32_t id, char *out, size_t size)
{
    last_heard_entry_t entry;

    out[0] = '\0';
    // Search all last heard entries for this ID (not just position 0) because the
    // talker alias arrives in a separate LC block and may not be at position 0 yet
    for (int i = 0; i < last_heard_count(); i++) {
        if (last_heard_get(i, &entry) && (entry.id == id)) {
            bool alias = (entry.talker_alias[0] != 0) && (dmr_ids_talker_alias_first() || (entry.text[0] == 0));

            snprintf(out, size, "%s", alias ? entry.talker_alias : entry.text);
            return;
        }
    }
}

// "callsign rest": the callsign alone, then the rest (uiUtilitySplitCallsign())
static void split_callsign(const char *text, char *callsign, size_t callsign_size, char *rest, size_t rest_size)
{
    const char *space = strchr(text, ' ');
    int length = (int)(space ? (size_t)(space - text) : strlen(text));

    snprintf(callsign, callsign_size, "%.*s", length, text);
    rest[0] = '\0';
    if (space) {
        while (*space == ' ') {
            space++;
        }
        snprintf(rest, rest_size, "%s", space);
    }
}

// The text cut to the width, "..." at its end then (whole UTF-8 characters off)
static void draw_fitted(int32_t x, int32_t y, const char *text, const font_t *font, int32_t width,
                        theme_colour_t colour)
{
    char shown[96];
    size_t length;

    snprintf(shown, sizeof(shown), "%s", text);
    if (font_text_width(font, shown) > width) {
        length = strlen(shown);
        while (length > 0) {
            do {
                length--;
            } while ((length > 0) && ((shown[length] & 0xC0) == 0x80));
            while ((length > 0) && (shown[length - 1] == ' ')) {
                length--;
            }
            snprintf(&shown[length], sizeof(shown) - length, "...");
            if (font_text_width(font, shown) <= width) {
                break;
            }
        }
    }
    draw_text(x, y, shown, font, FONT_ALIGN_LEFT, colour);
}

// The caller's place from the database (ours: tools/ids/id_db.py), the country from the ID when it is not in it
static void look_up(uint32_t id)
{
    if (id != info_id) {
        info_id = id;
        info_found = dmr_ids_info(id, &info);
        if (info_found && info.country[0]) {
            snprintf(country, sizeof(country), "%s", info.country);
        } else {
            (void)dmr_ids_country(id, country, sizeof(country));
        }
    }
}

static void draw(const dmr_call_t *call, const char *mode, const char *status_item, const char *status_item2,
                 const char *channel_name, const char *power)
{
    const int32_t x = WIDGET_MARGIN;
    const int32_t width = FB_WIDTH - (2 * WIDGET_MARGIN);
    bool private_call = ((call->tg_or_pc >> 24) == LAST_HEARD_PC_FLAG);
    uint32_t called = call->tg_or_pc & 0xFFFFFF;
    const font_t *call_font = &font_roboto44_bold;
    status_bar_data_t status = {.state = STATUS_BAR_RECEIVING};
    s_meter_data_t meter = {.active = true, .open = true, .compact = true};
    bottom_bar_data_t bottom = {.mode = mode, .item1 = power, .item2 = "12.5K"}; // as the screen's
    char caller[LAST_HEARD_TEXT_SIZE];
    char callsign[LAST_HEARD_TEXT_SIZE];
    char rest[LAST_HEARD_TEXT_SIZE];
    char id[20];
    char location[32];
    char place[96];
    char line[40];
    const char *details[DETAILS];
    int32_t lx;

    s_meter_set_signal(&meter, radio_signal());
    status.signal_bars = (int8_t)((meter.s_units * ICONS_SIGNAL_BARS) / 9);
    status.item1 = status_item;
    status.item2 = status_item2;
    status.item1_bold = true;
    status_bar_draw(&status);

    caller_text(call->source, caller, sizeof(caller));
    if (caller[0] == '\0') {
        snprintf(callsign, sizeof(callsign), "%lu", (unsigned long)call->source);
        rest[0] = '\0';
        id[0] = '\0'; // the number is already the large line
    } else {
        split_callsign(caller, callsign, sizeof(callsign), rest, sizeof(rest));
        snprintf(id, sizeof(id), "%lu", (unsigned long)call->source);
    }

    if (private_call) {
        if (called == codeplug_get_dmr_id()) {
            snprintf(line, sizeof(line), "%s %lu", text(TEXT_UI_ME), (unsigned long)called);
        } else {
            snprintf(line, sizeof(line), "%s %lu", text(TEXT_UI_PC_TO), (unsigned long)called);
        }
    } else {
        if (called == ALL_CALL) {
            snprintf(line, sizeof(line), "%s", text(TEXT_ALL_CALL));
        } else {
            snprintf(line, sizeof(line), "%s %lu", text(TEXT_TG), (unsigned long)called);
        }
    }

    // The channel (or VFO) on the left, the caller's ID on the right
    lx = 0;
    if (id[0]) {
        draw_text(x + width, TOP_Y, id, &font_roboto17, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT_LABEL);
        lx = font_text_width(&font_roboto17, id) + 12;
    }
    if (channel_name && channel_name[0]) {
        draw_fitted(x, TOP_Y, channel_name, &font_roboto17_bold, width - lx, THEME_COLOUR_TEXT);
    }

    if (font_text_width(call_font, callsign) > width) {
        call_font = &font_roboto32_bold;
    }
    draw_text(x, CALLSIGN_Y + (font_roboto44_bold.ascent - call_font->ascent), callsign, call_font, FONT_ALIGN_LEFT,
              THEME_COLOUR_TEXT);

    // The caller's lines known
    look_up(call->source);
    details[DETAIL_NAME] = rest;
    place[0] = '\0';
    if (info_found && !extra_option(EXTRA_OPTION_HIDE_CALLER_PLACE) && (info.city[0] || info.state[0])) {
        snprintf(place, sizeof(place), "%.47s%s%.45s", info.city, ((info.city[0] && info.state[0]) ? ", " : ""),
                 info.state);
    }
    details[DETAIL_PLACE] = place;
    details[DETAIL_COUNTRY] = country;
    location[0] = '\0';
    if (call->has_location) { // the caller's position (ETSI GPS Info LC): distance and direction, or coordinates
        position_format(location, sizeof(location), call->latitude_e5, call->longitude_e5);
    }
    details[DETAIL_POSITION] = location;

    for (int i = 0; i < DETAILS; i++) {
        const font_t *font = DETAIL_FONTS[i];
        int32_t tx = x;

        if (i == DETAIL_POSITION) { // its pin always, lit when the call carries the position
            tx += icons_draw(x, DETAIL_Y[i] + ((font->line_height - ICONS_SIZE) / 2), ICONS_LOCATION,
                             (details[i][0] ? THEME_COLOUR_RX : THEME_COLOUR_OFF)) +
                  6;
            if (details[i][0] && (font_text_width(font, location) > (x + width - tx))) {
                position_format_short(location, sizeof(location), call->latitude_e5, call->longitude_e5);
            }
        }
        if (details[i][0]) {
            draw_fitted(tx, DETAIL_Y[i], details[i], font, x + width - tx, DETAIL_COLOURS[i]);
        }
    }

    draw_hline(x, SEPARATOR_Y, width, THEME_COLOUR_LINE);
    lx = x + icons_draw(x, TG_Y + 5, (private_call ? ICONS_CONTACT : ICONS_TALKGROUP), THEME_COLOUR_TEXT_LABEL) + 8;
    draw_fitted(lx, TG_Y, line, &font_roboto20, x + width - lx, THEME_COLOUR_TEXT);

    s_meter_draw(S_METER_Y, &meter);
    bottom_bar_draw(&bottom);
}

void qso_panel_draw(const dmr_call_t *call, const char *channel_name, uint8_t colour_code, const char *power)
{
    char ts_text[8];
    char cc_text[8];

    snprintf(ts_text, sizeof(ts_text), "TS%d", call->timeslot + 1);
    snprintf(cc_text, sizeof(cc_text), "CC%u", (unsigned)colour_code);
    draw(call, "DMR", ts_text, cc_text, channel_name, power);
}

void qso_panel_draw_p25(const p25_call_t *call, const char *channel_name, const char *power)
{
    dmr_call_t shown = {.source = call->src, .tg_or_pc = p25_call_tg_or_pc(call)};
    char nac_text[8];

    snprintf(nac_text, sizeof(nac_text), "NAC%03X", call->nac);
    draw(&shown, "P25", nac_text, NULL, channel_name, power);
}
