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
 * OpenDMR, screen: transmit (uiTxScreen.c and ui/widgets/txPanel.c of the main firmware, DM-32 drawing;
 * docs/ui-design.md 5.4). The screen is drawn first, then the radio transmits; the timer is drawn each second. The PTT
 * release, or the channel's timeout, ends it. A DTMF sequence (tx_screen_send_dtmf()) ends by itself.
 */

#include <stdio.h>
#include <string.h>
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "framebuffer.h"
#include "systime.h"
#include "settings.h"
#include "icons.h"
#include "radio.h"
#include "list_screen.h"
#include "sound.h"
#include "dmr.h"
#include "tx_target.h"
#include "tx_screen.h"
#include "aprs.h"

// Layout of the main firmware's transmit panel
#define LABEL_Y       42
#define TITLE_Y       62
#define DETAIL_Y      102
#define SEPARATOR_Y   134
#define TIMER_Y       140
#define TIMER_LABEL_Y 192
#define TIMER_BAR_Y   220
#define TIMER_BAR_H   10
#define WARNING_S     10 // the last seconds before the timeout: in the warning colour
#define TOT_UNIT_S    15 // codeplug tot: 15 s units, 0: none
#define TOT_BEEP_S    5  // the timeout warning: a beep each 5 s of the last tx_timeout_beep_x5_secs x 5 s

#define FLAG4_WIDE 0x02
#define TONE_BURST_HZ 1750
#define BEEP_DMR_TX_START 0x01 // settings.beep_options
#define BEEP_POLL_MS      10

typedef enum {
    TX_REFUSED,
    TX_STARTING, // drawn, not transmitting yet
    TX_BEEPING,  // DMR: the start beep plays, then it transmits (uiTxScreen.c, as the stock firmware)
    TX_ON,
    TX_TIMEOUT,
    TX_NO_REPEATER,
} tx_state_t;

static struct {
    char label[48];
    char title[48];
    tx_target_t target; // DMR: the TG / PC and the timeslot of this transmission
    codeplug_channel_t channel;
    tx_state_t state;
    radio_tx_check_t refused;
    uint32_t start_ms;
} tx;

static const ui_screen_t tx_screen;
static uint8_t signal_key = UI_KEY_COUNT; // the key sending 1750 Hz or DTMF; UI_KEY_COUNT: none

// A DTMF sequence being sent (dtmfProcess()): length 0 when none
static struct {
    uint8_t code[CODEPLUG_DTMF_CODE_LENGTH];
    uint8_t length;
    uint8_t position;
    bool in_tone;
    codeplug_dtmf_durations_t durations;
} sequence;

#define DTMF_STAR 14
#define DTMF_HASH 15

void tx_screen_open(const char *label, const char *title, const codeplug_channel_t *channel)
{
    snprintf(tx.label, sizeof(tx.label), "%s", label ? label : "");
    snprintf(tx.title, sizeof(tx.title), "%s", title ? title : "");
    memcpy(&tx.channel, channel, sizeof(tx.channel));
    tx_target_get(&tx.channel, &tx.target); // from the screen the PTT was pressed on
    ui_push(&tx_screen, (void *)&tx);
}

static uint32_t elapsed_s(void)
{
    return (systime_get_ms() - tx.start_ms) / 1000;
}

// The DTMF pair of a code 0 - 15 ("0123456789ABCD*#")
static void dtmf_pair(uint8_t code, uint16_t *low, uint16_t *high)
{
    static const uint16_t rows[4] = {697, 770, 852, 941};
    static const uint16_t columns[4] = {1209, 1336, 1477, 1633};
    static const uint8_t places[16] = {13, 0, 1, 2, 4, 5, 6, 8, 9, 10, 3, 7, 11, 15, 12, 14}; // row * 4 + column

    *low = rows[places[code & 0x0F] / 4];
    *high = columns[places[code & 0x0F] % 4];
}

