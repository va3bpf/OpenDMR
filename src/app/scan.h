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
// channel or frequency when told, the scan decides when. A carrier (FM) or timeslot sync (DMR) pauses it shortly, a
// signal heard pauses it the scan delay (pause mode), as long as it is heard (hold mode), or stops it (stop mode).
// The screen runs scan_tick() every SCAN_TICK_MS on its timer.

#define SCAN_TICK_MS 10

typedef enum {
    SCAN_KEY_NONE,    // not the scan's: the screen handles it
    SCAN_KEY_USED,    // done (a release of the key that stopped it, a side key ...)
    SCAN_KEY_SKIP,    // * on a paused one: leave it out of this scan, then on
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
} scan_t;

// The time on a channel or frequency before moving on: the step time, DMR at least the time to find the timeslots
uint32_t scan_dwell_ms(bool dmr, bool simplex);
// The screen is on the first one; starts the screen's timer. direction: 1 up, -1 down
void scan_start(scan_t *scan, uint32_t dwell_ms, int direction);
void scan_stop(scan_t *scan);
// true: time to move on (the screen steps, then scan_moved()); scan->active false: it stopped (stop mode)
bool scan_tick(scan_t *scan, bool dmr);
void scan_moved(scan_t *scan, uint32_t dwell_ms);
// A key while scanning (or after: the release of the key that stopped it): LEFT turns the direction, UP goes on now,
// PTT stops it (and transmits: SCAN_KEY_NONE), SK1 / SK2 alone are left to the screen, another key stops it
scan_key_t scan_key(scan_t *scan, const ui_event_t *ev);

#endif
