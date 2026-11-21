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

// 4FSK from the FM discriminator recording at 8 kHz, the parts the software receivers share (NXDN, M17, DMR): the
// samples kept in a ring and read at any time by cubic interpolation, an input FIR, a least squares fit of gain and DC
// to known symbols (a sync word), soft symbols. No OS, no hardware, all integer; also built on the PC.
#ifndef FSK4_H
#define FSK4_H

#include <stdbool.h>
#include <stdint.h>

// Times are in samples Q12 (int32: the receivers move them back before 2^19 samples)
#define FSK4_Q   12
#define FSK4_ONE (1 << FSK4_Q)
// 4800 symbols/s (M17, DMR): a symbol every 5/3 samples; 2400 (NXDN48) 10/3
#define FSK4_SPS_4800_NUM 20480 // symbol k of a frame at t: t + (k * NUM + DEN / 2) / DEN
#define FSK4_SPS_DEN      3

// the samples: ring[n % size] sample n (size a power of two); within +-65535 (the interpolation's products in 32 bits)
typedef struct {
    int32_t *x;
    uint32_t mask; // size - 1
    uint32_t n;    // samples in so far
} fsk4_ring;

void fsk4_ring_init(fsk4_ring *r, int32_t *buf, uint32_t size);
static inline void fsk4_put(fsk4_ring *r, int32_t v)
{
    r->x[r->n & r->mask] = v;
    r->n++;
}
// the signal at time t (Q12), cubic (Catmull-Rom); needs the samples t - 1 .. t + 2
int32_t fsk4_at(const fsk4_ring *r, int32_t t);
// t can be read (its samples in)
static inline bool fsk4_have(const fsk4_ring *r, int32_t t)
{
    return ((t >> FSK4_Q) + 3) <= (int32_t)r->n;
}
// the time of symbol k (0 .. 1000) of a frame starting at t, the symbol period num / den samples (32-bit: the CPU
// has no 64-bit divide)
static inline int32_t fsk4_sym_t(int32_t t, int k, int32_t num, int32_t den)
{
    return t + (k * num + den / 2) / den;
}
// the samples moved back by `by` (a multiple of the ring's size): n reduced, the caller's times by by << FSK4_Q
void fsk4_rebase(fsk4_ring *r, uint32_t by);

// An FIR on the input: taps Q(shift), the history in hist (taps long)
#define FSK4_RRC_TAPS 21 // the receive filter at 4800 symbols/s (DMR, P25): root raised cosine, alpha 0.2, +-6 symbols
extern const int16_t fsk4_rrc_4800[FSK4_RRC_TAPS]; // Q12, sum 4096
typedef struct {
    const int16_t *taps;
    int32_t *hist;
    int n, pos, shift;
} fsk4_fir;
void fsk4_fir_init(fsk4_fir *f, const int16_t *taps, int32_t *hist, int n, int shift);
int32_t fsk4_fir_step(fsk4_fir *f, int32_t x);

// Gain and DC by least squares of the values v to the symbols p (levels +-1, +-3), n of them: v ~ g p + dc. The gain
// Q8 (per symbol level), the error the mean squared residual in levels^2, Q8. False (the others not set): the gain under
// g_min or the error not under err_max
typedef struct {
    int32_t g, dc, err;
} fsk4_fit_t;
bool fsk4_fit(const int32_t *v, const int8_t *p, int n, int32_t g_min, int32_t err_max, fsk4_fit_t *out);

// A value as a soft symbol: (v - dc) / gain x one (gain Q8 a level), clipped to +-127
int fsk4_soft(int32_t v, int32_t g, int32_t dc, int one);

// A block's symbols in levels Q8 (+3: 768): the four levels' means from the decisions (thresholds 0, +-512), the
// thresholds put at their midpoints, mapped back to 0 and +-512, piecewise linear (the FM's non-linearity, DC)
void fsk4_relevel(int32_t *z, int n);
// The same block through the 3-tap equaliser (-0.05, 0.95, -0.05: least squares on DMR's recordings, and on M17's
// -0.03, 0.95, -0.025 nearly as good: the intersymbol interference at 5/3 samples a symbol); its first and last
// symbols as they are
void fsk4_eq3(int32_t *z, int n);

#endif
