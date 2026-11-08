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
 * OpenDMR, screen: the FM broadcast radio (uiFMRadio.c, docs/manual fm-radio.png): the frequency large,
 * the RDS station name, the 87 - 108 MHz dial, the RDS radio text, stereo in the status bar.
 *
 *   rotary, LEFT / RIGHT: 100 kHz steps; # / *: seek up / down; digits: a frequency (999 is 99.9 MHz, 1071 107.1 MHz);
 *   left key: squelch on / off (muted when the receiver finds no station); right key, SK1 or PTT: back (the receiver
 *   off). The frequency and the squelch are saved (custom data block 0x44540003; the main firmware kept them until
 *   power off only) when the screen is left and at power off. So is "playing": switched off on this screen, the
 *   radio starts on it again (fm_radio_was_playing()).
 *
 * The receiver is services/fm_broadcast.c (drivers/rda5807.c). Its status and RDS are read on the
 * screen's timer: the RDS group comes every 88 ms (read every 20 ms, none lost), the status is enough every 300 ms.
 */

#include <stdio.h>
#include <string.h>
#include "util.h"
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "battery.h"
#include "datetime.h"
#include "fm_broadcast.h"
#include "custom_data.h"
#include "power.h"
#include "list_screen.h"
#include "fm_radio.h"
#include "sound.h"

#define POLL_MS          20 // RDS: a group every 88 ms, the chip keeps the last one only
#define STATUS_EVERY     15 // polls: the status every 300 ms
#define FREQ_Y           52  // frequency with MHz, centred
#define NAME_Y           112 // RDS station name, or "seeking"
#define DIAL_Y           150 // dial: line, ticks, labels below
#define TEXT_Y           196 // RDS radio text
#define TEXT_LINES       3
#define HINT_Y           260
#define STEREO_X         84 // stereo icon in the status bar, after "FM"
#define DIGITS_MAX       4
#define RSSI_BAR_OFFSET  16 // dBuV: status bar bars from the receiver's RSSI
#define RSSI_BAR_STEP    6
#define DIAL_MAJOR_EVERY 4 // MHz between labelled ticks

#define SAVED_DATA_TYPE 0x44540003 // a custom data block of this firmware ("DT", 3)
#define SAVED_VERSION   1

static uint16_t frequency = 999; // 100 kHz units
static bool squelch = false;

// What is saved, and as last read or written
static struct {
    uint16_t version;
    uint16_t frequency;
    uint8_t squelch;
    uint8_t playing; // this screen was on: switched off here
    uint8_t unused[2];
} saved;
static bool saved_read = false;
static bool playing = false;

static void load(void)
{
    if (saved_read) {
        return;
    }
    saved_read = true;
    if (custom_data_read(SAVED_DATA_TYPE, (uint8_t *)&saved, sizeof(saved)) && (saved.version == SAVED_VERSION) &&
        (saved.frequency >= FM_BROADCAST_FREQ_MIN) && (saved.frequency <= FM_BROADCAST_FREQ_MAX)) {
        frequency = saved.frequency;
        squelch = (saved.squelch != 0);
    }
}

static bool unsaved(void)
{
    return (saved.version != SAVED_VERSION) || (saved.frequency != frequency) || ((saved.squelch != 0) != squelch) ||
           ((saved.playing != 0) != playing);
}

static void save(void)
{
    if (!unsaved()) {
        return;
    }
    saved.version = SAVED_VERSION;
    saved.frequency = frequency;
    saved.squelch = squelch;
    saved.playing = playing;
    custom_data_write(SAVED_DATA_TYPE, (const uint8_t *)&saved, sizeof(saved));
}

bool fm_radio_was_playing(void)
{
    load();
    return (saved.version == SAVED_VERSION) && (saved.playing != 0);
}
static char digits[DIGITS_MAX + 1];
static int digit_count = 0;
static int polls = 0;
static fm_broadcast_status_t status;

// Reads the receiver; true when something shown changed
static bool update(void)
{
    fm_broadcast_status_t now;
    bool changed;

    fm_broadcast_get_status(&now);
    changed = (memcmp(&now, &status, sizeof(now)) != 0);
    status = now;
    frequency = status.frequency;
    fm_broadcast_mute(squelch && (status.seeking || !status.station));
    return changed;
}

static void tune(uint16_t to)
{
    digit_count = 0;
    fm_broadcast_tune(to);
}

// The digits typed so far as a frequency: 3 digits (87.0 - 99.9 MHz) or 4 starting with 1 (100.0 - 108.0 MHz);
// 0 while not complete
static uint16_t entered_frequency(void)
{
    int value = 0;

    if (digit_count < ((digits[0] == '1') ? 4 : 3)) {
        return 0;
    }
    for (int i = 0; i < digit_count; i++) {
        value = (value * 10) + (digits[i] - '0');
    }
    return (uint16_t)value;
}

