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
 * OpenDMR, screen: the amateur satellites (menuSatelliteScreen.c, DM-32 drawing; docs/manual
 * satellites.png). The predictions are services/satellite.c, from the keps in the codeplug and the location of the
 * settings.
 *
 *   - the coming passes of all the satellites in time order (two-line rows): UP / DOWN move, the left key opens one;
 *   - a pass: polar track (done solid, to come dotted), the satellite now, azimuth / elevation / maximum, the Doppler
 *     corrected frequencies of one kind (the left key: voice, APRS, other); * shows the passes of the satellite;
 *   - the passes of the satellite, one at a time (UP / DOWN, LEFT / RIGHT, the rotary); * back to the pass.
 *   The right key goes back to the list, from the list out (held: home).
 *
 * Before: the clock and the location must be set (TODO: the main firmware opens the radio information on its location
 * or date page; here a message says what is missing), and the keps loaded by the CPS.
 * The pass screen tunes the radio (calculateActiveSatelliteData()): FM 25 kHz on the corrected frequencies of the kind
 * shown, the satellite's CTCSS, the receive frequency set again when it is 500 Hz off; the PTT transmits on the
 * transmit frequency of that moment. The knob and LEFT / RIGHT change the squelch, shown a moment. Leaving the screen,
 * the home screen tunes its channel again. Left: the pass alarm (SK2 + left key) and its beeps.
 */

#include <stdio.h>
#include <string.h>
#include "util.h"
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "battery.h"
#include "datetime.h"
#include "satellite.h"
#include "gps_screen.h"
#include "list_screen.h"
#include "satellite_screen.h"
#include "radio.h"
#include "tx_screen.h"
#include "systime.h"

#define PREDICT_MS     10   // a prediction step each, while predicting
#define PREDICT_BUDGET 100  // positions a step
#define REFRESH_MS     1000 // then: the countdowns, the passes that end
#define MESSAGE_Y      120
#define PROGRESS_Y     160
#define PROGRESS_H     10
#define POLAR_CX       66
#define POLAR_CY       140
#define POLAR_R        54
#define TRACK_STEP_S   20
#define LOOK_X         132
#define FREQ_Y         210
#define ENTRIES_MAX    (SATELLITE_MAX * SATELLITE_PASSES)
#define RETUNE_10HZ    50   // the receive frequency set again when this much off (500 Hz)
#define SQL_DEFAULT    10   // the main firmware's satellite channel
#define SQL_MIN        1
#define SQL_MAX        21
#define SQL_SHOWN_MS   2000
#define FLAG4_WIDE     0x02
#define FLAG4_RX_ONLY  0x04

typedef enum {
    VIEW_LIST,
    VIEW_PASS,
    VIEW_PASSES
} view_t;

typedef struct {
    uint8_t sat;
    uint8_t pass;
} entry_t;

static bool has_clock;
static bool has_location;
static bool predicted;
static int predicted_shown;
static view_t view;
static int selected; // in the list
static int sat;      // the pass and passes views
static uint32_t pass_aos;
static satellite_freq_kind_t freq_kind;
static entry_t entries[ENTRIES_MAX]; // the list in time order
static int entry_count;
static codeplug_channel_t channel;   // the satellite's, tuned on the pass screen
static bool tuned = false;           // since then the radio is on it (radio_channel()) until the screen is left
static uint8_t sql = SQL_DEFAULT;
static uint32_t sql_shown_until = 0;

static void tune(bool force);

static int32_t local_offset(void)
{
    return (int32_t)(datetime_shown() - datetime_now());
}

static void format_hhmm(char *buffer, size_t size, uint32_t utc)
{
    datetime_fields_t t;

    datetime_to_fields(utc + local_offset(), &t);
    snprintf(buffer, size, "%02u:%02u", t.hour, t.minute);
}

// m:ss, or h:mm:ss over an hour
static void format_duration(char *buffer, size_t size, int32_t seconds)
{
    seconds = MAX(seconds, 0);
    if (seconds >= 3600) {
        snprintf(buffer, size, "%d:%02d:%02d", (int)(seconds / 3600), (int)((seconds / 60) % 60), (int)(seconds % 60));
    } else {
        snprintf(buffer, size, "%d:%02d", (int)(seconds / 60), (int)(seconds % 60));
    }
}

