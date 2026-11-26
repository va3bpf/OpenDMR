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
 * OpenDMR: the scan of the channel and VFO screens (scan.h). From uiChannelMode.c / uiVFOMode.c of the main
 * firmware: scanStart() (the dwell time), scanning() (the pauses), the keys of handleEvent() while scanning.
 */

#include <string.h>
#include "settings.h"
#include "systime.h"
#include "radio.h"
#include "dmr.h"
#include "p25_radio.h"
#include "nxdn_radio.h"
#include "ysf/ysf_radio.h"
#include "scan.h"
#include "custom_data.h"

#define SETTLE_MS       30   // after a change, before the carrier is believed (SCAN_FREQ_CHANGE_SETTLING_INTERVAL)
#define SHORT_PAUSE_MS  500  // a carrier: time for the tone or the audio (SCAN_SHORT_PAUSE_TIME)
// DMR sync (or a P25 data unit): time for the audio. The main firmware waits (1 superframe + 1 timeslot) x 4 = 1560 ms;
// joining a DMR call already going on (as a scan does), the audio came 0.6 to 1.9 s after the retune (measured)
#define SYNC_PAUSE_MS   2000 // 0.9: less dead time when the scan lands on an ongoing DMR call (still above the
                             // measured 0.6 - 1.9 s until its audio; 1500 ms missed calls that start late)
#define TIMESLOT_MS     30
// DMR and P25: a step long enough to see a transmission without a repeater in the signal strength (on 27.5 ms of
// every 60: radio_scan_peak() reads every few ms), then up to SIGNAL_WAIT_MS where it rose SIGNAL_RISE over the noise
// floor: the HR_C7000 found the DMR sync 100 to 300 ms after the retune (measured, 50 mW a metre away)
#define DIGITAL_STEP_MS 50
#define SIGNAL_WAIT_MS  400
#define STAR_HOLD_MS    5000 // * held on a paused one: it stays there until the release or the long press
#define SIGNAL_RISE     8    // raw steps (about dB): the floor moved 76 - 82 from one frequency to another
#define FLOOR_WEIGHT    8    // the floor: a moving average of the steps without a signal

enum { MODE_HOLD, MODE_PAUSE, MODE_STOP }; // settings.scan_mode_pause
enum { STATE_SCANNING, STATE_SHORT_PAUSED, STATE_PAUSED };

uint32_t scan_dwell_ms(scan_kind_t kind)
{
    uint32_t step = TIMESLOT_MS + (settings.scan_step_time * TIMESLOT_MS);

    if (kind == SCAN_ANALOG) {
        return step;
    }
    return (step < DIGITAL_STEP_MS) ? DIGITAL_STEP_MS : step;
}

static bool found(const scan_t *scan, scan_kind_t kind, uint32_t now)
{
    switch (kind) {
        case SCAN_DMR:
            return dmr_has_sync();
        case SCAN_P25:
            return p25_radio_heard_within(now - scan->step_ms);
        case SCAN_NXDN:
            return nxdn_radio_heard_within(now - scan->step_ms);
        case SCAN_YSF:
            return ysf_radio_heard_within(now - scan->step_ms);
        default:
            return radio_has_carrier();
    }
}

// DMR / P25 while the step runs: the signal over the floor keeps the step for its sync
static void signal_check(scan_t *scan)
{
    uint16_t samples;
    uint8_t peak = radio_scan_peak(&samples);

    if (!scan->waiting && (samples > 0) && (scan->floor != 0) && (peak >= (scan->floor + SIGNAL_RISE))) {
        scan->waiting = true;
        scan->due_ms = scan->step_ms + SIGNAL_WAIT_MS;
    }
}

// DMR / P25 moving on from a step without a signal: its peak into the floor
static void floor_update(scan_t *scan)
{
    uint16_t samples;
    uint8_t peak = radio_scan_peak(&samples);

    if (scan->waiting || (samples == 0)) {
        return;
    }
    scan->floor = (scan->floor == 0)
                      ? peak
                      : (uint8_t)((((uint32_t)scan->floor * (FLOOR_WEIGHT - 1)) + peak + (FLOOR_WEIGHT / 2)) /
                                  FLOOR_WEIGHT);
}

