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
 * OpenDMR, screen: the DMR hotspot (uiHotspot.c), over every other screen while the hotspot runs
 * (services/hotspot.c). Shown when a host talks MMDVM on the cable (hotspot_screen_hook()), it starts the hotspot;
 * the hotspot ending (MMDVMHost silent for 20 s) takes it away, the home screen's channel goes back to the radio.
 * While it is shown the hotspot tunes the radio (hotspot.c), the UI leaves it alone (UI_CHANNEL_KEPT). Keys do
 * nothing, but the right key with BlueDV, which ends it (MMDVMHost would not recover from a hotspot that went away).
 *
 *   status bar: DMR, the colour code, receiving / transmitting
 *   "Hotspot", then the call (the caller's ID and the TG / PC) received or transmitted, else the host's IP address
 *   the frequency (R when receiving, T when transmitting) and the power
 */

#include <stdio.h>
#include <string.h>
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "text.h"
#include "hotspot.h"
#include "hotspot_screen.h"

#define TITLE_Y   60
#define CALL_Y    120
#define TARGET_Y  160
#define FREQ_Y    220
#define POWER_Y   250

static bool shown = false;

bool hotspot_screen_hook(const ui_event_t *ev)
{
    if ((ev->type == UI_EVENT_DATA) && (ev->what & UI_DATA_HOTSPOT) && hotspot_wanted() && !shown) {
        ui_push(&hotspot_screen, (void *)1);
    }
    return false;
}

static void enter(void *arg)
{
    if (arg) {
        shown = true;
        hotspot_start();
    }
}

static void leave(void)
{
    shown = false;
}

static bool handle(const ui_event_t *ev)
{
    hotspot_status_t s;

    if (ev->type == UI_EVENT_DATA) {
        if ((ev->what & UI_DATA_HOTSPOT) && !hotspot_active()) {
            ui_pop(); // ended
            return false;
        }
        return true;
    }
    hotspot_get_status(&s);
    if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_RED) && s.bluedv) {
        hotspot_stop();
    }
    return false;
}

// The channel of the screen (ui_screen_t.channel): the hotspot's, set by the hotspot task at its moments
static const codeplug_channel_t *radio_channel(void)
{
    return UI_CHANNEL_KEPT;
}

static void format_frequency(char *buffer, size_t size, char prefix, uint32_t frequency)
{
    snprintf(buffer, size, "%c %lu.%05lu MHz", prefix, (unsigned long)(frequency / 100000),
             (unsigned long)(frequency % 100000));
}

static void draw(void)
{
    char buffer[40];
    char cc[8];
    hotspot_status_t s;
    status_bar_data_t bar = {0};
    bool tx;

    hotspot_get_status(&s);
    tx = (s.show == HOTSPOT_SHOW_TX) || (s.show == HOTSPOT_SHOW_CW);
    snprintf(cc, sizeof(cc), "CC%u", s.colour_code);
    bar.state = tx ? STATUS_BAR_TRANSMITTING : ((s.show == HOTSPOT_SHOW_RX) ? STATUS_BAR_RECEIVING : STATUS_BAR_IDLE);
    bar.item1 = "DMR";
    bar.item1_bold = true;
    bar.item2 = cc;
    status_bar_draw(&bar);

    draw_text(FB_WIDTH / 2, TITLE_Y, text(TEXT_HOTSPOT_MODE), &font_roboto32_bold, FONT_ALIGN_CENTER,
              THEME_COLOUR_TEXT);

    switch (s.show) {
        case HOTSPOT_SHOW_WAITING:
            draw_text(FB_WIDTH / 2, CALL_Y, text(TEXT_UI_HOTSPOT_WAITING), &font_roboto20, FONT_ALIGN_CENTER,
                      THEME_COLOUR_TEXT_SECOND);
            break;
        case HOTSPOT_SHOW_CW:
            draw_text(FB_WIDTH / 2, CALL_Y, "<CW ID>", &font_roboto22, FONT_ALIGN_CENTER, THEME_COLOUR_TX);
            break;
        case HOTSPOT_SHOW_RX:
        case HOTSPOT_SHOW_TX:
            snprintf(buffer, sizeof(buffer), "ID %lu", (unsigned long)s.source);
            draw_text(FB_WIDTH / 2, CALL_Y, buffer, &font_roboto22, FONT_ALIGN_CENTER,
                      tx ? THEME_COLOUR_TX : THEME_COLOUR_RX);
            if (s.destination != 0) {
                snprintf(buffer, sizeof(buffer), "%s %lu", text(s.private_call ? TEXT_PC : TEXT_TG),
                         (unsigned long)s.destination);
                draw_text(FB_WIDTH / 2, TARGET_Y, buffer, &font_roboto20, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_SECOND);
            }
            break;
        default:
            if (s.pocsag) {
                draw_text(FB_WIDTH / 2, CALL_Y, "<POCSAG>", &font_roboto20, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_SECOND);
            } else if (s.ip[0]) {
                draw_text(FB_WIDTH / 2, CALL_Y, s.ip, &font_roboto20, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_SECOND);
            }
            break;
    }

    if (s.show != HOTSPOT_SHOW_WAITING) {
        format_frequency(buffer, sizeof(buffer), tx ? 'T' : 'R', tx ? s.tx_freq : s.rx_freq);
        draw_text(FB_WIDTH / 2, FREQ_Y, buffer, &font_roboto20, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT);
        if (s.power_level >= 0) {
            draw_text(FB_WIDTH / 2, POWER_Y, bottom_bar_power_label(s.power_level), &font_roboto17, FONT_ALIGN_CENTER,
                      THEME_COLOUR_TEXT_LABEL);
        }
    }
}

const ui_screen_t hotspot_screen = {
    .enter = enter,
    .leave = leave,
    .handle = handle,
    .draw = draw,
    .channel = radio_channel,
};