// The passes of all the satellites in time order
static void build_entries(void)
{
    entry_count = 0;
    for (int s = 0; s < satellite_count(); s++) {
        for (int p = 0; p < satellite_pass_count(s); p++) {
            uint32_t aos = satellite_pass(s, p)->aos;
            int at = entry_count++;

            while ((at > 0) && (satellite_pass(entries[at - 1].sat, entries[at - 1].pass)->aos > aos)) {
                entries[at] = entries[at - 1];
                at--;
            }
            entries[at].sat = (uint8_t)s;
            entries[at].pass = (uint8_t)p;
        }
    }
    if (selected >= entry_count) {
        selected = MAX(entry_count - 1, 0);
    }
}

// The pass shown in the pass views: found again by its start (the passes that ended were dropped); -1: none
static int shown_pass(void)
{
    for (int p = 0; p < satellite_pass_count(sat); p++) {
        if (satellite_pass(sat, p)->aos == pass_aos) {
            return p;
        }
    }
    return (satellite_pass_count(sat) > 0) ? 0 : -1;
}

static void enter(void *arg)
{
    int32_t *started = ui_state();

    (void)arg;
    if (*started == 0) {
        *started = 1;
        tuned = false; // the radio on the home screen's channel until a pass is opened
        view = VIEW_LIST;
        selected = 0;
        freq_kind = SATELLITE_FREQ_VOICE;
        has_clock = datetime_is_valid();
        has_location = satellite_set_observer_from_settings();
        predicted = false;
        predicted_shown = 0;
        if (has_clock && has_location && (satellite_load() > 0)) {
            satellite_predict_start(datetime_now());
        }
    }
    if (has_clock && has_location && (satellite_count() > 0)) {
        ui_timer_start(predicted ? REFRESH_MS : PREDICT_MS);
    }
}

static bool timer(void)
{
    bool was = predicted;

    predicted = satellite_predict_step(datetime_now(), PREDICT_BUDGET);
    if (!predicted && was) {
        predicted = true; // a satellite predicted further after a pass ended: the list stays
    }
    ui_timer_start(predicted ? REFRESH_MS : PREDICT_MS);
    if (predicted && (view == VIEW_PASS)) {
        tune(false);
    }
    if (!predicted) {
        int done = satellite_predicted_count();
        bool changed = (done != predicted_shown);

        predicted_shown = done;
        return changed;
    }
    return true;
}

// The channel of the screen (ui_screen_t.channel): the satellite's once a pass was opened, else the home screen's
static const codeplug_channel_t *radio_channel(void)
{
    return tuned ? &channel : UI_CHANNEL_UNDER;
}

// The radio on the corrected frequencies of the kind shown (calculateActiveSatelliteData()); force: set again even
// when it is close (a new kind, the squelch, the PTT: the exact transmit frequency)
static void tune(bool force)
{
    satellite_look_t look;
    const satellite_freq_t *nominal = satellite_freq(sat, freq_kind);
    uint32_t rx;
    uint32_t tx;

    satellite_look(sat, datetime_now(), &look);
    rx = look.rx_hz[freq_kind] / 10;
    tx = look.tx_hz[freq_kind] / 10;
    if (rx == 0) {
        return;
    }
    if (tuned && !force && ((int32_t)(rx - channel.rx_freq) <= RETUNE_10HZ) &&
        ((int32_t)(channel.rx_freq - rx) <= RETUNE_10HZ)) {
        return;
    }
    memset(&channel, 0, sizeof(channel));
    memset(channel.name, 0xFF, sizeof(channel.name));
    channel.mode = CODEPLUG_MODE_FM;
    channel.rx_freq = rx;
    channel.tx_freq = (tx != 0) ? tx : rx;
    channel.flag4 = FLAG4_WIDE | ((tx == 0) ? FLAG4_RX_ONLY : 0);
    channel.rx_tone = CODEPLUG_CSS_NONE;
    channel.tx_tone = ((nominal->tx_ctcss != 0) && (nominal->tx_ctcss != 0xFFFF) && ((nominal->tx_ctcss & 0x8000) == 0))
                          ? nominal->tx_ctcss
                          : CODEPLUG_CSS_NONE;
    channel.sql = sql;
    channel.not_in_codeplug_flag = 0x01;
    tuned = true; // the UI task tunes it after the event (radio_channel())
}