void scan_moved(scan_t *scan, uint32_t dwell_ms)
{
    scan->state = STATE_SCANNING;
    scan->step_ms = systime_get_ms();
    scan->dwell_ms = dwell_ms;
    scan->due_ms = scan->step_ms + dwell_ms;
    scan->waiting = false;
}

void scan_start(scan_t *scan, uint32_t dwell_ms, int direction)
{
    scan->active = true;
    scan->direction = (int8_t)((direction < 0) ? -1 : 1);
    scan->swallow_key = UI_KEY_COUNT;
    scan->floor = 0;
    scan->star_held = false;
    scan->move_now = false;
    scan_moved(scan, dwell_ms);
    radio_set_scanning(true);
    ui_timer_start(SCAN_TICK_MS);
}

bool scan_on_signal(const scan_t *scan)
{
    return scan->active && (scan->state != STATE_SCANNING);
}

void scan_stop(scan_t *scan)
{
    if (scan->active) {
        radio_set_scanning(false);
    }
    scan->active = false;
    scan->star_held = false;
}

bool scan_tick(scan_t *scan, scan_kind_t kind)
{
    uint32_t now = systime_get_ms();
    bool digital = (kind != SCAN_ANALOG);

    if (!scan->active) {
        return false;
    }
    if (scan->move_now) { // locked out with *: away from its signal now
        scan->move_now = false;
        scan->state = STATE_SCANNING;
        return true;
    }
    if (digital && (scan->state == STATE_SCANNING)) {
        signal_check(scan);
    }
    if ((scan->state == STATE_SCANNING) && ((now - scan->step_ms) >= SETTLE_MS) && found(scan, kind, now)) {
        scan->state = STATE_SHORT_PAUSED;
        scan->due_ms = now + (digital ? SYNC_PAUSE_MS : SHORT_PAUSE_MS);
    }
    if ((scan->state == STATE_SHORT_PAUSED) ||
        ((scan->state == STATE_PAUSED) && (settings.scan_mode_pause == MODE_HOLD))) {
        if (radio_audio_heard()) {
            if (settings.scan_mode_pause == MODE_STOP) {
                scan_stop(scan);
                return false;
            }
            scan->due_ms = now + (settings.scan_delay * 1000U);
            scan->state = STATE_PAUSED;
        }
    }
    if ((int32_t)(now - scan->due_ms) < 0) {
        return false;
    }
    if (digital && (scan->state == STATE_SCANNING)) {
        floor_update(scan);
    }
    return true;
}

scan_key_t scan_key(scan_t *scan, const ui_event_t *ev)
{
    // * on a paused one: short (its release) leaves it out until power off, long for good
    if (scan->star_held && (ev->key == UI_KEY_STAR)) {
        if ((ev->type == UI_EVENT_KEY_LONG) || (ev->type == UI_EVENT_KEY_RELEASE)) {
            bool held = (ev->type == UI_EVENT_KEY_LONG);

            scan->star_held = false;
            scan->swallow_key = (held ? UI_KEY_STAR : UI_KEY_COUNT); // the release after a long press
            scan->move_now = true;
            return (held ? SCAN_KEY_SKIP_SAVED : SCAN_KEY_SKIP);
        }
        return SCAN_KEY_USED;
    }
    if ((scan->swallow_key != UI_KEY_COUNT) && (ev->key == scan->swallow_key)) {
        if (ev->type == UI_EVENT_KEY_RELEASE) {
            scan->swallow_key = UI_KEY_COUNT;
        }
        return SCAN_KEY_USED;
    }
    if (!scan->active || (ev->key == UI_KEY_SK1) || (ev->key == UI_KEY_SK2)) {
        return SCAN_KEY_NONE;
    }
    if (ev->type != UI_EVENT_KEY_PRESS) {
        return (ev->key == UI_KEY_PTT) ? SCAN_KEY_NONE : SCAN_KEY_USED; // the keys that started it
    }
    if (ev->key == UI_KEY_PTT) {
        scan_stop(scan);
        return SCAN_KEY_NONE; // and transmits
    }
    scan->swallow_key = ev->key;
    if ((ev->key == UI_KEY_STAR) && (scan->state != STATE_SCANNING)) { // on a signal (waiting for its audio too)
        scan->star_held = true;
        scan->swallow_key = UI_KEY_COUNT;
        scan->due_ms = systime_get_ms() + STAR_HOLD_MS; // it stays there while * is held
        return SCAN_KEY_USED;
    }
    if ((ev->key == UI_KEY_LEFT) && (ev->modifiers == 0)) {
        scan->direction = (int8_t)-scan->direction;
        return SCAN_KEY_USED;
    }
    if ((ev->key == UI_KEY_UP) && (ev->modifiers == 0)) {
        scan->due_ms = systime_get_ms();
        return SCAN_KEY_USED;
    }
    scan_stop(scan);
    return SCAN_KEY_STOPPED;
}

