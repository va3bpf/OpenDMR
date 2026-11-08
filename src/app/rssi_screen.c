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
 * OpenDMR, screen: the signal strength (menuRSSIScreen.c, DM-32 drawing; docs/manual rssi.png): status bar,
 * the dBm value large, the S meter of the receive screens, the frequency and the mode; * shows the raw signal and
 * noise values too. Left or right key: back.
 *
 * The values come from the radio service (radio.h), read every RSSI_SCREEN_REFRESH_MS (as the main firmware); the
 * screen is drawn again only when they changed.
 */

#include <stdio.h>
#include <string.h>
#include "util.h"
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "battery.h"
#include "datetime.h"
#include "codeplug.h"
#include "list_screen.h"
#include "radio.h"
#include "rssi_screen.h"

#define TITLE_Y 44
#define VALUE_Y 70
#define METER_Y 128
#define LINE_Y  206
#define FREQ_Y  216
#define RAW_Y   246

static rssi_signal_t shown;
static bool raw_values = false;

static void read_signal(rssi_signal_t *out)
{
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->dbm = (int16_t)radio_signal_dbm();
    out->raw_signal = radio_signal_raw();
    out->raw_noise = radio_noise();
    out->shown = radio_signal();
    out->open = radio_is_receiving();
    out->rx_freq = radio_rx_frequency();
    out->digital = radio_channel_is_dmr();
}

static void enter(void *arg)
{
    (void)arg;
    read_signal(&shown);
    ui_timer_start(RSSI_SCREEN_REFRESH_MS);
}

static bool handle(const ui_event_t *ev)
{
    if (ev->type == UI_EVENT_TIMER) {
        rssi_signal_t now;
        bool changed;

        read_signal(&now);
        changed = (memcmp(&now, &shown, sizeof(now)) != 0);
        shown = now;
        ui_timer_start(RSSI_SCREEN_REFRESH_MS);
        return changed;
    }
    if (ev->type == UI_EVENT_DATA) {
        return true; // battery, time
    }
    if (ev->type == UI_EVENT_KEY_PRESS) {
        if (ev->key == UI_KEY_GREEN) {
            ui_pop();
            return false;
        }
        if (ev->key == UI_KEY_STAR) {
            raw_values = !raw_values;
            return true;
        }
    }
    list_handle_back(ev);
    return false;
}

static void draw(void)
{
    status_bar_data_t bar = {0};
    s_meter_data_t meter = {0};
    char buffer[40];
    int32_t x;
    const font_t *big = &font_roboto44_bold;

    meter.active = true;
    if (shown.valid) {
        s_meter_set_signal(&meter, shown.shown);
    }
    meter.open = shown.valid && shown.open;

    bar.state = (meter.open ? STATUS_BAR_RECEIVING : STATUS_BAR_IDLE);
    bar.signal_bars = (int8_t)((meter.s_units * ICONS_SIGNAL_BARS) / 9);
    status_bar_draw(&bar);

    x = WIDGET_MARGIN + icons_draw(WIDGET_MARGIN, TITLE_Y + 1, ICONS_SIGNAL, THEME_COLOUR_TEXT_LABEL) + 8;
    draw_text(x, TITLE_Y, text(TEXT_RSSI), &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);

    // dBm, the unit on the same baseline
    if (shown.valid) {
        snprintf(buffer, sizeof(buffer), "%d", shown.dbm);
    } else {
        snprintf(buffer, sizeof(buffer), "--");
    }
    x = font_draw_text(WIDGET_MARGIN, VALUE_Y, buffer, big, FONT_ALIGN_LEFT, theme_colour(THEME_COLOUR_TEXT)) + 8;
    draw_text(x, VALUE_Y + big->ascent - font_roboto20.ascent, "dBm", &font_roboto20, FONT_ALIGN_LEFT,
              THEME_COLOUR_TEXT_LABEL);

    s_meter_draw(METER_Y, &meter);
    draw_hline(WIDGET_MARGIN, LINE_Y, FB_WIDTH - (2 * WIDGET_MARGIN), THEME_COLOUR_LINE);

    if (shown.rx_freq != 0) {
        snprintf(buffer, sizeof(buffer), "%lu.%04lu MHz   %s", (unsigned long)(shown.rx_freq / 100000),
                 (unsigned long)((shown.rx_freq % 100000) / 10), (shown.digital ? "DMR" : "FM"));
        draw_text(WIDGET_MARGIN, FREQ_Y, buffer, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_SECOND);
    }

    if (raw_values) {
        snprintf(buffer, sizeof(buffer), "S/N  %u / %u", shown.raw_signal, shown.raw_noise);
        draw_text(WIDGET_MARGIN, RAW_Y, buffer, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    }

    bottom_bar_draw_soft_keys(NULL, text(TEXT_UI_BACK));
}

const ui_screen_t rssi_screen = {
    .enter = enter,
    .handle = handle,
    .draw = draw,
};