static void open_selected(void)
{
    if (entry_count == 0) {
        return;
    }
    sat = entries[selected].sat;
    pass_aos = satellite_pass(sat, entries[selected].pass)->aos;
    view = VIEW_PASS;
    tuned = false;
    for (int i = 0; i < SATELLITE_FREQS; i++) { // the first kind this satellite has
        if (satellite_freq(sat, (satellite_freq_kind_t)i)->rx_hz != 0) {
            freq_kind = (satellite_freq_kind_t)i;
            break;
        }
    }
    tune(true);
}

static void next_freq_kind(void)
{
    for (int i = 0; i < SATELLITE_FREQS; i++) {
        freq_kind = (satellite_freq_kind_t)((freq_kind + 1) % SATELLITE_FREQS);
        if (satellite_freq(sat, freq_kind)->rx_hz != 0) {
            break;
        }
    }
    tune(true);
}

static void move_pass(int step)
{
    int p = shown_pass();

    if (p >= 0) {
        p = CLAMP(p + step, 0, satellite_pass_count(sat) - 1);
        pass_aos = satellite_pass(sat, p)->aos;
    }
}

static bool handle(const ui_event_t *ev)
{
    bool press = (ev->type == UI_EVENT_KEY_PRESS);
    bool step = press || (ev->type == UI_EVENT_KEY_REPEAT);

    if (ev->type == UI_EVENT_TIMER) {
        return timer();
    }
    if (ev->type == UI_EVENT_DATA) {
        return true;
    }
    if (!predicted || (view == VIEW_LIST)) {
        if (predicted && step && ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN)) && (entry_count > 0)) {
            selected = (selected + ((ev->key == UI_KEY_DOWN) ? 1 : -1) + entry_count) % entry_count;
            return true;
        }
        if (predicted && press && (ev->key == UI_KEY_GREEN)) {
            open_selected();
            return true;
        }
        list_handle_back(ev);
        return false;
    }

    // The pass views: the right key back to the list
    if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_RED) && !ev->long_press) {
        view = VIEW_LIST;
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_LONG) && (ev->key == UI_KEY_RED)) {
        ui_home();
        return false;
    }
    if (press && (ev->key == UI_KEY_STAR)) {
        view = ((view == VIEW_PASS) ? VIEW_PASSES : VIEW_PASS);
        return true;
    }
    if (view == VIEW_PASS) {
        int direction = (ev->type == UI_EVENT_ROTARY) ? ev->steps
                        : (step && (ev->key == UI_KEY_RIGHT)) ? 1
                        : (step && (ev->key == UI_KEY_LEFT))  ? -1
                                                              : 0;

        if (press && (ev->key == UI_KEY_GREEN)) {
            next_freq_kind();
            return true;
        }
        if (press && (ev->key == UI_KEY_PTT) && tuned) {
            tune(true); // the transmit frequency of this moment
            tx_screen_open(satellite_name(sat), text(TEXT_SATELLITE), &channel);
            return false;
        }
        if (direction != 0) {
            sql = (uint8_t)CLAMP(sql + direction, SQL_MIN, SQL_MAX);
            sql_shown_until = systime_get_ms() + SQL_SHOWN_MS;
            tune(true);
            ui_timer_start(SQL_SHOWN_MS); // drawn again without it after
            return true;
        }
        return false;
    }
    if (ev->type == UI_EVENT_ROTARY) {
        move_pass(ev->steps);
        return true;
    }
    if (step && ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_LEFT))) {
        move_pass(-1);
        return true;
    }
    if (step && ((ev->key == UI_KEY_DOWN) || (ev->key == UI_KEY_RIGHT))) {
        move_pass(1);
        return true;
    }
    return false;
}

static void draw_status(const char *item1, const char *item2, bool in_pass)
{
    status_bar_data_t bar = {0};

    bar.state = (in_pass ? STATUS_BAR_RECEIVING : STATUS_BAR_IDLE);
    bar.item1 = item1;
    bar.item1_bold = true;
    bar.item2 = item2;
    status_bar_draw(&bar);
}