/* Nuisance delete */

scan_skipped_t scan_skipped_channels;
scan_skipped_t scan_skipped_frequencies;

bool scan_skipped_has(const scan_skipped_t *skipped, uint32_t value)
{
    for (int i = 0; i < skipped->count; i++) {
        if (skipped->item[i] == value) {
            return true;
        }
    }
    return false;
}

void scan_skipped_remove(scan_skipped_t *skipped, int index)
{
    if ((index >= 0) && (index < skipped->count)) {
        for (int i = index + 1; i < skipped->count; i++) {
            skipped->item[i - 1] = skipped->item[i];
        }
        skipped->count--;
    }
}

bool scan_skipped_add(scan_skipped_t *skipped, uint32_t value)
{
    if (scan_skipped_has(skipped, value)) {
        return true;
    }
    if (skipped->count >= SCAN_SKIPPED_MAX) {
        return false;
    }
    skipped->item[skipped->count++] = value;
    return true;
}

/* Skipped for good: VFO frequencies (the channels have their skip flags in the codeplug) */

#define SAVED_DATA_TYPE 0x44540005 // a custom data block of this firmware ("DT", 5)

typedef struct {
    uint32_t count;
    uint32_t frequency[SCAN_SAVED_MAX];
} saved_t;

static saved_t saved;
static bool saved_loaded = false;

static void saved_load(void)
{
    if (!saved_loaded) {
        if (!custom_data_read(SAVED_DATA_TYPE, (uint8_t *)&saved, sizeof(saved)) || (saved.count > SCAN_SAVED_MAX)) {
            saved.count = 0;
        }
        saved_loaded = true;
    }
}

bool scan_saved_has(uint32_t frequency)
{
    saved_load();
    for (uint32_t i = 0; i < saved.count; i++) {
        if (saved.frequency[i] == frequency) {
            return true;
        }
    }
    return false;
}

bool scan_saved_add(uint32_t frequency)
{
    if (scan_saved_has(frequency)) {
        return true;
    }
    if (saved.count >= SCAN_SAVED_MAX) {
        return false;
    }
    saved.frequency[saved.count++] = frequency;
    return custom_data_write(SAVED_DATA_TYPE, (const uint8_t *)&saved, sizeof(saved));
}

int scan_saved_count(void)
{
    saved_load();
    return (int)saved.count;
}

uint32_t scan_saved_frequency(int index)
{
    saved_load();
    return ((index >= 0) && ((uint32_t)index < saved.count)) ? saved.frequency[index] : 0;
}

void scan_saved_remove(int index)
{
    saved_load();
    if ((index >= 0) && ((uint32_t)index < saved.count)) {
        for (uint32_t i = (uint32_t)index + 1; i < saved.count; i++) {
            saved.frequency[i - 1] = saved.frequency[i];
        }
        saved.frequency[--saved.count] = 0;
        (void)custom_data_write(SAVED_DATA_TYPE, (const uint8_t *)&saved, sizeof(saved));
    }
}

void scan_saved_clear(void)
{
    saved_load();
    if (saved.count) {
        memset(&saved, 0, sizeof(saved));
        (void)custom_data_write(SAVED_DATA_TYPE, (const uint8_t *)&saved, sizeof(saved));
    }
}
