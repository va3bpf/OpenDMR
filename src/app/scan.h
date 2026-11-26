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

#ifndef DM32_SCAN_H
#define DM32_SCAN_H

#include <stdbool.h>
#include <stdint.h>
#include "ui.h"

// The scan of the channel and VFO screens (uiChannelMode.c / uiVFOMode.c scanning()): the screen steps to the next
// channel or frequency when told, the scan decides when. A carrier (FM), timeslot sync (DMR) or a data unit (P25)
// pauses it shortly, a signal heard pauses it the scan delay (pause mode), as long as it is heard (hold mode), or stops
// it (stop mode). DMR and P25 take longer to find than a carrier: a step stays longer only where the signal strength
// rose over the noise floor. The screen runs scan_tick() every SCAN_TICK_MS on its timer.

#define SCAN_TICK_MS 10

typedef enum { SCAN_ANALOG, SCAN_DMR, SCAN_P25, SCAN_NXDN, SCAN_YSF } scan_kind_t;

typedef enum {
    SCAN_KEY_NONE,    // not the scan's: the screen handles it
    SCAN_KEY_USED,    // done (a release of the key that stopped it, a side key ...)
    SCAN_KEY_SKIP,       // * (short) on a paused one: left out until power off (scan_skipped_add()), then on
    SCAN_KEY_SKIP_SAVED, // * held on a paused one: left out for good (the channel's skip flag, scan_saved_add())
    SCAN_KEY_STOPPED, // the scan ended: nothing more for this key
} scan_key_t;

typedef struct {
    bool active;
    uint8_t state;
    int8_t direction; // 1 up, -1 down
    uint8_t swallow_key;
    uint32_t step_ms; // when the channel or frequency changed
    uint32_t due_ms;  // when it moves on
    uint32_t dwell_ms;
    bool waiting;  // a DMR / P25 step kept longer for its signal
    uint8_t floor; // DMR / P25: the signal strength of the steps without a signal (0: not known yet)
    bool star_held; // * pressed on a paused one: its release (short) or its long press decides; it stays there
    bool move_now;  // locked out with *: on at once, whatever the signal does (Hold would keep it there)
} scan_t;

// Nuisance delete: the channels (their numbers) and VFO frequencies skipped with * while a scan stood on them; left
// out of every scan until they are cleared (the quick menu) or the radio is switched off
#define SCAN_SKIPPED_MAX 64

typedef struct {
    uint32_t item[SCAN_SKIPPED_MAX];
    uint8_t count;
} scan_skipped_t;

extern scan_skipped_t scan_skipped_channels;
extern scan_skipped_t scan_skipped_frequencies;

bool scan_skipped_add(scan_skipped_t *skipped, uint32_t value); // false: the list is full
bool scan_skipped_has(const scan_skipped_t *skipped, uint32_t value);
void scan_skipped_remove(scan_skipped_t *skipped, int index);

// VFO frequencies skipped for good (* held): a custom data block of the codeplug ("DT" 5), kept over power off
#define SCAN_SAVED_MAX 32

bool scan_saved_has(uint32_t frequency);
bool scan_saved_add(uint32_t frequency); // false: full, or not written
int scan_saved_count(void);
uint32_t scan_saved_frequency(int index);
void scan_saved_remove(int index);
void scan_saved_clear(void);

// The time on a channel or frequency before moving on: the step time, DMR and P25 at least DIGITAL_STEP_MS
uint32_t scan_dwell_ms(scan_kind_t kind);
// The screen is on the first one; starts the screen's timer. direction: 1 up, -1 down
void scan_start(scan_t *scan, uint32_t dwell_ms, int direction);
void scan_stop(scan_t *scan);
// Standing on a signal (paused, or waiting for its audio): * there locks it out
bool scan_on_signal(const scan_t *scan);
// true: time to move on (the screen steps, then scan_moved()); scan->active false: it stopped (stop mode)
bool scan_tick(scan_t *scan, scan_kind_t kind);
void scan_moved(scan_t *scan, uint32_t dwell_ms);
// A key while scanning (or after: the release of the key that stopped it): LEFT turns the direction, UP goes on now,
// PTT stops it (and transmits: SCAN_KEY_NONE), SK1 / SK2 alone are left to the screen, another key stops it
scan_key_t scan_key(scan_t *scan, const ui_event_t *ev);

#endif
