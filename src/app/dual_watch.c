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
 * OpenDMR, screen: the dual watch (dual_watch.h). After OpenDM32 user_interface/uiDualWatch.c.
 *
 * Two paths, each any channel or VFO A / B (settings.dual_watch_path: a channel number, -1 VFO A, -2 VFO B; 0 is VFO A
 * for the first path, VFO B for the second), the same one twice allowed (one panel then). Watched in turn, a dwell on
 * each; a carrier (FM) or timeslot sync (DMR) is checked for audio, then it stays on that path while the signal lasts
 * and DW_HANG_MS after it (the answer). The transmit path: the one heard last (DW_FOLLOW_MS), or a fixed one (UP the
 * upper, DOWN the lower; "follow" again in the menu).
 *
 * Keys: the left key the main menu, held the dual watch menu (off, the transmit path); the right key back to the
 * single watch; the PTT transmits on the transmit path.
 */

#include <stdio.h>
#include <string.h>
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "icons.h"
#include "settings.h"
#include "codeplug.h"
#include "codeplug_text.h"
#include "systime.h"
#include "radio.h"
#include "dmr.h"
#include "tones.h"
#include "text.h"
#include "menus.h"
#include "list_screen.h"
#include "vfo_screen.h"
#include "tx_screen.h"
#include "tx_target.h"
#include "dual_watch.h"

#define BIT_DUAL_WATCH    (1UL << 10) // BIT_DUAL_WATCH (BIT_UNUSED_1)
#define BIT_DW_TX_1       (1UL << 25) // the transmit path fixed: the first
#define BIT_DW_TX_2       (1UL << 26) // the second (neither: follow)

#define DW_TICK_MS        10
#define DW_DWELL_FM_MS    135   // as the VFO dual watch of OpenGD77
#define DW_DWELL_DMR_MS   150   // the HR_C7000 needs a few timeslots to see a DMR signal
#define DW_SETTLE_MS      30    // after a change of frequency, before looking for a signal
#define DW_CHECK_FM_MS    500   // carrier seen: time for the squelch and the tone to open the audio
#define DW_CHECK_DMR_MS   1500  // sync seen: time for the voice (1 superframe + 1 timeslot, 4 times)
#define DW_HANG_MS        3000  // after the signal, on that path, for the answer
#define DW_FOLLOW_MS      10000 // the transmission follows the path heard during this time after its signal
#define DW_REDRAW_RX_MS   250

#define PANEL_TOP    (WIDGET_STATUS_BAR_HEIGHT + 2)
#define PANEL_HEIGHT ((WIDGET_BOTTOM_BAR_TOP - PANEL_TOP) / 2)

typedef enum {
    STATE_WATCHING, // switching between the two paths
    STATE_CHECKING, // something on the path: does it open the audio
    STATE_RECEIVING,
    STATE_HANG // after the signal (or our transmission), staying on the path
} state_t;

typedef struct {
    codeplug_channel_t channel;
    uint32_t talk_group; // DMR: the contact of the channel, or the first of its RX group
    char name[20];
    bool vfo;
} path_t;

static path_t paths[2];
static int current = 0;
static state_t state = STATE_WATCHING;
static uint32_t state_ms;  // when the path was tuned, or the state started
static uint32_t signal_ms; // the last time the signal was there
static int heard = -1;     // the path heard last
static uint32_t heard_ms;
static uint32_t redraw_ms;
static bool menu_open = false;
static int menu_item = 0;
static int menu_tx = 0; // 0 follow, 1 the first path, 2 the second
static bool green_down = false;
static bool red_down = false;
static bool transmitted = false; // the transmit screen was pushed: back from it, stay on that path

bool dual_watch_is_on(void)
{
    return (settings.bitfield_options & BIT_DUAL_WATCH) != 0;
}

void dual_watch_set(bool on)
{
    settings_set_option(BIT_DUAL_WATCH, on);
    settings_save_if_changed();
}

bool dual_watch_show(void)
{
    if (!dual_watch_is_on()) {
        return false;
    }
    ui_push(&dual_watch_screen, NULL);
    return true;
}

static int16_t path_ref(int i)
{
    int16_t ref = settings.dual_watch_path[i];

    return (ref == 0) ? (int16_t)(-1 - i) : ref;
}

static bool paths_same(void)
{
    return path_ref(0) == path_ref(1);
}

