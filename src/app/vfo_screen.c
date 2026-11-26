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
 * OpenDMR, screen: the VFO screen of the main firmware (uiVFOMode.c drawVfoScreen() and handleEvent(),
 * docs/ui-design.md 5.2): "VFO A", the frequency the keys change (RX or TX) and the step; that frequency large (being
 * entered: the next digit underlined); the other frequency (set on its own: no offset); the tone or the contact; the S meter
 * (compact); the bottom bar. The VFO shown is the one the radio receives (radio.c); the PTT transmits (tx_screen.c).
 *
 * Keys (as the main firmware on the DM-32):
 *   UP / DOWN, rotary   the chosen frequency one step up / down (held: repeat); SK2 + UP / DOWN (keymap.c) chooses
 *                       the RX (upper) or the TX (lower) one: in white, its step digit underlined, the one digits enter
 *   0 - 9               frequency entry (8 digits, xxx.xxxxx MHz; not 0 first); then the left key or the 8th digit
 *                       sets it, LEFT deletes a digit, the right key cancels
 *   SK1 + SK2 held      shows the frequencies swapped (reverse repeater)
 *   left key            menu; held: VFO quick menu; SK2 + left key: channel details of the VFO
 *   right key           channel screen; held: the other VFO
 *   the shortcuts of keymap.c (SK2 + UP / DOWN the frequency changed, * timeslot or bandwidth, SK1 + * FM / DMR,
 *   LEFT / RIGHT TG or squelch, SK2 + LEFT / RIGHT power, # number entry ...)
 * The VFO goes to the radio with the timeslot and TG of tx_target.c. The screen doesn't tune the radio: it gives the VFO
 * (radio_channel()) when the UI task asks, after each event (ui_tune()), also while the menus are over it.
 * Scan (keymap.c, SK1 + UP; scan.c, uiVFOMode.c scanning()): the RX frequency in VFO steps from the VFO's low to high
 * scan limit (settings, the quick menu sets them), round again; without limits the band of the frequency. Shown on
 * the TX line while it scans; * on a paused frequency skips it for this scan.
 * The left and right keys act on their release, and only when their press came to this screen (a list that was left
 * with the press of a key doesn't open the menu here with its release).
 *
 * The VFOs are channels of the codeplug (codeplug_write.c), kept in RAM (vfo_screen_vfo()). Changes on this screen
 * are written at power off, the one place they are written (vfo_power_off_save()); the channel details and the quick
 * menu write a VFO at once (vfo_screen_save()).
 */

#include <stdio.h>
#include <string.h>
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "icons.h"
#include "text.h"
#include "settings.h"
#include "codeplug.h"
#include "codeplug_text.h"
#include "battery.h"
#include "datetime.h"
#include "list_screen.h"
#include "tones.h"
#include "menus.h"
#include "channel_screen.h"
#include "channel_details.h"
#include "quick_menu.h"
#include "vfo_screen.h"
#include "power.h"
#include "lock_screen.h"
#include "radio.h"
#include "tx_screen.h"
#include "sound.h"
#include "util.h"
#include "dmr.h"
#include "qso_panel.h"
#include "keymap.h"
#include "quick_keys.h"
#include "tx_target.h"
#include "number_entry.h"
#include "fm_radio.h"
#include "scan.h"
#include "dual_watch.h"
#include "contacts.h"
#include "aprs.h"

#define FREQ_DIGITS     8 // xxx.xxxxx MHz: the frequency in 10 Hz
#define FREQ_DOT_AFTER  3
#define POWER_LEVEL_MAX 8 // 10 W; 9 is the user power ("+W-")
#define POWER_USER      9
#define SQUELCH_MIN     1 // codeplug sql: 0 the band default, 1 .. 21: 0 .. 100 %
#define SQUELCH_MAX     21
#define SQUELCH_PERCENT_STEP 5

#define FLAG2_TIMESLOT_2       0x40
#define FLAG4_BANDWIDTH_25K    0x02
#define VFO_STEP_SHIFT         4

// Layout of drawVfoScreen(), LCD pixels
#define TOP_Y       44
#define CHIP_Y      42
#define CHIP_HEIGHT 22
#define RX_Y        66 // roboto42_bold
#define FREQ_CHIP_H   19 // the RX / TX label on the right of a frequency
#define FREQ_CHIP_PAD 4
#define TX_Y        118
#define LINE_Y      174
#define TONE_Y      184 // DMR: the TG; FM: the tone, the squelch on the right
#define S_METER_Y   250
#define VALUE_X     (WIDGET_MARGIN + 70)
#define LABEL_GAP   10

const uint16_t vfo_screen_steps[VFO_SCREEN_STEPS] = {250, 500, 625, 1000, 1250, 2500, 3000, 5000};
#define SSB_STEP 10 // USB, LSB: the knob tunes 100 Hz a click whatever the VFO's step

static unsigned step_of(const codeplug_channel_t *v)
{
    return codeplug_mode_is_ssb(v->mode) ? SSB_STEP : vfo_screen_steps[(v->vfo_flag5 >> VFO_STEP_SHIFT) % VFO_SCREEN_STEPS];
}

// The bands of the radio (codeplug_frequency_is_valid()), to go from the end of one to the next with the knob
static const uint32_t band_edges[][2] = {
    {10800000, 13699750}, {13700000, 18000000}, {20000000, 27997500}, {28000000, 52000000}};

static codeplug_channel_t vfos[2];  // 2 x 64 bytes
static codeplug_channel_t radio_ch; // the channel for the radio: the VFO with its TG (tx_target_apply())
static tx_target_t target;          // its TG (DMR)
static bool loaded[2];
static bool home_is_vfo = false;
static bool dirty[2]; // a VFO changed here, for the power off

static bool editing_tx = false; // the TX frequency was stepped last: shown large, digits enter it

static scan_t scan = {.swallow_key = UI_KEY_COUNT};
static digit_entry_t entry = {
    .max = FREQ_DIGITS, .dot_after = FREQ_DOT_AFTER, .zero_fill = true, .ok_on_release = true};
static uint8_t held = 0; // UI_MODIFIER_x of the last key or rotary event
static bool green_down = false;
static bool red_down = false;

// What the S meter shows: nothing until there is a receiver (plug the RSSI in here)
static void vfo_screen_signal(s_meter_data_t *meter)
{
    s_meter_set_signal(meter, radio_signal_dbm());
    meter->open = radio_is_receiving();
}

bool vfo_screen_draw_entry(int32_t x, int32_t y, const font_t *font)
{
    if (!digit_entry_active(&entry)) {
        return false;
    }
    (void)digit_entry_draw_font(&entry, x, y, font);
    return true;
}

bool vfo_screen_editing_tx(void)
{
    return editing_tx;
}

bool vfo_screen_typing(void)
{
    return digit_entry_active(&entry);
}

int vfo_screen_current(void)
{
    return (settings.current_vfo_number == 1) ? 1 : 0;
}

static void vfo_power_off_save(void);
static void changed(void);

codeplug_channel_t *vfo_screen_vfo(int vfo)
{
    vfo = (vfo == 1) ? 1 : 0;
    if (!loaded[vfo]) {
        power_off_save_register(vfo_power_off_save);
        codeplug_read_vfo(vfo, &vfos[vfo]);
        if (vfos[vfo].libre_dmr_power == 0) {
            vfos[vfo].libre_dmr_power = (uint8_t)(settings.tx_power_level + 1); // its own from the default power on
        }
        loaded[vfo] = true;
    }
    return &vfos[vfo];
}

// Power To All (codeplug_set_all_power() wrote it in the codeplug): the VFOs in RAM take it too, keeping their other
// changes for the power off
void vfo_screen_set_power(uint8_t power)
{
    for (int v = 0; v < 2; v++) {
        if (loaded[v]) {
            vfos[v].libre_dmr_power = power;
        }
    }
}

void vfo_screen_save(int vfo)
{
    vfo = (vfo == 1) ? 1 : 0;
    codeplug_write_vfo(vfo, vfo_screen_vfo(vfo));
    dirty[vfo] = false;
}

// At power off (power_off_save_register()): the VFOs changed here
static void vfo_power_off_save(void)
{
    for (int v = 0; v < 2; v++) {
        if (dirty[v] && loaded[v]) {
            dirty[v] = false;
            codeplug_write_vfo(v, &vfos[v]);
        }
    }
}

static codeplug_channel_t *vfo(void)
{
    return vfo_screen_vfo(vfo_screen_current());
}

unsigned vfo_screen_step(void)
{
    return step_of(vfo());
}

static void compute_radio_channel(void)
{
    memcpy(&radio_ch, vfo(), sizeof(radio_ch)); // with the VFO's own power (0: the radio's), as vfo_power() shows
    tx_target_apply(&radio_ch, &target);
}

// The channel of the screen (ui_screen_t.channel): asked after each event, the same again changes nothing there
static const codeplug_channel_t *radio_channel(void)
{
    compute_radio_channel();
    return &radio_ch;
}

/* Tone scan (uiVFOMode.c toneScan(), started from the quick menu): the receive tone steps through the tones of the
   type chosen until the squelch opens with one (the speaker on), which becomes the transmit tone too; 200 ms a tone (a
   CTCSS tone 2 ms less a step up the list). A key stops it. */

#define TONE_SCAN_MS 200

static struct {
    bool active;
    int first; // positions of tones.c
    int last;
    int position;
} tone_scan;

static bool tone_in_type(uint16_t tone, int type) // type: 0 all, 1 CTCSS, 2 DCS, 3 inverted DCS
{
    bool dcs = (tone & TONES_DCS) != 0;
    bool inverted = (tone & TONES_DCS_INVERTED) != 0;

    return (type == 0) || ((type == 1) && !dcs) || ((type == 2) && dcs && !inverted) || ((type == 3) && inverted);
}

static void tone_scan_set(void)
{
    uint16_t tone = tones_at(tone_scan.position);

    vfo()->rx_tone = tone;
    ui_timer_start((tone & TONES_DCS) ? TONE_SCAN_MS : (uint32_t)(TONE_SCAN_MS - ((tone_scan.position - 1) * 2)));
}

void vfo_screen_tone_scan(int type)
{
    int current = tones_position(vfo()->rx_tone);

    tone_scan.first = 0;
    tone_scan.last = 0;
    for (int p = 1; p < tones_count(); p++) {
        if (tone_in_type(tones_at(p), type)) {
            tone_scan.first = (tone_scan.first == 0) ? p : tone_scan.first;
            tone_scan.last = p;
        }
    }
    if (tone_scan.first == 0) {
        return;
    }
    // from the tone after the current one when it is of the type, else from the first
    tone_scan.position = ((current >= tone_scan.first) && (current < tone_scan.last)) ? (current + 1) : tone_scan.first;
    tone_scan.active = true;
    settings.analog_filter_level = 1; // trxSetAnalogFilterLevel(ANALOG_FILTER_CSS): the tone opens the squelch
    tone_scan_set();
}

// The timer while scanning: found (the speaker on), else the next tone
static bool tone_scan_step(void)
{
    if (radio_audio_heard()) {
        vfo()->tx_tone = vfo()->rx_tone;
        tone_scan.active = false;
        changed();
        sound_play(SOUND_ACK);
        return true;
    }
    tone_scan.position = (tone_scan.position >= tone_scan.last) ? tone_scan.first : (tone_scan.position + 1);
    tone_scan_set();
    return true;
}

static void changed(void)
{
    dirty[vfo_screen_current()] = true;
}

static void set_home_mode(bool is_vfo)
{
    uint8_t number = (is_vfo ? VFO_SCREEN_MENU_VFO : VFO_SCREEN_MENU_CHANNEL);

    home_is_vfo = is_vfo;
    if (settings.initial_menu_number != number) {
        settings.initial_menu_number = number; // saved with the next settings save
        settings_changed();
    }
}

bool vfo_screen_is_home(void)
{
    return home_is_vfo;
}

// From the channel screen: the changes made there with the keys are gone (channel_screen_forget_changes())
void vfo_screen_show(void)
{
    ui_home();
    channel_screen_forget_changes();
    ui_replace(&vfo_screen, NULL);
}

// To the channel screen: the channel as it is in the codeplug, without a TG or timeslot picked on the VFO
void vfo_screen_leave(void)
{
    ui_home(); // from the quick menu (VFO --> New Chan): back on this screen, whose enter() sets the VFO home
    set_home_mode(false);
    channel_screen_forget_changes();
    ui_replace(&channel_screen, NULL);
}

// stepFrequency(): past the end of a band, to the start of the next one (or the end of the one before). The RX and TX
// frequencies are set each on its own (OpenGD77 moves TX with RX)
static void step_frequency(int steps, bool tx_frequency)
{
    codeplug_channel_t *v = vfo();
    int32_t increment = steps * (int32_t)step_of(v);
    uint32_t rx = v->rx_freq;
    uint32_t tx = v->tx_freq;

    editing_tx = tx_frequency;
    if (editing_tx) {
        tx += increment;
        if (!codeplug_frequency_is_valid(tx)) {
            return;
        }
    } else {
        rx += increment;
        if (!codeplug_frequency_is_valid(rx)) {
            int count = (int)(sizeof(band_edges) / sizeof(band_edges[0]));
            int band = -1;

            for (int i = 0; i < count; i++) {
                if ((increment > 0) ? ((band < 0) && (band_edges[i][0] > rx)) : (band_edges[i][1] < rx)) {
                    band = i;
                }
            }
            if (band < 0) {
                return;
            }
            rx = band_edges[band][(increment > 0) ? 0 : 1];
        }
    }
    v->rx_freq = rx;
    v->tx_freq = tx;
    changed();
}

// updateFrequency(): the entered frequency, RX or TX alone
static bool set_frequency(uint32_t frequency)
{
    codeplug_channel_t *v = vfo();

    if (!codeplug_frequency_is_valid(frequency)) {
        return false;
    }
    if (editing_tx) {
        v->tx_freq = frequency;
    } else {
        v->rx_freq = frequency;
    }
    changed();
    return true;
}

// A key while a frequency is being entered (or a digit that starts one); true: handled
static bool handle_entry(const ui_event_t *ev)
{
    int digit = ui_key_digit(ev->key);
    uint8_t sides = (UI_MODIFIER_SK1 | UI_MODIFIER_SK2);

    if (!digit_entry_active(&entry)) {
        if ((ev->type != UI_EVENT_KEY_PRESS) || (digit <= 0) || (held & sides)) { // nothing under 100 MHz
            return false;
        }
    } else if ((digit >= 0) && (held & sides)) {
        return true;
    } else if ((ev->type == UI_EVENT_KEY_RELEASE) &&
               (((ev->key == UI_KEY_RED) && !red_down) || ((ev->key == UI_KEY_GREEN) && !green_down))) {
        return true; // its press was before the entry
    }
    if (ev->type == UI_EVENT_KEY_RELEASE) {
        red_down = red_down && (ev->key != UI_KEY_RED);
        green_down = green_down && (ev->key != UI_KEY_GREEN);
    }
    if (digit_entry_handle(&entry, ev, set_frequency)) {
        return true;
    }
    return digit_entry_active(&entry) && (ev->key != UI_KEY_SK1) && (ev->key != UI_KEY_SK2); // no other key
}

/* Scan */

// The VFO's scan limits; none set (or not valid): the band of the RX frequency
static void scan_range(uint32_t *low, uint32_t *high)
{
    int n = vfo_screen_current();
    uint32_t rx = vfo()->rx_freq;

    *low = settings.vfo_scan_low[n];
    *high = settings.vfo_scan_high[n];
    if ((*low < *high) && codeplug_frequency_is_valid(*low) && codeplug_frequency_is_valid(*high)) {
        return;
    }
    *low = rx;
    *high = rx;
    for (unsigned i = 0; i < (sizeof(band_edges) / sizeof(band_edges[0])); i++) {
        if ((rx >= band_edges[i][0]) && (rx <= band_edges[i][1])) {
            *low = band_edges[i][0];
            *high = band_edges[i][1];
        }
    }
}

static scan_kind_t scan_kind(void)
{
    return (vfo()->mode == CODEPLUG_MODE_DMR) ? SCAN_DMR : (vfo()->mode == CODEPLUG_MODE_P25) ? SCAN_P25
         : (vfo()->mode == CODEPLUG_MODE_NXDN) ? SCAN_NXDN
         : (vfo()->mode == CODEPLUG_MODE_YSF) ? SCAN_YSF : SCAN_ANALOG;
}

static uint32_t scan_dwell(void)
{
    return scan_dwell_ms(scan_kind());
}

int vfo_screen_scanning(void)
{
    return scan_on_signal(&scan) ? 2 : (scan.active ? 1 : 0);
}

// Skipped with * (nuisance delete, scan_skipped_frequencies): until cleared in the quick menu or power off
static bool scan_skipped(uint32_t frequency)
{
    return scan_skipped_has(&scan_skipped_frequencies, frequency) || scan_saved_has(frequency);
}

// One VFO step on in the scan direction, round within the limits (skipped frequencies left out); 0: none left
static uint32_t scan_next(void)
{
    codeplug_channel_t *v = vfo();
    uint32_t step = step_of(v);
    uint32_t low;
    uint32_t high;
    uint32_t f = v->rx_freq;

    scan_range(&low, &high);
    for (int i = 0; i <= SCAN_SKIPPED_MAX; i++) {
        if (scan.direction > 0) {
            f = ((f + step) > high) ? low : (f + step);
        } else {
            f = ((f < (low + step)) || ((f - step) < low)) ? high : (f - step);
        }
        if (!scan_skipped(f)) {
            return f;
        }
    }
    return 0;
}

static void scan_begin(int direction)
{
    uint32_t low;
    uint32_t high;
    codeplug_channel_t *v = vfo();

    scan_range(&low, &high);
    if (low >= high) {
        ui_key_beep(SOUND_ERROR);
        return;
    }
    digit_entry_stop(&entry);
    editing_tx = false;
    if ((v->rx_freq < low) || (v->rx_freq > high)) {
        v->rx_freq = (direction > 0) ? low : high; // into the range
        changed();
    }
    scan_start(&scan, scan_dwell(), direction);
}

// On the timer while scanning; true: redraw. The VFO is saved when the scan ends, not at each step
static bool scan_step(void)
{
    bool was_active = scan.active;
    uint32_t f;

    if (!scan_tick(&scan, scan_kind())) {
        return was_active && !scan.active;
    }
    f = scan_next();
    if (f == 0) {
        scan_stop(&scan);
        return true;
    }
    vfo()->rx_freq = f;
    changed();
    scan_moved(&scan, scan_dwell());
    return true;
}

static void switch_vfo(void)
{
    settings.current_vfo_number = (uint8_t)(1 - vfo_screen_current());
    settings_changed();
    editing_tx = false;
}

// The first choice of each gets the first item beep (uiVFOMode.c): FM, TS1, 25 kHz. The mode: FM, AM, USB, LSB, DMR
static void toggle_mode(void)
{
    codeplug_channel_t *v = vfo();

    v->mode = codeplug_mode_step(v->mode, 1);
    ui_key_beep((v->mode == CODEPLUG_MODE_FM) ? SOUND_KEY_FIRST_ITEM : SOUND_KEY);
    changed();
}

// DMR: the timeslot, kept as the manual override (tx_target.c); FM: the bandwidth
static void toggle_ts_or_bandwidth(void)
{
    codeplug_channel_t *v = vfo();

    if (v->mode == CODEPLUG_MODE_DMR) {
        tx_target_toggle_timeslot(v);
        compute_radio_channel();
        ui_key_beep((target.timeslot == 2) ? SOUND_KEY_FIRST_ITEM : SOUND_KEY); // back to TS1
        return;
    }
    if (v->mode != CODEPLUG_MODE_FM) {
        return; // AM and SSB have their own filters
    }
    v->flag4 ^= FLAG4_BANDWIDTH_25K;
    ui_key_beep((v->flag4 & FLAG4_BANDWIDTH_25K) ? SOUND_KEY_FIRST_ITEM : SOUND_KEY);
    changed();
}

// The power of a VFO: its own (libre_dmr_power 1 - 10, kept with it), else (0, as written before) the radio's
static int vfo_power(const codeplug_channel_t *v)
{
    int level = (v->libre_dmr_power != 0) ? (v->libre_dmr_power - 1) : settings.tx_power_level;

    return (level <= POWER_USER) ? level : POWER_USER;
}

// Each VFO its own power: A and B apart
static void change_power(int direction, bool to_user)
{
    codeplug_channel_t *v = vfo();
    int level = vfo_power(v);

    if (to_user) {
        level = POWER_USER;
    } else if (direction > 0) {
        level = (level < POWER_LEVEL_MAX) ? (level + 1) : level;
    } else {
        level = (level > 0) ? (((level > POWER_LEVEL_MAX) ? POWER_LEVEL_MAX : level) - 1) : 0;
    }
    if (level != vfo_power(v) || (v->libre_dmr_power == 0)) {
        v->libre_dmr_power = (uint8_t)(level + 1);
        changed();
    }
    if (level == 0) {
        ui_key_beep(SOUND_KEY_FIRST_ITEM); // the lowest power (uiChannelMode.c / uiVFOMode.c)
    }
}

// FM, AM, USB, LSB (the squelch, DTMF entry); else DMR
static bool v_is_fm(void)
{
    return codeplug_mode_is_analog(vfo()->mode);
}

// uiVFOMode.c: from the band's default when the VFO has none (0), then its own (1 .. 21: 0 .. 100 %)
static void change_squelch(int direction)
{
    codeplug_channel_t *v = vfo();
    int sql = (v->sql != 0) ? v->sql : settings.squelch_defaults[radio_band_of(v->rx_freq)];

    v->sql = (uint8_t)CLAMP(sql + direction, SQUELCH_MIN, SQUELCH_MAX);
    if (v->sql == SQUELCH_MIN) {
        ui_key_beep(SOUND_KEY_FIRST_ITEM);
    }
    changed();
}

static void enter(void *arg)
{
    (void)arg;
    set_home_mode(true);
    green_down = red_down = false;
    held = 0;
    (void)dual_watch_show(); // dual watch on: shown over this screen, its selected path this VFO
}

static void leave(void)
{
    scan_stop(&scan);
    digit_entry_stop(&entry);
}

// SK2 while a call is shown: transmit to its TG (a private call: to the caller) on its timeslot
static void reply_to_call(void)
{
    dmr_call_t call;

    if (v_is_fm() || !dmr_call(&call)) {
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
    switch (action) {
        case KEYMAP_NUMBER_ENTRY:
            ui_push(&number_entry_screen, (!v_is_fm()) ? NUMBER_ENTRY_TX_TARGET : NUMBER_ENTRY_DTMF);
            return false;
        case KEYMAP_CONTACT_LIST:
            contacts_open_quick_list(v_is_fm());
            return false;
        case KEYMAP_TS_OR_BANDWIDTH:
            toggle_ts_or_bandwidth();
            return true;
        case KEYMAP_TS_CLEAR:
            if (!v_is_fm()) {
                tx_target_clear_timeslot();
            }
            return true;
        case KEYMAP_MODE:
            toggle_mode();
            return true;
        case KEYMAP_TG_OR_SQUELCH_UP:
        case KEYMAP_TG_OR_SQUELCH_DOWN: {
            int step = (action == KEYMAP_TG_OR_SQUELCH_UP) ? 1 : -1;

            if (v_is_fm()) {
                change_squelch(step);
            } else if (tx_target_step_group(vfo(), step) == 0) {
                ui_key_beep(SOUND_KEY_FIRST_ITEM);
            }
            return true;
        }
        case KEYMAP_POWER_UP:
        case KEYMAP_POWER_DOWN:
            change_power(((action == KEYMAP_POWER_UP) ? 1 : -1), false);
            return true;
        case KEYMAP_POWER_USER:
            change_power(1, true);
            return true;
        case KEYMAP_TX_UP: // SK2 + up / down: the RX (upper) or the TX (lower) frequency is the one changed
        case KEYMAP_TX_DOWN:
            editing_tx = (action == KEYMAP_TX_DOWN);
            return true;
        case KEYMAP_TG_FROM_CALL:
            reply_to_call();
            return true;
        case KEYMAP_SCAN:
        case KEYMAP_SCAN_DOWN:
            scan_begin((action == KEYMAP_SCAN) ? 1 : -1);
            return true;
        case KEYMAP_APRS_TOGGLE:
            aprs_toggle();
            return true;
        case KEYMAP_APRS_SEND:
            aprs_send();
            return true;
        case KEYMAP_FM_RADIO:
            ui_push(&fm_radio_screen, NULL);
            return false;
        case KEYMAP_LOCK:
            lock_screen_lock(true, lock_screen_is_ptt_locked());
            return true;
        default:
            return false;
    }
}

static bool handle_key(const ui_event_t *ev)
{
    keymap_action_t action = keymap_action(ev, KEYMAP_SCREEN_VFO);
    bool press = (ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT);

    if (ev->type == UI_EVENT_KEY_PRESS) {
        green_down |= (ev->key == UI_KEY_GREEN);
        red_down |= (ev->key == UI_KEY_RED);
    }

    if (handle_entry(ev)) {
        return true;
    }
    if (action != KEYMAP_NONE) {
        return do_action(action);
    }
    if (quick_keys_home(ev)) {
        return true;
    }

    switch (ev->key) {
        case UI_KEY_SK1:
        case UI_KEY_SK2:
            return true; // SK1 + SK2: the frequencies swapped while held
        case UI_KEY_UP:
        case UI_KEY_DOWN:
            if (!press || (held & (UI_MODIFIER_SK1 | UI_MODIFIER_SK2))) {
                return false;
            }
            step_frequency(((ev->key == UI_KEY_UP) ? 1 : -1), editing_tx); // the one chosen (SK2 + up / down)
            return true;
        case UI_KEY_GREEN:
            // held: the main menu; a press: the menu of the VFO (with SK2: its details)
            if ((ev->type == UI_EVENT_KEY_LONG) && green_down) {
                green_down = false;
                ui_push(&list_screen, (void *)&main_menu);
            } else if ((ev->type == UI_EVENT_KEY_RELEASE) && green_down) {
                green_down = false;
                if (ev->modifiers & UI_MODIFIER_SK2) {
                    ui_push(&channel_details_screen, NULL);
                } else {
                    quick_menu_open_vfo();
                }
            }
            return false;
        case UI_KEY_RED:
            if ((ev->type == UI_EVENT_KEY_LONG) && red_down) {
                red_down = false;
                switch_vfo();
                return true;
            }
            if ((ev->type == UI_EVENT_KEY_RELEASE) && red_down) {
                red_down = false;
                vfo_screen_leave();
            }
            return false;
        default:
            break;
    }
    return false;
}

static bool handle_event(const ui_event_t *ev)
{
    switch (ev->type) {
        case UI_EVENT_KEY_PRESS:
        case UI_EVENT_KEY_LONG:
        case UI_EVENT_KEY_REPEAT:
        case UI_EVENT_KEY_RELEASE: {
            held = ev->modifiers;
            switch (scan_key(&scan, ev)) {
                case SCAN_KEY_SKIP:
                    ui_key_beep(scan_skipped_add(&scan_skipped_frequencies, vfo()->rx_freq) ? SOUND_KEY : SOUND_ERROR);
                    (void)keymap_action(ev, KEYMAP_SCREEN_VFO);
                    return true;
                case SCAN_KEY_SKIP_SAVED:
                    ui_key_beep(scan_saved_add(vfo()->rx_freq) ? SOUND_ACK : SOUND_ERROR);
                    // fall through
                case SCAN_KEY_USED:
                case SCAN_KEY_STOPPED:
                    (void)keymap_action(ev, KEYMAP_SCREEN_VFO); // keeps the side key state
                    return true;
                default:
                    break;
            }
            return handle_key(ev);
        }
        case UI_EVENT_ROTARY:
            held = ev->modifiers;
            scan_stop(&scan);
            if (digit_entry_active(&entry)) {
                return false;
            }
            step_frequency(ev->steps, editing_tx); // the one chosen (SK2 + up / down)
            return true;
        case UI_EVENT_TIMER:
            if (tone_scan.active) {
                return tone_scan_step();
            }
            if (scan.active) {
                bool redraw = scan_step();

                if (scan.active) {
                    ui_timer_start(SCAN_TICK_MS);
                }
                return redraw;
            }
            return false;
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

// The PTT transmits (a frequency being entered is dropped)
static bool handle(const ui_event_t *ev)
{
    bool redraw;

    if (lock_screen_filter(ev)) {
        return false;
    }
    if (tone_scan.active && ((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_ROTARY))) {
        tone_scan.active = false; // a key stops it, on the tone it was at
        changed();
        if (ev->key != UI_KEY_PTT) {
            return true;
        }
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_PTT)) {
        char name[16];

        scan_stop(&scan);
        digit_entry_stop(&entry);
        snprintf(name, sizeof(name), "VFO %c", (vfo_screen_current() == 0) ? 'A' : 'B');
        tx_screen_open("VFO", name, &radio_ch);
        return false;
    }
    redraw = handle_event(ev);
    return redraw;
}

static void draw_info_line(int32_t y, const char *label, const char *value)
{
    draw_text(WIDGET_MARGIN, y, label, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    draw_text(VALUE_X, y, value, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_SECOND);
}

static void draw_status_bar(const codeplug_channel_t *v)
{
    status_bar_data_t status = {0};
    s_meter_data_t meter = {0};

    vfo_screen_signal(&meter);
    status.state = meter.open ? STATUS_BAR_RECEIVING : STATUS_BAR_IDLE;
    status.signal_bars = s_meter_bars(&meter);
    status.scanning = scan.active || tone_scan.active;
    status_bar_draw(&status); // DMR's timeslot and colour code: in the bottom bar (draw_bottom())
}

// The power of the bottom bar; "Rx only" when the VFO can't transmit (AM, SSB, the aviation band)
static const char *power_text(const codeplug_channel_t *v)
{
    return (codeplug_mode_can_transmit(v->mode) && codeplug_frequency_can_transmit(v->tx_freq))
               ? bottom_bar_power_label(vfo_power(v))
               : text(TEXT_RX_ONLY);
}

// The digits a step changes, a ^ under each (rows high, its rows 1, 3, 5 ... pixels wide; green RX, red TX): from the largest place of the step to its
// last digit that is not 0 (xxx.xxxxx in 10 Hz units: 12.5 kHz three digits, 25 kHz two, 6.25 kHz three)
void vfo_screen_draw_step_marks(int32_t x, int32_t y, const char *frequency, const font_t *font, unsigned step, bool tx,
                                int rows)
{
    unsigned high = 1; // the largest place not over the step
    unsigned low = 1;  // the last place of the step that is not 0
    int first = 5;     // their decimals
    int last = 5;
    const char *dot = strchr(frequency, '.');
    char part[16];

    if ((step == 0) || !dot) {
        return;
    }
    while (((high * 10) <= step) && (first > 1)) {
        high *= 10;
        first--;
    }
    while (((step % (low * 10)) == 0) && (last > first)) {
        low *= 10;
        last--;
    }
    size_t from = (size_t)(dot - frequency) + (size_t)first;
    size_t to = (size_t)(dot - frequency) + (size_t)last + 1;
    int32_t room = font->line_height - font->ascent; // under the baseline
    int32_t top = y + font->ascent + ((room > rows) ? ((room - rows) / 2) : 0);

    for (size_t at = from; at < to; at++) {
        memcpy(part, frequency, at);
        part[at] = '\0';
        int32_t start = x + font_text_width(font, part);

        part[at] = frequency[at];
        part[at + 1] = '\0';
        int32_t middle = (start + x + font_text_width(font, part)) / 2;

        for (int row = 0; row < rows; row++) {
            draw_fill_rect(middle - row, top + row, (2 * row) + 1, 1, (tx ? THEME_COLOUR_TX : THEME_COLOUR_RX));
        }
    }
}

// A frequency line: the frequency large (the digits typed while entering it), then on the right its label ("RX" on
// green, "TX" on red) at the digits' top and "MHz" at their baseline. The one the keys change: white, the digits
// of its step marked (^).
static void draw_frequency(int32_t y, bool is_tx, uint32_t frequency, unsigned step)
{
    const font_t *font = &font_roboto42_bold;
    bool selected = (editing_tx == is_tx);
    const char *label = text(is_tx ? TEXT_UI_TX : TEXT_UI_RX);
    char buffer[16];
    int32_t x = WIDGET_MARGIN;
    int32_t right = FB_WIDTH - WIDGET_MARGIN;
    int32_t chip_w = font_text_width(&font_roboto17_bold, label) + (2 * FREQ_CHIP_PAD);

    // the chip from a little over the digits' top (their cap height about 0.72 of the size: 30 px), clear of "MHz"
    draw_fill_rect(right - chip_w, y + (font->ascent - 30) - 5, chip_w, FREQ_CHIP_H,
                   (is_tx ? THEME_COLOUR_TX : THEME_COLOUR_RX));
    draw_text(right - FREQ_CHIP_PAD, y + (font->ascent - 30) - 5 + ((FREQ_CHIP_H - font_roboto17_bold.line_height) / 2),
              label, &font_roboto17_bold, FONT_ALIGN_RIGHT, (is_tx ? THEME_COLOUR_WHITE : THEME_COLOUR_BACKGROUND));
    draw_text(right, y + font->ascent - font_roboto17.ascent, "MHz", &font_roboto17, FONT_ALIGN_RIGHT,
              THEME_COLOUR_TEXT_LABEL);
    digit_entry_format_frequency(frequency, buffer, sizeof(buffer));
    if (selected && digit_entry_active(&entry)) {
        (void)digit_entry_draw_font(&entry, x, y, font); // its own cursor
        return;
    }
    draw_text(x, y, buffer, font, FONT_ALIGN_LEFT, (selected ? THEME_COLOUR_TEXT : THEME_COLOUR_TEXT_LABEL));

    // The one the keys and the knob change: the digits of its step marked
    if (selected) {
        vfo_screen_draw_step_marks(x, y, buffer, font, step, is_tx, 7);
    }
}

static void draw(void)
{
    const codeplug_channel_t *v = vfo();
    bool swapped = ((held & (UI_MODIFIER_SK1 | UI_MODIFIER_SK2)) == (UI_MODIFIER_SK1 | UI_MODIFIER_SK2));
    uint32_t rx = (swapped ? v->tx_freq : v->rx_freq);
    uint32_t tx = (swapped ? v->rx_freq : v->tx_freq);
    unsigned step = step_of(v);
    char buffer[40];
    dmr_call_t call;
    p25_call_t p25;
    nxdn_call_t nxdn;
    ysf_call_t ysf;
    m17_call_t m17;

    if ((v->mode == CODEPLUG_MODE_DMR) && !digit_entry_active(&entry) && dmr_call(&call)) {
        snprintf(buffer, sizeof(buffer), "VFO %c", (vfo_screen_current() == 0) ? 'A' : 'B');
        qso_panel_draw(&call, buffer, codeplug_colour_code(v), power_text(v));
        return;
    }
    if ((v->mode == CODEPLUG_MODE_P25) && !digit_entry_active(&entry) && p25_radio_call(&p25) && (p25.src != 0)) {
        snprintf(buffer, sizeof(buffer), "VFO %c", (vfo_screen_current() == 0) ? 'A' : 'B');
        qso_panel_draw_p25(&p25, buffer, power_text(v));
        return;
    }
    if ((v->mode == CODEPLUG_MODE_NXDN) && !digit_entry_active(&entry) && nxdn_radio_call(&nxdn) && (nxdn.src != 0)) {
        snprintf(buffer, sizeof(buffer), "VFO %c", (vfo_screen_current() == 0) ? 'A' : 'B');
        qso_panel_draw_nxdn(&nxdn, buffer, power_text(v));
        return;
    }
    if ((v->mode == CODEPLUG_MODE_M17) && !digit_entry_active(&entry) && m17_radio_call(&m17) && m17.src[0]) {
        snprintf(buffer, sizeof(buffer), "VFO %c", (vfo_screen_current() == 0) ? 'A' : 'B');
        qso_panel_draw_m17(&m17, buffer, power_text(v));
        return;
    }
    if ((v->mode == CODEPLUG_MODE_YSF) && !digit_entry_active(&entry) && ysf_radio_call(&ysf)) {
        snprintf(buffer, sizeof(buffer), "VFO %c", (vfo_screen_current() == 0) ? 'A' : 'B');
        qso_panel_draw_ysf(&ysf, buffer, power_text(v));
        return;
    }
    draw_status_bar(v);

    // Which VFO, the step
    snprintf(buffer, sizeof(buffer), "VFO %c", (vfo_screen_current() == 0) ? 'A' : 'B');
    font_draw_text(WIDGET_MARGIN, TOP_Y, buffer, &font_roboto17_bold, FONT_ALIGN_LEFT, theme_colour(THEME_COLOUR_TEXT));
    snprintf(buffer, sizeof(buffer), "%s %u.%uk", text(TEXT_STEPFREQ), step / 100, (step % 100) / 10);
    draw_text(FB_WIDTH - WIDGET_MARGIN, TOP_Y, buffer, &font_roboto17, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT_LABEL);

    // The receive and the transmit frequencies, as large; the one the keys change white, its step digit underlined. While
    // scanning, the range instead of the transmit one.
    draw_frequency(RX_Y, false, rx, step);
    if (scan.active) {
        uint32_t low;
        uint32_t high;

        scan_range(&low, &high);
        snprintf(buffer, sizeof(buffer), "%lu.%03lu - %lu.%03lu", (unsigned long)(low / 100000),
                 (unsigned long)((low % 100000) / 100), (unsigned long)(high / 100000),
                 (unsigned long)((high % 100000) / 100));
        draw_info_line(TX_Y + 10, text(TEXT_SCAN), buffer);
        if (scan_skipped_frequencies.count + scan_saved_count()) { // nuisance delete: how many are left out
            snprintf(buffer, sizeof(buffer), "%u", (unsigned)(scan_skipped_frequencies.count + scan_saved_count()));
            draw_info_line(TX_Y + 34, text(TEXT_SCAN_SKIPPED), buffer);
        }
    } else {
        draw_frequency(TX_Y, true, tx, step);
    }

    draw_hline(WIDGET_MARGIN, LINE_Y, FB_WIDTH - (2 * WIDGET_MARGIN), THEME_COLOUR_LINE);

    // The TG or contact (DMR); the squelch and the tone (FM), a line each as the TX frequency
    if (v->mode == CODEPLUG_MODE_DMR) {
        if (target.id != 0) {
            bool pc = ((target.id >> 24) == TX_TARGET_PC_FLAG);

            icons_draw(WIDGET_MARGIN, TONE_Y + 3, (pc ? ICONS_CONTACT : ICONS_TALKGROUP), THEME_COLOUR_TEXT_LABEL);
            draw_text(VALUE_X, TONE_Y, target.name, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
        }
    } else if (v->mode == CODEPLUG_MODE_NXDN) {
        // NXDN: as P25, the group or unit transmitted to (tx_target_apply()), the RAN on its line, right
        bool pc = ((target.id >> 24) == TX_TARGET_PC_FLAG);

        icons_draw(WIDGET_MARGIN, TONE_Y + 3, (pc ? ICONS_CONTACT : ICONS_TALKGROUP), THEME_COLOUR_TEXT_LABEL);
        if (target.id != 0) {
            draw_text(VALUE_X, TONE_Y, target.name, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
        }
        snprintf(buffer, sizeof(buffer), "RAN %u", (unsigned)codeplug_nxdn_ran(v));
        draw_text(FB_WIDTH - WIDGET_MARGIN, TONE_Y + 2, buffer, &font_roboto17, FONT_ALIGN_RIGHT,
                  THEME_COLOUR_TEXT_LABEL);
    } else if (v->mode == CODEPLUG_MODE_P25) {
        // P25: the talk group transmitted to (tx_target_apply()), the NAC on its line, right
        icons_draw(WIDGET_MARGIN, TONE_Y + 3, ICONS_TALKGROUP, THEME_COLOUR_TEXT_LABEL);
        if (target.id != 0) {
            draw_text(VALUE_X, TONE_Y, target.name, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
        }
        codeplug_text_nac(codeplug_p25_nac(v), buffer, sizeof(buffer));
        draw_text(FB_WIDTH - WIDGET_MARGIN, TONE_Y + 2, buffer, &font_roboto17, FONT_ALIGN_RIGHT,
                  THEME_COLOUR_TEXT_LABEL);
    } else if (v->mode == CODEPLUG_MODE_M17) {
        // M17: to ALL (m17_radio.h), the CAN on its line, right
        icons_draw(WIDGET_MARGIN, TONE_Y + 3, ICONS_TALKGROUP, THEME_COLOUR_TEXT_LABEL);
        draw_text(VALUE_X, TONE_Y, text(TEXT_ALL), &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
        snprintf(buffer, sizeof(buffer), "CAN %u", (unsigned)codeplug_m17_can(v));
        draw_text(FB_WIDTH - WIDGET_MARGIN, TONE_Y + 2, buffer, &font_roboto17, FONT_ALIGN_RIGHT,
                  THEME_COLOUR_TEXT_LABEL);
    } else if (v->mode == CODEPLUG_MODE_YSF) {
        // YSF Fusion: the destination (a contact's callsign, or ALL), the DG-ID on its line, right
        icons_draw(WIDGET_MARGIN, TONE_Y + 3, ICONS_TALKGROUP, THEME_COLOUR_TEXT_LABEL);
        draw_text(VALUE_X, TONE_Y, target.name[0] ? target.name : "ALL", &font_roboto20, FONT_ALIGN_LEFT,
                  THEME_COLOUR_TEXT);
        snprintf(buffer, sizeof(buffer), "DG-ID %u", (unsigned)codeplug_ysf_dgid(v));
        draw_text(FB_WIDTH - WIDGET_MARGIN, TONE_Y + 2, buffer, &font_roboto17, FONT_ALIGN_RIGHT,
                  THEME_COLOUR_TEXT_LABEL);
    } else {
        uint16_t tone = ((v->rx_tone != CODEPLUG_CSS_NONE) ? v->rx_tone : v->tx_tone);

        int sql = (v->sql != 0) ? v->sql : settings.squelch_defaults[radio_band_of(v->rx_freq)];

        // As the channel screen's line (channel_panel.c): the tone left, the squelch right
        if (tone != CODEPLUG_CSS_NONE) {
            tones_format(tone, buffer, sizeof(buffer));
            int32_t x = WIDGET_MARGIN + icons_draw(WIDGET_MARGIN, TONE_Y + 3, ICONS_TONE, THEME_COLOUR_TEXT_LABEL) + 8;

            draw_text(x, TONE_Y, buffer, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
        }
        snprintf(buffer, sizeof(buffer), "%d%%", (sql - SQUELCH_MIN) * SQUELCH_PERCENT_STEP);
        draw_text(FB_WIDTH - WIDGET_MARGIN, TONE_Y + 2, buffer, &font_roboto17, FONT_ALIGN_RIGHT,
                  THEME_COLOUR_TEXT_LABEL);
        icons_draw(FB_WIDTH - WIDGET_MARGIN - font_text_width(&font_roboto17, buffer) - 4 - ICONS_SQUELCH_WIDTH,
                   TONE_Y + ((font_roboto20.line_height - ICONS_SIZE) / 2) + 2, ICONS_SQUELCH, THEME_COLOUR_TEXT_LABEL);
    }

    if (digit_entry_active(&entry)) {
        bottom_bar_draw_soft_keys(text(TEXT_UI_OK), text(TEXT_UI_DELETE));
        return;
    }

    s_meter_data_t meter = {.compact = true, .active = true};
    bottom_bar_data_t bottom = {0};

    vfo_screen_signal(&meter);
    s_meter_draw(S_METER_Y, &meter);
    bottom.mode = codeplug_mode_name(v->mode);
    bottom.item1 = power_text(v);
    bottom.item2 = ((v->mode == CODEPLUG_MODE_FM) && (v->flag4 & FLAG4_BANDWIDTH_25K)) ? "25K" : "12.5K";
    static char ts_cc[16];
    if (v->mode == CODEPLUG_MODE_DMR) { // the timeslot and the colour code (DMR's bandwidth is always 12.5K)
        snprintf(ts_cc, sizeof(ts_cc), "TS%d  CC%d", target.timeslot, codeplug_colour_code(v));
        bottom.item2 = NULL;
        bottom.right = ts_cc; // right aligned
    }
    if (v->mode == CODEPLUG_MODE_NXDN) {
        bottom.item2 = "6.25K"; // NXDN48
    }
    if (codeplug_mode_is_ssb(v->mode)) {
        bottom.item2 = "3K";
    }
    bottom_bar_draw(&bottom);
}

const ui_screen_t vfo_screen = {
    .enter = enter,
    .leave = leave,
    .handle = handle,
    .draw = draw,
    .channel = radio_channel,
};
