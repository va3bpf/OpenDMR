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

// The DMR receiver in software (dmr_soft.c): the FM discriminator recorded and demodulated as NXDN and M17 are,
// instead of the HR_C7000's own DMR receive; the HR_C7000 engine still transmits. On a DMR channel (not in hotspot
// mode) while dmr_soft_enabled.
#ifndef DMR_SOFT_H
#define DMR_SOFT_H

#include <stdbool.h>
#include <stdint.h>

extern volatile uint32_t dmr_soft_enabled; // 0: the HR_C7000 receives (over the cable: A / B tests)
// direct mode (Rx == Tx) transmitted in software too (0: the HR_C7000 engine transmits); 50 mW (tests side by side);
// counters of the last transmission: calls, bursts, AMBE frames encoded, FIFO underruns
extern volatile uint32_t dmr_soft_tx, dmr_soft_tx_low_power, dmr_soft_tx_stats[4];

bool dmr_soft_wanted(void); // a DMR channel would be received in software now
void dmr_soft_start(void);
void dmr_soft_stop(void);
// APRS over DMR with the software receiver running (dmr.c): the engine sends the bursts (hr_c7000.h), the recording
// off meanwhile; false: not running (the engine's own way then)
bool dmr_soft_send_data(const uint8_t *types, const uint8_t *lengths, const uint8_t (*payloads)[12], uint8_t count);
bool dmr_soft_data_busy(void);
bool dmr_soft_data_failed(void);
void dmr_soft_data_cancel(void);
bool dmr_soft_running(void);
void dmr_soft_ptt(bool on);       // the PTT: the recording off, the engine transmits (dmr.c)
bool dmr_soft_transmitting(void); // the PTT held, or the transmission still ending
bool dmr_soft_has_sync(void);     // a DMR signal's time line followed (scan)
bool dmr_soft_busy(void);         // a call on the channel now

// counters, read over the cable
typedef struct {
    uint32_t halves, missed_halves, busy_us_max, bursts, syncs, lost, relocks;
    uint32_t headers, terminators, emb_ok, emb_bad, emb_lcs, voice_bursts, calls, filtered;
    uint32_t pcm_drops, underruns, dec_us_max, other_cc;
} dmr_soft_stats_t;
extern volatile dmr_soft_stats_t dmr_soft_stats;

#endif