bool tx_screen_send_dtmf(const uint8_t *code)
{
    codeplug_channel_t channel;
    char shown[CODEPLUG_DTMF_CODE_LENGTH + 1];
    uint8_t length = 0;

    if (!radio_get_channel(&channel) || (channel.mode == CODEPLUG_MODE_DMR)) {
        return false;
    }
    while ((length < CODEPLUG_DTMF_CODE_LENGTH) && (code[length] != 0xFF)) {
        shown[length] = "0123456789ABCD*#"[code[length] & 0x0F];
        length++;
    }
    if (length == 0) {
        return false;
    }
    shown[length] = '\0';
    memcpy(sequence.code, code, length);
    sequence.length = length;
    sequence.position = 0;
    sequence.in_tone = false;
    codeplug_get_dtmf_durations(&sequence.durations);
    tx_screen_open("DTMF", shown, &channel);
    return true;
}

// dtmfGetToneDuration() / dtmfProcess(): a tone and a pause of half the digit time each; the first digit longer by
// the first digit time, * and # by their own (x 10 ms, as the main firmware); the tail after the last one. The time to
// the next step, 0: done.
static uint32_t sequence_step(void)
{
    uint32_t half = 1000 / (sequence.durations.rate * 2);

    if (sequence.in_tone) {
        radio_tx_signal(0, 0);
        sound_play_dtmf(0, 0); // silence, the speaker kept on for the next digit
        sequence.in_tone = false;
        sequence.position++;
        return (sequence.position < sequence.length) ? half : (half + (sequence.durations.tail * 100U));
    }
    if (sequence.position >= sequence.length) {
        sound_play(NULL);
        return 0;
    }

    uint8_t code = sequence.code[sequence.position];
    uint32_t extra = 0;
    uint16_t low;
    uint16_t high;

    if ((code == DTMF_STAR) || (code == DTMF_HASH)) {
        extra = sequence.durations.other_duration * 10U;
    } else if (sequence.position == 0) {
        extra = sequence.durations.first_duration * 10U;
    }
    dtmf_pair(code, &low, &high);
    radio_tx_signal(low, high);
    sound_play_dtmf(low, high);
    sequence.in_tone = true;
    return half + extra;
}

static void stop(void)
{
    if ((tx.state == TX_STARTING) || (tx.state == TX_BEEPING) || (tx.state == TX_ON)) {
        radio_transmit(false);
    }
}

static void enter(void *arg)
{
    if (!arg) {
        return; // nothing goes over it
    }
    tx.refused = radio_tx_check();
    tx.state = (tx.refused == RADIO_TX_OK) ? TX_STARTING : TX_REFUSED;
    if (tx.state == TX_STARTING) {
        ui_timer_start(0); // after the first draw: start()
    } else {
        sound_play(SOUND_ERROR); // menuTxScreenHandleTxTermination()
    }
}

static bool dmr(void)
{
    return (tx.channel.mode == CODEPLUG_MODE_DMR);
}

// The first event after the push: the screen is on the display, now transmit (DMR: after the start beep)
static void start(void)
{
    if ((tx.state == TX_STARTING) && dmr() && (settings.beep_options & BEEP_DMR_TX_START) &&
        (settings.audio_prompt_mode != SOUND_PROMPT_SILENT)) {
        sound_play(SOUND_DMR_TX_START);
        tx.state = TX_BEEPING;
        ui_timer_start(BEEP_POLL_MS);
        return;
    }
    if (sequence.length) {
        radio_transmit_plain(); // the DTMF alone, as the main firmware
    } else {
        radio_transmit(true);
    }
    tx.start_ms = systime_get_ms();
    tx.state = TX_ON;
    // a sequence: the first digit delay (x 100 ms) first, the preamble
    ui_timer_start(sequence.length ? (sequence.durations.first_digit_delay * 100U) : 1000);
}

// The DTMF pair of a keypad key (low, high); false: not a DTMF key
static bool dtmf_of(uint8_t key, uint16_t *low, uint16_t *high)
{
    int digit = ui_key_digit(key);

    if (digit >= 0) {
        dtmf_pair((uint8_t)digit, low, high);
    } else if (key == UI_KEY_STAR) {
        dtmf_pair(DTMF_STAR, low, high);
    } else if (key == UI_KEY_HASH) {
        dtmf_pair(DTMF_HASH, low, high);
    } else {
        return false;
    }
    return true;
}

