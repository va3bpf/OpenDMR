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

#ifndef DM32_DMR_H
#define DM32_DMR_H

#include <stdbool.h>
#include <stdint.h>
#include "codeplug.h"

// DMR (dmr.c): the main firmware's HR_C7000 engine (hr_c7000.c) behind the radio service. The radio service tunes the
// FD6818 and calls these; the UI reads the call.

void dmr_init(void);  // the engine's state and its interrupts (vectors 61 - 64)
void dmr_start(void); // its task

void dmr_enter(const codeplug_channel_t *channel);   // from FM: the modem to DMR, then dmr_channel()
void dmr_retune(void);                               // DMR to DMR: the engine stopped before the FD6818 is tuned again
void dmr_channel(const codeplug_channel_t *channel); // then the engine on that channel (colour code, timeslot ...)
void dmr_leave(void);                                // to FM
void dmr_modem_calibration(void);                    // the modem's DMR calibration (transmitting)

void dmr_transmit(bool on);     // the PTT: the engine wakes the repeater if needed and transmits; off: it ends
void dmr_set_tx_id(uint32_t id); // the TG, or TX_TARGET_PC_FLAG << 24 | the called ID (tx_target.c)
void dmr_set_own_id(uint32_t id); // the DMR ID changed (number entry)
bool dmr_is_transmitting(void); // asked or still ending
bool dmr_has_sync(void);        // a DMR signal's timeslots found (scan)
bool dmr_repeater_failed(void); // the repeater didn't wake up

typedef struct {
    uint32_t source;   // the caller's DMR ID
    uint32_t tg_or_pc; // the talk group, or LAST_HEARD_PC_FLAG << 24 | the called ID (last_heard.h)
    uint8_t timeslot;  // 0: TS1, 1: TS2
    bool has_location; // a GPS Info LC was heard from this caller (position.h)
    int32_t latitude_e5;  // 1/100000 degree, + north
    int32_t longitude_e5; // 1/100000 degree, + east
} dmr_call_t;

bool dmr_call(dmr_call_t *call); // a call is shown (the caller known); false: none

#endif