static void enter_digit(int digit)
{
    uint16_t entered;

    if ((digit_count == 0) && (digit != 8) && (digit != 9) && (digit != 1)) {
        ui_key_beep(SOUND_ERROR); // a frequency starts with 8, 9 or 1
        return;
    }
    digits[digit_count++] = (char)('0' + digit);
    digits[digit_count] = '\0';
    entered = entered_frequency();
    if (entered != 0) {
        digit_count = 0;
        if ((entered >= FM_BROADCAST_FREQ_MIN) && (entered <= FM_BROADCAST_FREQ_MAX)) {
            tune(entered);
        } else {
            ui_key_beep(SOUND_ERROR);
        }
    }
}

// Pushed (its state 0) or back on top (the receiver was stopped when it was covered)
static void enter(void *arg)
{
    int32_t *started = ui_state();

    (void)arg;
    if (*started == 0) {
        *started = 1;
        digit_count = 0;
    }
    polls = 0;
    load();
    if (!fm_broadcast_is_on() && !fm_broadcast_start(frequency)) {
        sound_play(SOUND_ERROR); // no receiver
        ui_pop();
        return;
    }
    playing = true;
    save();
    power_off_save_register(save); // the last frequency, and "playing", kept when switched off here
    update();
    ui_timer_start(POLL_MS);
}

// Popped, home, or covered: the receiver off, saved as not playing
static void leave(void)
{
    fm_broadcast_stop();
    playing = false;
    save();
}

static bool handle(const ui_event_t *ev)
{
    bool press = (ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT);

    switch (ev->type) {
        case UI_EVENT_TIMER: {
            bool changed = fm_broadcast_poll_rds();

            if (++polls >= STATUS_EVERY) {
                polls = 0;
                changed |= update();
            }
            ui_timer_start(POLL_MS);
            return changed;
        }
        case UI_EVENT_DATA:
            return true; // battery, time
        case UI_EVENT_ROTARY:
            tune(frequency + ev->steps);
            break;
        case UI_EVENT_KEY_LONG:
            if (ev->key == UI_KEY_RED) {
                ui_home();
            }
            return false;
        case UI_EVENT_KEY_RELEASE:
            if ((ev->key == UI_KEY_SK1) && !ev->long_press) {
                ui_pop();
            } else if ((ev->key == UI_KEY_RED) && !ev->long_press) {
                if (digit_count > 0) {
                    digit_count = 0;
                    return true;
                }
                ui_pop();
            }
            return false;
        default:
            if (!press) {
                return false;
            }
            if (ev->key == UI_KEY_PTT) {
                ui_pop();
                return false;
            } else if ((ev->key == UI_KEY_GREEN) && (ev->type == UI_EVENT_KEY_PRESS)) {
                squelch = !squelch;
            } else if ((ev->key == UI_KEY_LEFT) || (ev->key == UI_KEY_RIGHT)) {
                tune(frequency + ((ev->key == UI_KEY_RIGHT) ? 1 : -1));
            } else if (((ev->key == UI_KEY_HASH) || (ev->key == UI_KEY_STAR)) && (ev->type == UI_EVENT_KEY_PRESS)) {
                digit_count = 0;
                fm_broadcast_seek(ev->key == UI_KEY_HASH);
            } else if ((ui_key_digit(ev->key) >= 0) && (ev->type == UI_EVENT_KEY_PRESS)) {
                enter_digit(ui_key_digit(ev->key));
            } else {
                return false;
            }
            break;
    }
    update();
    return true;
}

// Text on up to TEXT_LINES lines of the screen width, broken at spaces when it can
static void draw_wrapped(int32_t y, const char *text_in, const font_t *font, theme_colour_t colour)
{
    const int32_t width = FB_WIDTH - (2 * WIDGET_MARGIN);
    char line[FM_BROADCAST_RDS_TEXT + 1];

    for (int l = 0; (l < TEXT_LINES) && (*text_in != '\0'); l++) {
        int length = 0;
        int last_space = -1;

        while ((text_in[length] != '\0') && (length < FM_BROADCAST_RDS_TEXT)) {
            memcpy(line, text_in, length + 1);
            line[length + 1] = '\0';
            if (font_text_width(font, line) > width) {
                break;
            }
            if (text_in[length] == ' ') {
                last_space = length;
            }
            length++;
        }
        if ((text_in[length] != '\0') && (last_space > 0)) {
            length = last_space + 1;
        }
        memcpy(line, text_in, length);
        line[length] = '\0';
        draw_text(WIDGET_MARGIN, y + (l * font->line_height), line, font, FONT_ALIGN_LEFT, colour);
        text_in += length;
        while (*text_in == ' ') {
            text_in++;
        }
    }
}