// uiTxScreen.c: while transmitting FM, SK2 held sends 1750 Hz, a keypad key held its DTMF, heard here too; the key
// gets no key beep. true: the key was one of them.
static bool signal_keys(const ui_event_t *ev)
{
    uint16_t low;
    uint16_t high;

    if ((ev->type == UI_EVENT_KEY_PRESS) && (signal_key == UI_KEY_COUNT) && (tx.state == TX_ON) &&
        (tx.channel.mode != CODEPLUG_MODE_DMR)) {
        if (ev->key == UI_KEY_SK2) {
            radio_tx_signal(TONE_BURST_HZ, 0);
            sound_play(SOUND_1750);
        } else if (dtmf_of(ev->key, &low, &high)) {
            radio_tx_signal(low, high);
            sound_play_dtmf(low, high);
        } else {
            return false;
        }
        signal_key = ev->key;
        return true;
    }
    if ((ev->key == signal_key) && ((ev->type == UI_EVENT_KEY_RELEASE) || (ev->type == UI_EVENT_KEY_LONG))) {
        if (ev->type == UI_EVENT_KEY_RELEASE) {
            radio_tx_signal(0, 0);
            sound_play(NULL);
            signal_key = UI_KEY_COUNT;
        }
        ui_key_beep(NULL);
        return true;
    }
    return false;
}

// While a DTMF sequence is sent: the timer steps it, RED stops it, the end closes the screen
static bool handle_sequence(const ui_event_t *ev)
{
    uint32_t next;

    if (ev->type == UI_EVENT_TIMER) {
        if (tx.state == TX_STARTING) {
            start();
            return true;
        }
        next = (tx.state == TX_ON) ? sequence_step() : 0;
        if (next != 0) {
            ui_timer_start(next);
            return sequence.in_tone; // the timer drawn at each digit
        }
    } else if ((ev->type != UI_EVENT_KEY_PRESS) || (ev->key != UI_KEY_RED)) {
        return false; // no other key while sending; refused: RED goes back
    }
    ui_timer_stop();
    ui_pop(); // leave() ends it
    return false;
}

static bool handle(const ui_event_t *ev)
{
    bool ptt_up = (ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_PTT);
    uint32_t tot = tx.channel.tot * TOT_UNIT_S;

    if (sequence.length) {
        return handle_sequence(ev);
    }
    if (signal_keys(ev)) {
        return false;
    }
    if (ptt_up) {
        if (signal_key != UI_KEY_COUNT) {
            radio_tx_signal(0, 0);
            sound_play(NULL);
            signal_key = UI_KEY_COUNT;
        }
        if ((tx.state == TX_ON) && !dmr()) {
            aprs_after_ptt(); // the PTT mode's beacon
        }
        stop();
        ui_timer_stop();
        ui_pop();
        return false;
    }

    switch (tx.state) {
        case TX_STARTING:
            if (ev->type == UI_EVENT_TIMER) {
                start();
                return true;
            }
            break;
        case TX_BEEPING:
            if (ev->type == UI_EVENT_TIMER) {
                if (sound_is_playing()) {
                    ui_timer_start(BEEP_POLL_MS);
                } else {
                    start();
                }
                return true;
            }
            break;
        case TX_ON:
            if (ev->type == UI_EVENT_TIMER) {
                if (dmr() && dmr_repeater_failed()) {
                    stop();
                    tx.state = TX_NO_REPEATER;
                    sound_play(SOUND_ERROR);
                    return true;
                }
                if ((tot > 0) && (elapsed_s() >= tot)) {
                    stop();
                    tx.state = TX_TIMEOUT;
                    sound_play(SOUND_TX_TIMEOUT);
                    return true;
                }
                if ((tot > 0) && ((tot - elapsed_s()) <= (settings.tx_timeout_beep_x5_secs * TOT_BEEP_S)) &&
                    (((tot - elapsed_s()) % TOT_BEEP_S) == 0)) {
                    sound_play(SOUND_KEY);
                }
                ui_timer_start(1000 - ((systime_get_ms() - tx.start_ms) % 1000));
                return true;
            }
            break;
        default:
            break;
    }
    return false; // no other key while transmitting; the refused and timeout pages wait for the PTT release
}

