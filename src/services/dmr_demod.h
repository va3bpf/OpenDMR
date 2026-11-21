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

// DMR 4FSK demodulation from the FM discriminator at 8 kHz (the HR_C7000's recording, as NXDN's and M17's): the bursts
// of the TDMA time line, 30 ms each, out as bits for dmr_l2.c. No OS, no hardware, all integer; also built on the PC
// (dmr_soft/c).
#ifndef DMR_DEMOD_H
#define DMR_DEMOD_H

#include <stdbool.h>
#include <stdint.h>

#include "fsk4.h"
#include "dmr_l2.h"

#define DMR_DEMOD_RING 2048 // samples kept (256 ms; a burst's slot is 240)

typedef struct {
    uint8_t bits[DMR_BURST_BYTES]; // the burst (dmr_l2.h)
    uint32_t cach;    // the 24 bits before it (a BS signal's CACH), the first sent the MSB
    int sync;         // DMR_SYNC_*: the sync found in it (timing it); DMR_SYNC_NONE: none
    int since_sync;   // slots since the last burst with a sync (0: this one)
    int32_t mse;      // the symbols' mean squared distance from their levels (Q8, level^2)
    int32_t g;        // the gain (Q8, sample units per symbol level)
} dmr_burst_t;

typedef void (*dmr_demod_cb)(void *ctx, const dmr_burst_t *b);

// Times in samples Q12
typedef struct {
    int32_t hist[FSK4_RRC_TAPS];
    fsk4_fir rrc;
    int32_t buf[DMR_DEMOD_RING];
    fsk4_ring x;               // the filtered samples
    uint32_t signs[DMR_DEMOD_RING / 32]; // each sample above dc_slow or not (bit n % RING): hunting's first look
    int32_t dc_slow;           // the long-term mean, Q8
    int32_t hunt_t;            // hunting: the next sync start time to try
    bool cand;                 // hunting: the best sync found among those close together
    int32_t cand_t;
    int cand_kind;
    fsk4_fit_t cand_fit;
    bool locked;
    int32_t t;                 // locked: the next slot's burst start (its symbol 0)
    int32_t g, dc;             // the levels (fsk4_fit())
    int since_sync;
    dmr_demod_cb cb;
    void *ctx;
    uint32_t syncs, lost, bursts, relocks; // counters (relocks: another time line found while locked)
} dmr_demod;

void dmr_demod_init(dmr_demod *d, dmr_demod_cb cb, void *ctx);
void dmr_demod_samples(dmr_demod *d, const int16_t *x, int count);

#endif