// A tick each MHz, longer each 4 MHz with its label, the needle at the frequency
static void draw_dial(uint16_t at)
{
    const int32_t x0 = WIDGET_MARGIN + 4;
    const int32_t width = FB_WIDTH - (2 * WIDGET_MARGIN) - 8;
    const int32_t span = FM_BROADCAST_FREQ_MAX - FM_BROADCAST_FREQ_MIN;

    draw_hline(x0, DIAL_Y + 10, width, THEME_COLOUR_LINE);
    for (int32_t mhz = 88; mhz <= 108; mhz++) {
        int32_t x = x0 + ((((mhz * 10) - FM_BROADCAST_FREQ_MIN) * width) / span);
        bool major = (((mhz - 88) % DIAL_MAJOR_EVERY) == 0);

        draw_fill_rect(x, DIAL_Y + (major ? 2 : 6), 1, (major ? 8 : 4),
                       (major ? THEME_COLOUR_TEXT_LABEL : THEME_COLOUR_LINE));
        if (major) {
            char label[4];

            snprintf(label, sizeof(label), "%d", (int)mhz);
            draw_text(x, DIAL_Y + 14, label, &font_roboto12, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_DIM);
        }
    }
    draw_fill_rect(x0 + ((((int32_t)at - FM_BROADCAST_FREQ_MIN) * width) / span) - 1, DIAL_Y - 4, 3, 18,
                   THEME_COLOUR_ACCENT);
}

static void draw(void)
{
    char buffer[48];
    status_bar_data_t bar = {0};
    const font_t *big = &font_roboto44_bold;

    bar.state = (status.station ? STATUS_BAR_RECEIVING : STATUS_BAR_IDLE);
    bar.signal_bars = CLAMP((status.rssi - RSSI_BAR_OFFSET) / RSSI_BAR_STEP, 0, ICONS_SIGNAL_BARS);
    bar.item1 = "FM";
    bar.item1_bold = true;
    status_bar_draw(&bar);
    if (status.stereo) {
        icons_draw(STEREO_X, (WIDGET_STATUS_BAR_HEIGHT - ICONS_SIZE) / 2, ICONS_STEREO, THEME_COLOUR_RX);
    }

    // The frequency with MHz, centred as one; or the digits being typed
    if (digit_count > 0) {
        snprintf(buffer, sizeof(buffer), "%s_", digits);
        draw_text(FB_WIDTH / 2, FREQ_Y, buffer, big, FONT_ALIGN_CENTER, THEME_COLOUR_ACCENT);
    } else {
        snprintf(buffer, sizeof(buffer), "%d.%d", status.frequency / 10, status.frequency % 10);

        int32_t w = font_text_width(big, buffer) + 6 + font_text_width(&font_roboto20, "MHz");
        int32_t x =
            font_draw_text((FB_WIDTH - w) / 2, FREQ_Y, buffer, big, FONT_ALIGN_LEFT, theme_colour(THEME_COLOUR_TEXT));

        draw_text(x + 6, FREQ_Y + big->ascent - font_roboto20.ascent, "MHz", &font_roboto20, FONT_ALIGN_LEFT,
                  THEME_COLOUR_TEXT_LABEL);
    }

    if (status.seeking) {
        draw_text(FB_WIDTH / 2, NAME_Y, text(TEXT_UI_SEEKING), &font_roboto22, FONT_ALIGN_CENTER, THEME_COLOUR_WARNING);
    } else if (fm_broadcast_rds_name()[0] != '\0') {
        draw_text(FB_WIDTH / 2, NAME_Y, fm_broadcast_rds_name(), &font_roboto22, FONT_ALIGN_CENTER, THEME_COLOUR_RX);
    }

    draw_dial(status.frequency);
    draw_wrapped(TEXT_Y, fm_broadcast_rds_text(), &font_roboto17, THEME_COLOUR_TEXT_SECOND);
    draw_text(FB_WIDTH / 2, HINT_Y, text(TEXT_UI_FM_HINT), &font_roboto17, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_DIM);

    snprintf(buffer, sizeof(buffer), "%s %s", text(TEXT_SQUELCH), text(squelch ? TEXT_ON : TEXT_OFF));
    bottom_bar_draw_soft_keys(buffer, text(TEXT_UI_BACK));
}

const ui_screen_t fm_radio_screen = {
    .enter = enter,
    .leave = leave,
    .handle = handle,
    .draw = draw,
};