static void draw_message(const char *line1, const char *line2)
{
    draw_status(text(TEXT_SATELLITE), NULL, false);
    draw_text(FB_WIDTH / 2, MESSAGE_Y, line1, &font_roboto22, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT);
    if (line2) {
        draw_text(FB_WIDTH / 2, MESSAGE_Y + 32, line2, &font_roboto17, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_LABEL);
    }
    bottom_bar_draw_soft_keys(NULL, text(TEXT_UI_BACK));
}

static void draw_predicting(void)
{
    const int32_t w = FB_WIDTH - (2 * WIDGET_MARGIN);

    draw_status(text(TEXT_SATELLITE), NULL, false);
    draw_text(FB_WIDTH / 2, MESSAGE_Y, text(TEXT_PREDICTING), &font_roboto22, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT);
    draw_fill_rect(WIDGET_MARGIN, PROGRESS_Y, w, PROGRESS_H, THEME_COLOUR_LINE);
    draw_fill_rect(WIDGET_MARGIN, PROGRESS_Y, (w * predicted_shown) / MAX(satellite_count(), 1), PROGRESS_H,
                   THEME_COLOUR_ACCENT);
    bottom_bar_draw_soft_keys(NULL, text(TEXT_UI_BACK));
}

static void draw_list(void)
{
    uint32_t now = datetime_now();
    char position[16];
    int top = (selected / LIST_VIEW_ROWS2) * LIST_VIEW_ROWS2;

    build_entries();
    snprintf(position, sizeof(position), "%d/%d", (entry_count > 0) ? (selected + 1) : 0, entry_count);
    draw_status(text(TEXT_SATELLITE), position, false);

    if (entry_count == 0) {
        list_view_draw_text(text(TEXT_LIST_EMPTY), NULL, THEME_COLOUR_TEXT_LABEL);
    }
    for (int row = 0; (row < LIST_VIEW_ROWS2) && ((top + row) < entry_count); row++) {
        const entry_t *e = &entries[top + row];
        const satellite_pass_t *pass = satellite_pass(e->sat, e->pass);
        bool in_pass = (now >= pass->aos) && (now <= pass->los);
        char value[16];
        char sub[40];
        char d[16];
        list_view_row_t data = {.icon = ICONS_COUNT, .selected = ((top + row) == selected)};

        if (in_pass) {
            snprintf(value, sizeof(value), "%s", text(TEXT_UI_IN_PASS));
            format_duration(d, sizeof(d), (int32_t)(pass->los - now));
            snprintf(sub, sizeof(sub), "%s %d°   %s %s", text(TEXT_MAXIMUM), satellite_max_elevation(e->sat, e->pass),
                     text(TEXT_UI_TIME_LEFT), d);
        } else {
            format_hhmm(value, sizeof(value), pass->aos);
            snprintf(sub, sizeof(sub), "%s %d°   %d %s", text(TEXT_MAXIMUM), satellite_max_elevation(e->sat, e->pass),
                     (int)((pass->los - pass->aos + 30) / 60), text(TEXT_UI_MINUTES_SHORT));
        }
        data.name = satellite_name(e->sat);
        data.value = value;
        data.sub = sub;
        list_view_draw_row2(LIST_VIEW_TOP + (row * LIST_VIEW_ROW2_HEIGHT), &data);
    }
    bottom_bar_draw_soft_keys((entry_count > 0) ? text(TEXT_UI_SELECT) : NULL, text(TEXT_UI_BACK));
}

static void draw_track(const satellite_pass_t *pass, uint32_t now)
{
    satellite_look_t look;
    int32_t last_x = 0;
    int32_t last_y = 0;

    for (uint32_t t = pass->aos; t <= pass->los; t += TRACK_STEP_S) {
        int32_t x;
        int32_t y;

        satellite_look(sat, t, &look);
        gps_screen_sky_point(POLAR_CX, POLAR_CY, POLAR_R, look.azimuth, look.elevation, &x, &y);
        if (t != pass->aos) {
            bool done = (t <= now);

            if (done || ((t / TRACK_STEP_S) & 1)) {
                draw_line(last_x, last_y, x, y, THEME_COLOUR_RX, done);
            }
        }
        last_x = x;
        last_y = y;
    }
}