static void load_path(int i)
{
    int16_t ref = path_ref(i);
    path_t *p = &paths[i];
    codeplug_contact_t contact;
    codeplug_rx_group_t group;

    if ((ref > 0) && codeplug_channel_in_use(ref) && codeplug_get_channel(ref, &p->channel)) {
        p->vfo = false;
        codeplug_text_name(p->channel.name, CODEPLUG_NAME_LENGTH, p->name, sizeof(p->name));
    } else {
        int n = (ref == -2) ? 1 : 0;

        memcpy(&p->channel, vfo_screen_vfo(n), sizeof(p->channel));
        // the VFO's own power (0: the radio's), as the VFO screen
        p->vfo = true;
        snprintf(p->name, sizeof(p->name), "VFO %c", (n == 1) ? 'B' : 'A');
    }

    p->talk_group = 9; // as the VFO screen when nothing is set
    if (p->channel.mode == CODEPLUG_MODE_DMR) {
        if ((p->channel.rx_group_list != 0) && codeplug_get_rx_group(p->channel.rx_group_list, &group) &&
            (group.contact_count > 0) && codeplug_get_contact(group.contacts[0], &contact)) {
            p->talk_group = contact.id;
        }
        if ((p->channel.contact != 0) && codeplug_get_contact(p->channel.contact, &contact)) {
            p->talk_group = contact.id |
                            ((contact.call_type == CODEPLUG_CALL_PRIVATE) ? ((uint32_t)TX_TARGET_PC_FLAG << 24) : 0);
        }
    }
}

static void tune_path(int i, uint32_t now)
{
    current = i;
    state_ms = now;
    radio_set_channel(&paths[i].channel);
    if (paths[i].channel.mode == CODEPLUG_MODE_DMR) {
        dmr_set_tx_id(paths[i].talk_group);
    }
}

static int tx_path(uint32_t now)
{
    if (settings.bitfield_options & BIT_DW_TX_1) {
        return 0;
    }
    if (settings.bitfield_options & BIT_DW_TX_2) {
        return 1;
    }
    if ((state == STATE_RECEIVING) || (state == STATE_HANG)) {
        return current;
    }
    if ((heard >= 0) && ((now - heard_ms) < DW_FOLLOW_MS)) {
        return heard;
    }
    return 0;
}

static bool dmr(void)
{
    return paths[current].channel.mode == CODEPLUG_MODE_DMR;
}

// Every DW_TICK_MS (uiDualWatch.c watch()); true: redraw
static bool watch(uint32_t now)
{
    uint32_t elapsed = now - state_ms;
    state_t was = state;

    switch (state) {
        case STATE_WATCHING:
            if (elapsed < DW_SETTLE_MS) {
                break;
            }
            if (radio_audio_heard()) {
                state = STATE_RECEIVING;
                signal_ms = now;
            } else if (dmr() ? dmr_has_sync() : radio_has_carrier()) {
                state = STATE_CHECKING;
                state_ms = now;
            } else if ((elapsed >= (dmr() ? DW_DWELL_DMR_MS : DW_DWELL_FM_MS)) && !paths_same()) {
                tune_path(1 - current, now);
            }
            break;
        case STATE_CHECKING:
            if (radio_audio_heard()) {
                state = STATE_RECEIVING;
                signal_ms = now;
            } else if (elapsed >= (dmr() ? DW_CHECK_DMR_MS : DW_CHECK_FM_MS)) {
                state = STATE_WATCHING; // nothing for us (another tone, another colour code): go on
                if (!paths_same()) {
                    tune_path(1 - current, now);
                }
            }
            break;
        case STATE_RECEIVING:
        case STATE_HANG:
            if (radio_audio_heard()) {
                state = STATE_RECEIVING;
                signal_ms = now;
                heard = current;
                heard_ms = now;
            } else {
                if (state == STATE_RECEIVING) {
                    state = STATE_HANG;
                }
                if ((now - signal_ms) >= DW_HANG_MS) {
                    state = STATE_WATCHING;
                    state_ms = now;
                }
            }
            break;
    }
    if ((state != was) || ((state == STATE_RECEIVING) && ((now - redraw_ms) >= DW_REDRAW_RX_MS))) {
        redraw_ms = now;
        return true;
    }
    return false;
}

static void enter(void *arg)
{
    uint32_t now = systime_get_ms();
    bool from_tx = transmitted; // back from the transmission (uiDualWatchPrepareTx())

    (void)arg;
    transmitted = false;

    load_path(0);
    load_path(1);
    menu_open = false;
    green_down = red_down = false;
    if (from_tx) {
        tune_path(current, now);
        signal_ms = now;
    } else {
        tune_path(0, now);
        state = STATE_WATCHING;
    }
    radio_set_scanning(true); // no receive beeps at each path
    ui_timer_start(DW_TICK_MS);
}

