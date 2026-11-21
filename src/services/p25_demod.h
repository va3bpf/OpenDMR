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

// P25 Phase 1 C4FM demodulation from the FM discriminator at 8 kHz (the HR_C7000's recording, as NXDN's, M17's and
// DMR's): the dibits on the air (status dibits in them) out one by one for p25-core's receiver (p25_rx_dibit()), which
// finds the frames itself. No OS, no hardware, all integer; also built on the PC (p25_soft).
#ifndef P25_DEMOD_H
#define P25_DEMOD_H

#include <stdbool.h>
#include <stdint.h>

#include "fsk4.h"

#define P25_DEMOD_RING 1024 // samples kept (128 ms: a symbol is 5/3 of one, an LDU 1440)
#define P25_DEMOD_LOST (-1) // the callback's dibit: the signal lost (the receiver hunts again)

typedef void (*p25_demod_cb)(void *ctx, int dibit);

// Times in samples Q12
typedef struct {
    fsk4_fir rrc;
    int32_t hist[FSK4_RRC_TAPS];
    int32_t buf[P25_DEMOD_RING];
    fsk4_ring x;
    uint32_t signs[P25_DEMOD_RING / 32]; // each sample above dc_slow or not (bit n % RING): hunting's first look
    int32_t dc_slow;           // the long-term mean, Q8
    int32_t hunt_t;            // hunting: the next sync start to try
    bool cand;                 // hunting: the best sync among those close together
    int32_t cand_t;
    int cand_sign;
    fsk4_fit_t cand_fit;
    bool locked;
    int32_t t;                 // locked: the last sync's start (symbol 0)
    int k;                     // locked: the next symbol read, counted from t (the one out is k - 2: the equaliser)
    int sign;                  // the polarity: +1, or -1 (the levels turned over)
    int32_t g, dc;             // the levels (fsk4_fit())
    int32_t z1, z2;            // symbols k - 1 and k - 2 (Q8 levels, releveled)
    int32_t m[4];              // the four levels' running means after g and dc (Q8: -3 .. +3 is -768 .. 768)
    int since_sync;            // symbols out since the last sync
    uint64_t last;             // the last 32 dibits out (2 bits each, the newest lowest)
    p25_demod_cb cb;
    void *ctx;
    uint32_t syncs, lost, relocks, symbols; // counters
} p25_demod;

void p25_demod_init(p25_demod *d, p25_demod_cb cb, void *ctx);
void p25_demod_samples(p25_demod *d, const int16_t *x, int count);

#endif
