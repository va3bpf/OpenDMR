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
 * OpenDMR, the DMR caller panel (uiUtilities.c renderQSODataDM32(), DM-32 drawing; docs/manual dmr-rx). The caller's
 * position, when its call carries an ETSI GPS Info LC, goes on the ID line: the distance and direction from ours, or
 * its coordinates when ours is not known (position.c).
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
#include "qso_panel.h"

#define LINE1_Y      44
#define CALLSIGN_Y   66
#define NAME_Y       120
#define ID_Y         146
#define SEPARATOR_Y  172
#define TG_Y         180
#define S_METER_Y    212
#define ALL_CALL     0xFFFFFF

// The caller's text of the last heard list ("callsign name ..." of the contacts or the DMR IDs, else the talker
// alias; the alias first with the TA priorities of the settings), "" when not known
static void caller_text(uint32_t id, char *out, size_t size)
{
    last_heard_entry_t entry;

    out[0] = '\0';
    if (last_heard_get(0, &entry) && (entry.id == id)) {
        bool alias = (entry.talker_alias[0] != 0) && (dmr_ids_talker_alias_first() || (entry.text[0] == 0));

        snprintf(out, size, "%s", alias ? entry.talker_alias : entry.text);
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

void qso_panel_draw(const dmr_call_t *call, const char *channel_name)
{
    const int32_t x = WIDGET_MARGIN;
    const int32_t width = FB_WIDTH - (2 * WIDGET_MARGIN);
    bool private_call = ((call->tg_or_pc >> 24) == LAST_HEARD_PC_FLAG);
    uint32_t called = call->tg_or_pc & 0xFFFFFF;
    const font_t *call_font = &font_roboto44_bold;
    status_bar_data_t status = {.state = STATUS_BAR_RECEIVING};
    s_meter_data_t meter = {.active = true, .open = true};
    bottom_bar_data_t bottom = {.mode = "DMR", .item1 = channel_name};
    char caller[LAST_HEARD_TEXT_SIZE];
    char callsign[LAST_HEARD_TEXT_SIZE];
    char rest[LAST_HEARD_TEXT_SIZE];
    char id[20];
    char location[32];
    char line1[40];
    char line[40];
    char ts_text[8];
    int32_t lx;

    s_meter_set_signal(&meter, radio_signal());
    status.signal_bars = (int8_t)((meter.s_units * ICONS_SIGNAL_BARS) / 9);
    snprintf(ts_text, sizeof(ts_text), "TS%d", call->timeslot + 1);
    status.item1 = ts_text;
    status.item1_bold = true;
    status_bar_draw(&status);

    caller_text(call->source, caller, sizeof(caller));
    if (caller[0] == '\0') {
        snprintf(callsign, sizeof(callsign), "%lu", (unsigned long)call->source);
        rest[0] = '\0';
        id[0] = '\0';
    } else {
        split_callsign(caller, callsign, sizeof(callsign), rest, sizeof(rest));
        snprintf(id, sizeof(id), "ID %lu", (unsigned long)call->source);
    }

    if (private_call) {
        snprintf(line1, sizeof(line1), "%s", text(TEXT_PRIVATE_CALL));
        if (called == codeplug_get_dmr_id()) {
            snprintf(line, sizeof(line), "%s %lu", text(TEXT_UI_ME), (unsigned long)called);
        } else {
            snprintf(line, sizeof(line), "%s %lu", text(TEXT_UI_PC_TO), (unsigned long)called);
        }
    } else {
        snprintf(line1, sizeof(line1), "%s", text(TEXT_UI_DW_RECEIVING));
        if (called == ALL_CALL) {
            snprintf(line, sizeof(line), "%s", text(TEXT_ALL_CALL));
        } else {
            snprintf(line, sizeof(line), "%s %lu", text(TEXT_TG), (unsigned long)called);
        }
    }

    icons_draw(x, LINE1_Y + 1, ICONS_SOUND, THEME_COLOUR_RX);
    draw_text(x + 26, LINE1_Y, line1, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_RX);

    if (font_text_width(call_font, callsign) > width) {
        call_font = &font_roboto32_bold;
    }
    draw_text(x, CALLSIGN_Y + (font_roboto44_bold.ascent - call_font->ascent), callsign, call_font, FONT_ALIGN_LEFT,
              THEME_COLOUR_TEXT);
    if (rest[0]) {
        draw_text(x, NAME_Y, rest, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_SECOND);
    }
    if (id[0]) {
        draw_text(x, ID_Y, id, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    }
    if (call->has_location) {
        int32_t room = width - (id[0] ? (font_text_width(&font_roboto17, id) + 6) : 0);

        position_format(location, sizeof(location), call->latitude_e5, call->longitude_e5);
        if (font_text_width(&font_roboto17, location) > room) {
            position_format_short(location, sizeof(location), call->latitude_e5, call->longitude_e5);
        }
        if ((room > 0) && (font_text_width(&font_roboto17, location) <= room)) {
            draw_text(x + width, ID_Y, location, &font_roboto17, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT_LABEL);
        }
    }

    draw_hline(x, SEPARATOR_Y, width, THEME_COLOUR_LINE);
    lx = x + icons_draw(x, TG_Y + 5, (private_call ? ICONS_CONTACT : ICONS_TALKGROUP), THEME_COLOUR_TEXT_LABEL) + 8;
    draw_text(lx, TG_Y, line, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);

    s_meter_draw(S_METER_Y, &meter);
    bottom_bar_draw(&bottom);
}