static void leave(void)
{
    radio_set_scanning(false);
}

// The PTT: on the transmit path, then stay on it for the answer
static void transmit(void)
{
    uint32_t now = systime_get_ms();
    int p = tx_path(now);

    if (p != current) {
        tune_path(p, now);
    }
    state = STATE_HANG;
    signal_ms = now;
    heard = p;
    heard_ms = now;
    transmitted = true;
    tx_screen_open(text(TEXT_DUAL_WATCH), paths[p].name, &paths[p].channel);
}

static bool handle_menu(const ui_event_t *ev)
{
    if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_RED)) {
        menu_open = false;
    } else if ((ev->type == UI_EVENT_KEY_PRESS) && ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN))) {
        menu_item = 1 - menu_item;
    } else if ((menu_item == 1) && (ev->type == UI_EVENT_KEY_PRESS) &&
               ((ev->key == UI_KEY_RIGHT) || (ev->key == UI_KEY_LEFT))) {
        menu_tx = (menu_tx + ((ev->key == UI_KEY_RIGHT) ? 1 : 2)) % 3;
    } else if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_GREEN) && !ev->long_press) {
        if (menu_item == 0) {
            dual_watch_set(false);
            ui_pop();
            return false;
        }
        settings_set_option(BIT_DW_TX_1, menu_tx == 1);
        settings_set_option(BIT_DW_TX_2, menu_tx == 2);
        settings_save_if_changed();
        menu_open = false;
    } else {
        return false;
    }
    return true;
}

static bool handle(const ui_event_t *ev)
{
    switch (ev->type) {
        case UI_EVENT_TIMER: {
            bool redraw = watch(systime_get_ms());

            ui_timer_start(DW_TICK_MS);
            return redraw && !menu_open;
        }
        case UI_EVENT_DATA:
            return !menu_open;
        case UI_EVENT_KEY_PRESS:
        case UI_EVENT_KEY_LONG:
        case UI_EVENT_KEY_REPEAT:
        case UI_EVENT_KEY_RELEASE:
            break;
        default:
            return false;
    }

    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_PTT)) {
        transmit();
        return false;
    }
    if (menu_open) {
        return handle_menu(ev);
    }
    if (ev->type == UI_EVENT_KEY_PRESS) {
        green_down |= (ev->key == UI_KEY_GREEN);
        red_down |= (ev->key == UI_KEY_RED);
    }
    if ((ev->type == UI_EVENT_KEY_LONG) && (ev->key == UI_KEY_GREEN) && green_down) {
        green_down = false;
        menu_open = true;
        menu_item = 0;
        menu_tx = (settings.bitfield_options & BIT_DW_TX_1) ? 1 : ((settings.bitfield_options & BIT_DW_TX_2) ? 2 : 0);
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_GREEN) && green_down) {
        green_down = false;
        ui_push(&list_screen, (void *)&main_menu);
        return false;
    }
    if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_RED) && red_down) {
        red_down = false;
        dual_watch_set(false); // back to the single watch
        ui_pop();
        return false;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN)) && !paths_same()) {
        bool upper = (ev->key == UI_KEY_UP); // the transmit path fixed; "follow" again in the menu

        settings_set_option(BIT_DW_TX_1, upper);
        settings_set_option(BIT_DW_TX_2, !upper);
        settings_save_if_changed();
        return true;
    }
    return false;
}

static void draw_menu(void)
{
    list_view_row_t row = {.icon = ICONS_COUNT};

    list_view_draw_title(text(TEXT_DUAL_WATCH), NULL);
    row.name = text(TEXT_DUAL_WATCH);
    row.value = text(TEXT_ON);
    row.selected = (menu_item == 0);
    list_view_draw_row(0, &row);
    row.name = text(TEXT_UI_DW_TX);
    row.value = (menu_tx == 0) ? text(TEXT_UI_DW_FOLLOW) : paths[menu_tx - 1].name;
    row.selected = (menu_item == 1);
    list_view_draw_row(1, &row);
    bottom_bar_draw_soft_keys(text(TEXT_UI_OK), text(TEXT_UI_BACK));
}

