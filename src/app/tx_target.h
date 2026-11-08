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

#ifndef DM32_TX_TARGET_H
#define DM32_TX_TARGET_H

#include <stdbool.h>
#include <stdint.h>
#include "codeplug.h"

// The DMR call target of the channel or VFO shown: the talk group or private call and the timeslot (uiChannelMode.c
// loadChannelData() and updateTrxID(), trx.c trxUpdateTsForCurrentChannelWithSpecifiedContact(), uiUtilities.c
// tsSetManualOverride(), setOverrideTGorPC()). In this order:
//   settings.override_tg       a TG / PC entered or picked (# entry, contacts, last heard); kept across channels
//   the RX group's TG          LEFT / RIGHT step through the channel's RX group list (current_index_in_trx_group_list)
//   the channel's contact
// The timeslot: the manual override of settings.ts_manual_override (* toggles it), else the contact's override, else
// the channel's.

#define TX_TARGET_PC_FLAG 0x03 // the top byte of a private call (HR_C7000 PC_CALL_FLAG)

typedef enum {
    TX_TARGET_VFO_A = 0, // the main firmware's CHANNEL_VFO_A, CHANNEL_VFO_B, CHANNEL_CHANNEL
    TX_TARGET_VFO_B = 1,
    TX_TARGET_CHANNEL = 2,
} tx_target_source_t;

typedef struct {
    uint32_t id;     // TX_TARGET_PC_FLAG << 24 | the number for a private call; 0: none
    int timeslot;    // 1, 2
    bool overridden; // from settings.override_tg
    char name[48];   // the contact's name (UTF-8), or "TG 9" / "PC 123" without one
} tx_target_t;

tx_target_source_t tx_target_source(void); // the screen at home: the channel, or the current VFO

void tx_target_get(const codeplug_channel_t *channel, tx_target_t *target);
// The channel as the radio gets it: its timeslot the target's; and the target's ID to the DMR engine. The target as
// tx_target_get() (FM: none), for the screen to show
void tx_target_apply(codeplug_channel_t *channel, tx_target_t *target);

void tx_target_set(uint32_t id, bool private_call); // override (setOverrideTGorPC())
void tx_target_set_timeslot(int timeslot);          // manual override, 1 / 2 (tsSetManualOverride())
void tx_target_toggle_timeslot(const codeplug_channel_t *channel);
void tx_target_clear_timeslot(void);                                // long *: the contact's or channel's again
int tx_target_step_group(const codeplug_channel_t *channel, int step); // the TG's position; -1: no group of 2+

#endif