static void leave(void)
{
    if (signal_key != UI_KEY_COUNT) {
        radio_tx_signal(0, 0);
        sound_play(NULL);
        signal_key = UI_KEY_COUNT;
    }
    if (sequence.length) {
        radio_tx_signal(0, 0);
        sound_play(NULL);
        sequence.in_tone = false;
    }
    sequence.length = 0;
    stop();
    tx.state = TX_REFUSED;
}

static void draw_bottom(void)
{
    bottom_bar_data_t bottom = {0};
    int power = (tx.channel.libre_dmr_power != 0) ? (tx.channel.libre_dmr_power - 1) : settings.tx_power_level;

    bottom.mode = codeplug_mode_name(tx.channel.mode);
    bottom.item1 = bottom_bar_power_label(power);
    bottom.item2 = (tx.channel.flag4 & FLAG4_WIDE) ? "25K" : "12.5K";
    bottom_bar_draw(&bottom);
}

static void format_frequency(uint32_t frequency, char *out, size_t size)
{
    snprintf(out, size, "%lu.%04lu MHz", (unsigned long)(frequency / 100000),
             (unsigned long)((frequency % 100000) / 10));
}

// "Can't transmit" / "TX timeout" (uiTxScreen.c drawTxRefused())
static void draw_refused(void)
{
    const int32_t x = WIDGET_MARGIN;
    bool timeout = (tx.state == TX_TIMEOUT);
    theme_colour_t colour = timeout ? THEME_COLOUR_WARNING : THEME_COLOUR_TX;
    status_bar_data_t bar = {.state = STATUS_BAR_IDLE};
    const char *hint = NULL;
    char big[40];
    char buffer[40];

    if (tx.state == TX_NO_REPEATER) {
        snprintf(big, sizeof(big), "%s", text(TEXT_UI_NO_REPEATER));
        hint = text(TEXT_UI_RELEASE_PTT);
    } else if (timeout) {
        uint32_t tot = tx.channel.tot * TOT_UNIT_S;

        snprintf(big, sizeof(big), "%lu:%02lu", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
        hint = text(TEXT_UI_RELEASE_PTT);
    } else if (tx.refused == RADIO_TX_RX_ONLY) {
        snprintf(big, sizeof(big), "%s", text(TEXT_RX_ONLY));
        hint = text(TEXT_UI_RX_ONLY_HINT);
    } else if (tx.refused == RADIO_TX_RX_MODE) {
        snprintf(big, sizeof(big), "%s %s", codeplug_mode_name(tx.channel.mode), text(TEXT_RX_ONLY));
        hint = text(TEXT_UI_RX_MODE_HINT);
    } else {
        snprintf(big, sizeof(big), "%s", text(TEXT_OUT_OF_BAND));
        hint = text(TEXT_UI_OUT_OF_BAND_HINT);
    }

    status_bar_draw(&bar);
    draw_fill_rect(0, WIDGET_STATUS_BAR_HEIGHT, FB_WIDTH, 2, colour);
    icons_draw(x, 45, ICONS_INFO, colour);
    draw_text(x + 28, 44, text(timeout ? TEXT_UI_TX_TIMEOUT : TEXT_UI_TX_REFUSED), &font_roboto17, FONT_ALIGN_LEFT,
              colour);
    draw_text(x, 68, big, &font_roboto32_bold, FONT_ALIGN_LEFT, colour);
    if (hint) {
        draw_text(x, 112, hint, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_SECOND);
    }
    draw_hline(x, 146, FB_WIDTH - (2 * WIDGET_MARGIN), THEME_COLOUR_LINE);
    draw_text(x, 156, tx.title, &font_roboto22, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
    format_frequency(tx.channel.tx_freq, buffer, sizeof(buffer));
    draw_text(x, 188, buffer, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_SECOND);
    draw_bottom();
}

// The time left of the timeout (with its bar), or the time elapsed without one
static void draw_timer(void)
{
    const int32_t x = WIDGET_MARGIN;
    const int32_t width = FB_WIDTH - (2 * WIDGET_MARGIN);
    uint32_t tot = tx.channel.tot * TOT_UNIT_S;
    uint32_t elapsed = (tx.state == TX_ON) ? elapsed_s() : 0;
    uint32_t seconds = (tot > 0) ? ((elapsed < tot) ? (tot - elapsed) : 0) : elapsed;
    bool warning = (tot > 0) && (seconds <= WARNING_S);
    char buffer[32];

    snprintf(buffer, sizeof(buffer), "%lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
    draw_text(FB_WIDTH / 2, TIMER_Y, buffer, &font_roboto44_bold, FONT_ALIGN_CENTER,
              warning ? THEME_COLOUR_WARNING : THEME_COLOUR_TEXT);
    if (tot > 0) {
        snprintf(buffer, sizeof(buffer), "%s %lu:%02lu", text(TEXT_UI_TIME_LEFT_OF), (unsigned long)(tot / 60),
                 (unsigned long)(tot % 60));
        draw_text(FB_WIDTH / 2, TIMER_LABEL_Y, buffer, &font_roboto17, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_LABEL);
        draw_fill_rect(x, TIMER_BAR_Y, width, TIMER_BAR_H, THEME_COLOUR_LINE);
        draw_fill_rect(x, TIMER_BAR_Y, (int32_t)((width * seconds) / tot), TIMER_BAR_H,
                       warning ? THEME_COLOUR_WARNING : THEME_COLOUR_TX);
    } else {
        draw_text(FB_WIDTH / 2, TIMER_LABEL_Y, text(TEXT_UI_ELAPSED), &font_roboto17, FONT_ALIGN_CENTER,
                  THEME_COLOUR_TEXT_LABEL);
    }
}

static void draw(void)
{
    status_bar_data_t bar = {.state = STATUS_BAR_TRANSMITTING}; // "TX" is the bar's own; FM has no items
    char buffer[32];
    char ts[8];
    char cc[8];

    if ((tx.state == TX_REFUSED) || (tx.state == TX_TIMEOUT) || (tx.state == TX_NO_REPEATER)) {
        draw_refused();
        return;
    }

    if (dmr()) { // the timeslot and colour code after "TX" (docs/ui-design.md 5.4)
        snprintf(ts, sizeof(ts), "TS%d", tx.target.timeslot);
        snprintf(cc, sizeof(cc), "CC%d", tx.channel.tx_colour);
        bar.item1 = ts;
        bar.item1_bold = true;
        bar.item2 = cc;
    } else if (tx.channel.mode == CODEPLUG_MODE_P25) {
        snprintf(cc, sizeof(cc), "NAC%03X", codeplug_p25_nac(&tx.channel));
        bar.item2 = cc;
    }
    status_bar_draw(&bar);
    draw_text(WIDGET_MARGIN, LABEL_Y, tx.label, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    draw_text(WIDGET_MARGIN, TITLE_Y, tx.title, &font_roboto32_bold, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
    format_frequency(tx.channel.tx_freq, buffer, sizeof(buffer));
    if (dmr()) {
        // DMR: where it goes (the TG or contact), the frequency on the label's line
        draw_text(FB_WIDTH - WIDGET_MARGIN, LABEL_Y, buffer, &font_roboto17, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT_LABEL);
        draw_text(WIDGET_MARGIN, DETAIL_Y, tx.target.name, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
    } else {
        draw_text(WIDGET_MARGIN, DETAIL_Y, buffer, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_SECOND);
    }
    draw_hline(WIDGET_MARGIN, SEPARATOR_Y, FB_WIDTH - (2 * WIDGET_MARGIN), THEME_COLOUR_LINE);
    draw_timer();
    draw_bottom();
}

static const ui_screen_t tx_screen = {
    .enter = enter,
    .leave = leave,
    .handle = handle,
    .draw = draw,
};