static void draw_panel(int i, int tx)
{
    const path_t *p = &paths[i];
    const codeplug_channel_t *ch = &p->channel;
    int32_t top = PANEL_TOP + (i * PANEL_HEIGHT);
    int32_t x = WIDGET_MARGIN + 4;
    int32_t lx = x;
    bool receiving = (i == current) && !paths_same() && ((state == STATE_RECEIVING) || (state == STATE_HANG));
    bool bright = receiving || (i == tx);
    int power = (ch->libre_dmr_power != 0) ? (ch->libre_dmr_power - 1) : settings.tx_power_level;
    char buffer[48];
    char detail[24];
    dmr_call_t call;

    if (paths_same() && (i == 1)) {
        return; // one path only
    }
    if (i == 1) {
        draw_hline(0, top - 1, FB_WIDTH, THEME_COLOUR_LINE);
    }
    if (receiving) {
        draw_fill_rect(0, top + 4, 5, PANEL_HEIGHT - 8, THEME_COLOUR_RX); // the path with a signal
    }

    // PTT (the transmit path), receiving, or channel / VFO; the mode and the power
    if (i == tx) {
        lx += draw_chip(lx, top + 8, 22, "PTT", &font_roboto17_bold, THEME_COLOUR_WHITE, THEME_COLOUR_ACCENT) + 6;
    }
    if (receiving) {
        draw_chip(lx, top + 8, 22, text(TEXT_UI_DW_RECEIVING), &font_roboto17_bold, THEME_COLOUR_BACKGROUND,
                  THEME_COLOUR_RX);
    } else if (p->vfo) {
        draw_text(lx, top + 8, "VFO", &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    } else {
        snprintf(buffer, sizeof(buffer), "%s %d", text(TEXT_APRS_CHANNEL), path_ref(i));
        draw_text(lx, top + 8, buffer, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    }
    snprintf(buffer, sizeof(buffer), "%s  %s", codeplug_mode_name(ch->mode),
             bottom_bar_power_label((power < 10) ? power : 9));
    draw_text(FB_WIDTH - WIDGET_MARGIN, top + 8, buffer, &font_roboto17, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT_LABEL);

    // The name, large
    draw_text(x, top + 32, p->name, &font_roboto32_bold, FONT_ALIGN_LEFT,
              (bright ? THEME_COLOUR_TEXT : THEME_COLOUR_TEXT_SECOND));

    // The frequency, then the tone, the TG, or who is talking
    snprintf(buffer, sizeof(buffer), "%lu.%04lu", (unsigned long)(ch->rx_freq / 100000),
             (unsigned long)((ch->rx_freq % 100000) / 10));
    detail[0] = '\0';
    if (ch->mode == CODEPLUG_MODE_DMR) {
        if (receiving && dmr_call(&call)) {
            snprintf(detail, sizeof(detail), "ID %lu", (unsigned long)call.source);
        } else {
            snprintf(detail, sizeof(detail), "TS%d  TG %lu", ((ch->flag2 & 0x40) ? 2 : 1),
                     (unsigned long)(p->talk_group & CODEPLUG_CONTACT_ID_MAX));
        }
    } else {
        uint16_t tone = (ch->rx_tone != CODEPLUG_CSS_NONE) ? ch->rx_tone : ch->tx_tone;

        if (tone != CODEPLUG_CSS_NONE) {
            tones_format(tone, detail, sizeof(detail));
        }
    }
    if (detail[0] != '\0') {
        size_t length = strlen(buffer);

        snprintf(buffer + length, sizeof(buffer) - length, "  %s", detail);
    }
    draw_text(x, top + 72, buffer, &font_roboto20, FONT_ALIGN_LEFT,
              (bright ? THEME_COLOUR_TEXT_SECOND : THEME_COLOUR_TEXT_DIM));

    if (receiving) {
        s_meter_data_t meter = {.compact = true, .active = true, .open = radio_is_receiving()};

        s_meter_set_signal(&meter, radio_signal());
        s_meter_draw(top + 100, &meter);
    }
}

static void draw(void)
{
    status_bar_data_t status = {0};

    if (menu_open) {
        draw_menu();
        return;
    }
    status.state = radio_is_receiving() ? STATUS_BAR_RECEIVING : STATUS_BAR_IDLE;
    status_bar_draw(&status);
    draw_panel(0, tx_path(systime_get_ms()));
    draw_panel(1, tx_path(systime_get_ms()));
    bottom_bar_draw_soft_keys(text(TEXT_MENU), text(TEXT_UI_BACK));
}

const ui_screen_t dual_watch_screen = {
    .enter = enter,
    .leave = leave,
    .handle = handle,
    .draw = draw,
};
