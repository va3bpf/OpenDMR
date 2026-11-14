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

#include "settings.h"
#include "systime.h"
#include "radio.h"
#include "dmr.h"
#include "p25_radio.h"
#include "scan.h"

#define SETTLE_MS       30   // after a change, before the carrier is believed (SCAN_FREQ_CHANGE_SETTLING_INTERVAL)
#define SHORT_PAUSE_MS  500  // a carrier: time for the tone or the audio (SCAN_SHORT_PAUSE_TIME)
// DMR sync (or a P25 data unit): time for the audio. The main firmware waits (1 superframe + 1 timeslot) x 4 = 1560 ms;
// joining a DMR call already going on (as a scan does), the audio came 0.6 to 1.9 s after the retune (measured)
#define SYNC_PAUSE_MS   3000
#define TIMESLOT_MS     30
// DMR and P25: a step long enough to see a transmission without a repeater in the signal strength (on 27.5 ms of
// every 60: radio_scan_peak() reads every few ms), then up to SIGNAL_WAIT_MS where it rose SIGNAL_RISE over the noise
// floor: the HR_C7000 found the DMR sync 100 to 300 ms after the retune (measured, 50 mW a metre away)
#define DIGITAL_STEP_MS 50
#define SIGNAL_WAIT_MS  400
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
    scan_moved(scan, dwell_ms);
    radio_set_scanning(true);
    ui_timer_start(SCAN_TICK_MS);
}

void scan_stop(scan_t *scan)
{
    if (scan->active) {
        radio_set_scanning(false);
    }
    scan->active = false;
}

bool scan_tick(scan_t *scan, scan_kind_t kind)
{
    uint32_t now = systime_get_ms();
    bool digital = (kind != SCAN_ANALOG);

    if (!scan->active) {
        return false;
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
    if ((ev->key == UI_KEY_STAR) && (scan->state == STATE_PAUSED)) {
        scan->due_ms = systime_get_ms();
        return SCAN_KEY_SKIP;
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