static void draw_look_value(int32_t y, text_id_t label, const char *value)
{
    draw_text(LOOK_X, y + 4, text(label), &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    draw_text(LOOK_X + 40, y, value, &font_roboto22, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
}

static void draw_pass(void)
{
    // As the main firmware: "Voice", "APRS", "CW Rx"
    const char *kind_name = (freq_kind == SATELLITE_FREQ_VOICE)  ? text(TEXT_VOICE_PROMPT_LEVEL_1)
                            : (freq_kind == SATELLITE_FREQ_APRS) ? text(TEXT_APRS)
                                                                 : text(TEXT_CW_RX);
    uint32_t now = datetime_now();
    int p = shown_pass();
    const satellite_pass_t *pass = (p >= 0) ? satellite_pass(sat, p) : NULL;
    bool in_pass = pass && (now >= pass->aos) && (now <= pass->los);
    satellite_look_t look;
    char buffer[40];
    char d[16];
    int32_t x;

    satellite_look(sat, now, &look);
    draw_status("SAT", kind_name, in_pass);

    // The name, in pass or the time to it
    x = font_draw_text(WIDGET_MARGIN, 40, satellite_name(sat), &font_roboto22, FONT_ALIGN_LEFT,
                       theme_colour(THEME_COLOUR_TEXT)) +
        8;
    if (pass) {
        if (in_pass) {
            draw_chip(x, 42, 22, text(TEXT_UI_IN_PASS), &font_roboto17, THEME_COLOUR_BACKGROUND, THEME_COLOUR_RX);
            format_duration(d, sizeof(d), (int32_t)(pass->los - now));
            snprintf(buffer, sizeof(buffer), "%s %s", text(TEXT_UI_TIME_LEFT), d);
        } else {
            format_duration(d, sizeof(d), (int32_t)(pass->aos - now));
            snprintf(buffer, sizeof(buffer), "%s %s", text(TEXT_UI_TIME_TO), d);
        }
        draw_text(FB_WIDTH - WIDGET_MARGIN, 44, buffer, &font_roboto17, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT_SECOND);
    } else {
        snprintf(buffer, sizeof(buffer), "%s: %s", text(TEXT_PASS), text(TEXT_NONE));
        draw_text(FB_WIDTH - WIDGET_MARGIN, 44, buffer, &font_roboto17, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT_LABEL);
    }

    // Polar plot: the track, the satellite now
    gps_screen_draw_sky(POLAR_CX, POLAR_CY, POLAR_R, false);
    if (pass) {
        draw_track(pass, now);
        if (in_pass) {
            int32_t px;
            int32_t py;

            gps_screen_sky_point(POLAR_CX, POLAR_CY, POLAR_R, look.azimuth, look.elevation, &px, &py);
            draw_fill_circle(px, py, 5 * 4, THEME_COLOUR_RX);
        }
    }

    // Azimuth, elevation now, maximum of the pass
    snprintf(buffer, sizeof(buffer), "%d°", look.azimuth);
    draw_look_value(92, TEXT_AZIMUTH, in_pass ? buffer : "--");
    snprintf(buffer, sizeof(buffer), "%d°", look.elevation);
    draw_look_value(120, TEXT_ELEVATION, in_pass ? buffer : "--");
    if (pass) {
        snprintf(buffer, sizeof(buffer), "%d°", satellite_max_elevation(sat, p));
        draw_look_value(148, TEXT_MAXIMUM, buffer);
    }

    draw_hline(WIDGET_MARGIN, FREQ_Y - 6, FB_WIDTH - (2 * WIDGET_MARGIN), THEME_COLOUR_LINE);

    // The frequencies corrected for the Doppler shift, the shift after them
    const satellite_freq_t *nominal = satellite_freq(sat, freq_kind);
    const struct {
        text_id_t label;
        uint32_t hz;
        uint32_t base;
    } lines[2] = {
        {TEXT_UI_RX, look.rx_hz[freq_kind], nominal->rx_hz},
        {TEXT_UI_TX, look.tx_hz[freq_kind], nominal->tx_hz},
    };

    for (int i = 0; i < 2; i++) {
        int32_t y = FREQ_Y + (i * 24);

        draw_text(WIDGET_MARGIN, y + 2, text(lines[i].label), &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
        if (lines[i].hz != 0) {
            int32_t shift = (int32_t)lines[i].hz - (int32_t)lines[i].base;
            int32_t magnitude = (shift < 0) ? -shift : shift;

            snprintf(buffer, sizeof(buffer), "%lu.%04lu", (unsigned long)(lines[i].hz / 1000000),
                     (unsigned long)((lines[i].hz % 1000000) / 100));
            draw_text(WIDGET_MARGIN + 36, y, buffer, &font_roboto22, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
            snprintf(buffer, sizeof(buffer), "%c%d.%dk", ((shift < 0) ? '-' : '+'), (int)(magnitude / 1000),
                     (int)((magnitude % 1000) / 100));
            draw_text(FB_WIDTH - WIDGET_MARGIN, y + 2, buffer, &font_roboto17, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT_DIM);
        } else {
            draw_text(WIDGET_MARGIN + 36, y, "--", &font_roboto22, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
        }
    }

    if ((nominal->tx_ctcss != 0) && (nominal->tx_ctcss != 0xFFFF) && ((nominal->tx_ctcss & 0x8000) == 0)) {
        snprintf(buffer, sizeof(buffer), "%s %d.%d Hz", text(TEXT_UI_TONE), nominal->tx_ctcss / 10,
                 nominal->tx_ctcss % 10);
        draw_text(WIDGET_MARGIN, FREQ_Y + 50, buffer, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_SECOND);
    }

    if ((int32_t)(sql_shown_until - systime_get_ms()) > 0) {
        snprintf(buffer, sizeof(buffer), "%s %d%%", text(TEXT_SQUELCH), (sql - SQL_MIN) * 5);
        draw_text(FB_WIDTH - WIDGET_MARGIN, FREQ_Y + 50, buffer, &font_roboto17, FONT_ALIGN_RIGHT, THEME_COLOUR_ACCENT);
    }
    bottom_bar_draw_soft_keys(kind_name, text(TEXT_UI_BACK));
}

static void draw_passes(void)
{
    int p = shown_pass();
    char buffer[40];
    char position[16];

    snprintf(position, sizeof(position), "%d/%d", p + 1, satellite_pass_count(sat));
    draw_status(satellite_name(sat), position, false);
    draw_text(FB_WIDTH / 2, 50, satellite_name(sat), &font_roboto32_bold, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT);

    if (p >= 0) {
        const satellite_pass_t *pass = satellite_pass(sat, p);
        datetime_fields_t t;

        datetime_to_fields(pass->aos + local_offset(), &t);
        snprintf(buffer, sizeof(buffer), "%02u-%02u  %02u:%02u", t.month, t.day, t.hour, t.minute);
        draw_text(WIDGET_MARGIN, 110, text(TEXT_UI_START), &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
        draw_text(WIDGET_MARGIN + 60, 106, buffer, &font_roboto22, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);

        snprintf(buffer, sizeof(buffer), "%d°", satellite_max_elevation(sat, p));
        draw_text(WIDGET_MARGIN, 144, text(TEXT_MAXIMUM), &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
        draw_text(WIDGET_MARGIN + 60, 140, buffer, &font_roboto22, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);

        snprintf(buffer, sizeof(buffer), "%d %s", (int)((pass->los - pass->aos + 30) / 60), text(TEXT_MINUTES));
        draw_text(WIDGET_MARGIN, 178, text(TEXT_UI_DURATION), &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
        draw_text(WIDGET_MARGIN + 60, 174, buffer, &font_roboto22, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
    } else {
        draw_text(FB_WIDTH / 2, 140, text(TEXT_LIST_EMPTY), &font_roboto22, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_LABEL);
    }
    bottom_bar_draw_soft_keys(NULL, text(TEXT_UI_BACK));
}

static void draw(void)
{
    if (!has_clock) {
        draw_message(text(TEXT_TIME), text(TEXT_NOT_SET));
    } else if (!has_location) {
        draw_message(text(TEXT_LOCATION), text(TEXT_NOT_SET));
    } else if (satellite_count() == 0) {
        draw_message(text(TEXT_LIST_EMPTY), text(TEXT_UI_NO_KEPS));
    } else if (!predicted) {
        draw_predicting();
    } else if (view == VIEW_LIST) {
        draw_list();
    } else if (view == VIEW_PASS) {
        draw_pass();
    } else {
        draw_passes();
    }
}

const ui_screen_t satellite_screen = {
    .enter = enter,
    .handle = handle,
    .draw = draw,
    .channel = radio_channel,
};
