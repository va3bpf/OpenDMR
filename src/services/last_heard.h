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

#ifndef DM32_LAST_HEARD_H
#define DM32_LAST_HEARD_H

#include <stdbool.h>
#include <stdint.h>

// The stations heard on DMR, the most recent first (uiUtilities.c lastHeardListUpdate(), callsList). The DMR receive
// adds the callers (from its task, not from an interrupt); the last heard screen shows them. Each change posts a
// UI_EVENT_DATA with UI_DATA_LAST_HEARD.

#define LAST_HEARD_MAX        32 // as the main firmware (NUM_LASTHEARD_STORED)
#define LAST_HEARD_TEXT_SIZE  32
#define LAST_HEARD_PC_FLAG    0x03 // in the top byte of tg_or_pc: a private call (HR_C7000 PC_CALL_FLAG)
#define LAST_HEARD_TS_UNKNOWN 0xFF

typedef struct {
    uint32_t id;                             // the caller's DMR ID
    uint32_t tg_or_pc;                       // the talk group, or LAST_HEARD_PC_FLAG << 24 | the called DMR ID
    uint32_t time_ms;                        // systime when heard last
    int32_t latitude_e5;                     // the position of its last GPS Info LC, 1/100000 degree, + north
    int32_t longitude_e5;                    // ... + east
    uint8_t timeslot;                        // 0: TS1, 1: TS2, LAST_HEARD_TS_UNKNOWN
    bool has_location;                       // the position above was heard (position.h)
    char text[LAST_HEARD_TEXT_SIZE];         // "callsign name ..." from the contacts or the DMR IDs; "": not known
    char talker_alias[LAST_HEARD_TEXT_SIZE]; // "": none
} last_heard_entry_t;

// A caller: to the top of the list (already there: moved, its talk group, timeslot and time updated; text NULL keeps
// its text), the oldest one goes when the list is full
void last_heard_add(uint32_t id, uint32_t tg_or_pc, uint8_t timeslot, const char *text);
void last_heard_set_talker_alias(uint32_t id, const char *alias);
void last_heard_set_location(uint32_t id, int32_t latitude_e5, int32_t longitude_e5);
void last_heard_clear(void);

int last_heard_count(void);
bool last_heard_get(int position, last_heard_entry_t *entry); // 0: the most recent; a copy

#endif
