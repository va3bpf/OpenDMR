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
 * OpenDMR, service: YSF C4FM demodulation (ysf_demod.h), derived from p25_demod.c: the physical layer is the same
 * (RRC 0.2 at 4800 symbols/s), only the sync differs -- YSF's 20 symbols D471C9634D, once every 480-symbol frame.
 *
 * - the samples through DMR's receive filter (fsk4_rrc_4800, root raised cosine 0.2) into fsk4.c's ring; a symbol every
 *   5/3 samples, read by cubic interpolation.
 * - hunting: every third of a sample, the sync's 20 symbols, both polarities, first by their signs at the nearest
 *   samples, then a least squares fit of gain and DC; the best of those close together, refined to a sixth of a sample,
 *   starts the time line at its first symbol.
 * - locked: each symbol, scaled by the gain and DC, through the four levels' running means and the 3-tap equaliser
 *   (one symbol late), decided, out. The last 20 dibits are checked for the sync (up to 4 wrong): it fits again within
 *   a sample of where it is. Every frame repeats the sync; no sync for LOST_SYMS ends the receiver's lock.
 */

#include <string.h>

#include "ysf_demod.h"

#define Q          FSK4_Q
#define ONE        FSK4_ONE
#define HUNT_STEP  1365              // a third of a sample, Q12
#define FINE_STEP  683               // a sixth
#define TRACK      6                 // a sync refined within +-TRACK sixths of a sample
#define G_MIN      (80 << 8)         // gain at least (Q8 a level)
#define HUNT_ERR   154               // the fit's mean squared error under 0.6 level^2 (Q8) hunting ...
#define TRACK_ERR  200               // ... 0.78 where the dibits say a sync is
#define SYNC_SYMS  20                // D471C9634D: 20 symbols, repeated at the start of every frame
#define HUNT_SIGNS 18                // of the 20 symbols' signs at least (or at most 20 - this: the other polarity)
#define SYNC_WRONG 4                 // dibits of the sync wrong at most, locked (2 symbol errors of 2 bits each)
#define HUNT_SYMS  600               // locked without a sync this long: hunting too (a frame is 480 symbols)
#define LOST_SYMS  2000              // ... and lost (four missed frames end a call)
#define REBASE_AT  (1U << 18)        // samples: the Q12 times kept within int32
#define REBASE_BY  (1U << 17)        // a multiple of the ring
#define SYNC_WORD  0xD471C9634DULL   // 20 dibits, the first sent the highest
#define SYNC_MASK  0xFFFFFFFFFFULL   // 40 bits
#define LEVEL_RATE 5                 // the levels' running means: 1/32 of each symbol

static int8_t pattern[SYNC_SYMS]; // the sync's levels
static uint64_t sign_mask[3], sign_positive[3];
static bool ready;

static int8_t level_of(unsigned dibit)
{
    static const int8_t lv[4] = {1, 3, -1, -3}; // 00 +1, 01 +3, 10 -1, 11 -3
    return lv[dibit & 3];
}

static void make_pattern(void)
{
    if (ready) {
        return;
    }
    for (int k = 0; k < SYNC_SYMS; k++) {
        pattern[k] = level_of((unsigned)(SYNC_WORD >> (38 - 2 * k)));
    }
    for (int ph = 0; ph < 3; ph++) {
        for (int k = 0; k < SYNC_SYMS; k++) {
            uint64_t bit = 1ULL << ((ph + 5 * k + 1) / 3);
            sign_mask[ph] |= bit;
            if (pattern[k] > 0) {
                sign_positive[ph] |= bit;
            }
        }
    }
    ready = true;
}

static int32_t sym_t(int32_t t, int k)
{
    return fsk4_sym_t(t, k, FSK4_SPS_4800_NUM, FSK4_SPS_DEN);
}

void ysf_demod_init(ysf_demod *d, ysf_demod_cb cb, void *ctx)
{
    make_pattern();
    memset(d, 0, sizeof(*d));
    fsk4_fir_init(&d->rrc, fsk4_rrc_4800, d->hist, FSK4_RRC_TAPS, 12);
    for (int i = 0; i < 4; i++) {
        d->m[i] = (2 * i - 3) * 256;
    }
    fsk4_ring_init(&d->x, d->buf, YSF_DEMOD_RING);
    d->cb = cb;
    d->ctx = ctx;
    d->hunt_t = 8 << Q;
    d->sign = 1;
}

// the sync at t (its first symbol) fitted with sign; true: under max
static bool fit_sync(const ysf_demod *d, int32_t t, int sign, int32_t max, fsk4_fit_t *f)
{
    int32_t v[SYNC_SYMS];
    int8_t p[SYNC_SYMS];
    for (int k = 0; k < SYNC_SYMS; k++) {
        v[k] = fsk4_at(&d->x, sym_t(t, k));
        p[k] = (int8_t)(sign * pattern[k]);
    }
    return fsk4_fit(v, p, SYNC_SYMS, G_MIN, max, f);
}

// 64 of the samples' sign bits from sample i on (bit k: sample i + k)
static uint64_t sign_window(const ysf_demod *d, uint32_t i)
{
    const uint32_t words = YSF_DEMOD_RING / 32;
    uint32_t w = (i & d->x.mask) >> 5, b = i & 31;
    uint64_t lo = d->signs[w] | ((uint64_t)d->signs[(w + 1) % words] << 32);
    if (!b) {
        return lo;
    }
    return (lo >> b) | ((uint64_t)d->signs[(w + 2) % words] << (64 - b));
}

static int32_t oldest_t(const ysf_demod *d)
{
    return (d->x.n > YSF_DEMOD_RING - 16) ? (int32_t)((d->x.n - YSF_DEMOD_RING + 16) << Q) : 0;
}

// symbol k in levels Q8 (+3: 768) from the time line, gain and DC; then through the levels' running means (as
// fsk4_relevel(), symbol by symbol: the FM's non-linearity and what the fit's DC missed), the means updated
static int32_t symbol(ysf_demod *d, int k)
{
    int32_t gs = (d->g >> 8) ? (d->g >> 8) : 1;
    int32_t v = d->sign * (fsk4_at(&d->x, sym_t(d->t, k)) - d->dc) * 256 / gs;
    int32_t *m = d->m;
    int i = (v >= 512) ? 3 : (v >= 0) ? 2 : (v >= -512) ? 1 : 0;
    m[i] += (v - m[i]) >> LEVEL_RATE;
    int32_t tn = (m[0] + m[1]) / 2, t0 = (m[1] + m[2]) / 2, tp = (m[2] + m[3]) / 2;
    if (v >= tp) {
        return 512 + (v - tp) * 256 / ((m[3] - tp > 1) ? (m[3] - tp) : 1);
    }
    if (v >= t0) {
        return 512 * (v - t0) / ((tp - t0 > 1) ? (tp - t0) : 1);
    }
    if (v >= tn) {
        return -512 * (t0 - v) / ((t0 - tn > 1) ? (t0 - tn) : 1);
    }
    return -512 - (tn - v) * 256 / ((tn - m[0] > 1) ? (tn - m[0]) : 1);
}

// symbol out next: out (its neighbours read again, the equaliser's)
static void restart(ysf_demod *d, int out)
{
    d->z2 = symbol(d, out - 1);
    d->z1 = symbol(d, out);
    d->k = out + 1;
}

// a time line from a sync starting at t. Locked already: the dibits go on from where they are (none twice, none
// skipped: the receiver needs them in a row), on the new line
static void lock_at(ysf_demod *d, int32_t t, int sign, const fsk4_fit_t *f)
{
    int out = 0;
    if (d->locked) {
        d->relocks++;
        int64_t ahead = (int64_t)t - sym_t(d->t, d->k - 1); // the sync's start from the next symbol due out, Q12
        out = -(int)((ahead * FSK4_SPS_DEN + (ahead >= 0 ? FSK4_SPS_4800_NUM / 2 : -FSK4_SPS_4800_NUM / 2)) /
                     FSK4_SPS_4800_NUM);
    }
    d->locked = true;
    d->t = t;
    d->sign = sign;
    d->g = f->g;
    d->dc = f->dc;
    d->since_sync = 0;
    d->syncs++;
    restart(d, out);
}

// hunting (not locked, or locked without a sync for HUNT_SYMS): the candidates whose symbols are all in
static void hunt(ysf_demod *d)
{
    while (fsk4_have(&d->x, sym_t(d->hunt_t, SYNC_SYMS) + FINE_STEP)) {
        if (d->locked && (d->since_sync < HUNT_SYMS)) {
            d->cand = false;
            d->hunt_t = sym_t(d->t, d->k); // kept up with the time line
            return;
        }
        int32_t t = d->hunt_t;
        uint32_t i0 = (uint32_t)(t >> Q);
        int ph = ((t & (ONE - 1)) + HUNT_STEP / 2) / HUNT_STEP;
        if (ph >= 3) {
            i0++;
            ph = 0;
        }
        d->hunt_t = (ph == 2) ? (int32_t)((i0 + 1) << Q) : (int32_t)((i0 << Q) + (uint32_t)(ph + 1) * HUNT_STEP);
        uint64_t win = sign_window(d, i0) & sign_mask[ph];
        int same = SYNC_SYMS - __builtin_popcountll(win ^ sign_positive[ph]);
        int sign = (same >= HUNT_SIGNS) ? 1 : (same <= SYNC_SYMS - HUNT_SIGNS) ? -1 : 0;
        fsk4_fit_t f;
        if (sign && fit_sync(d, t, sign, HUNT_ERR, &f) && (!d->cand || (f.err < d->cand_fit.err))) {
            d->cand = true;
            d->cand_t = t;
            d->cand_sign = sign;
            d->cand_fit = f;
        }
        if (d->cand && ((t - d->cand_t) > 2 * ONE)) { // the best of those close together, refined
            int32_t c = d->cand_t;
            for (int s = -2; s <= 2; s++) {
                fsk4_fit_t g;
                if (s && fit_sync(d, c + s * FINE_STEP / 2, d->cand_sign, d->cand_fit.err, &g)) {
                    d->cand_t = c + s * FINE_STEP / 2;
                    d->cand_fit = g;
                }
            }
            d->cand = false;
            lock_at(d, d->cand_t, d->cand_sign, &d->cand_fit);
        }
    }
}

static unsigned decide(int32_t z) // z: Q8 levels
{
    return (z >= 512) ? 1 : (z >= 0) ? 0 : (z >= -512) ? 2 : 3;
}

// locked: the symbols whose samples are in, out
static void track(ysf_demod *d)
{
    while (d->locked && fsk4_have(&d->x, sym_t(d->t, d->k + 1) + TRACK * FINE_STEP)) {
        int32_t z0 = symbol(d, d->k);
        unsigned db = decide((243 * d->z1 - 13 * (d->z2 + z0)) >> 8); // symbol k - 1, equalised (fsk4_eq3())
        d->z2 = d->z1;
        d->z1 = z0;
        d->k++;
        d->since_sync++;
        d->symbols++;
        d->last = (d->last << 2) | db;
        d->cb(d->ctx, (int)db);
        // the sync just out (its last symbol k - 2): its place and the levels again from it; the dibits right but the
        // fit not good enough (weak): a sync still, the time line and levels kept
        uint64_t diff = (d->last ^ SYNC_WORD) & SYNC_MASK;
        int wrong = 0;
        for (int i = 0; (i < SYNC_SYMS) && (wrong <= SYNC_WRONG); i++) {
            wrong += ((diff >> (2 * i)) & 3) != 0;
        }
        if ((wrong <= SYNC_WRONG) && (d->since_sync >= SYNC_SYMS)) {
            int32_t due = sym_t(d->t, d->k - (SYNC_SYMS + 1)), best_t = 0, best_err = TRACK_ERR;
            fsk4_fit_t best = {0, 0, 0};
            for (int s = -TRACK; s <= TRACK; s++) {
                fsk4_fit_t f;
                if (fit_sync(d, due + s * FINE_STEP, d->sign, best_err, &f)) {
                    best_err = f.err;
                    best = f;
                    best_t = due + s * FINE_STEP;
                }
            }
            if (best_err < TRACK_ERR) {
                d->t = best_t;
                d->g = best.g;
                d->dc = best.dc;
                restart(d, SYNC_SYMS);
            }
            d->since_sync = 0;
            d->syncs++;
        }
        if (d->since_sync >= LOST_SYMS) {
            d->locked = false;
            d->lost++;
            d->hunt_t = sym_t(d->t, d->k);
            d->cb(d->ctx, YSF_DEMOD_LOST);
        }
    }
}

void ysf_demod_samples(ysf_demod *d, const int16_t *x, int count)
{
    for (int k = 0; k < count; k++) {
        int32_t s = fsk4_fir_step(&d->rrc, x[k]);
        uint32_t i = d->x.n & d->x.mask;
        if (s > (d->dc_slow >> 8)) {
            d->signs[i >> 5] |= 1U << (i & 31);
        } else {
            d->signs[i >> 5] &= ~(1U << (i & 31));
        }
        fsk4_put(&d->x, s);
        d->dc_slow += ((s << 8) - d->dc_slow) >> 10;
    }
    int32_t oldest = oldest_t(d);
    if (d->hunt_t < oldest) {
        d->hunt_t = oldest;
        d->cand = false;
    }
    if (d->locked && (sym_t(d->t, d->k) < oldest)) { // fell behind (the task late): hunting again
        d->locked = false;
        d->lost++;
        d->hunt_t = oldest;
        d->cb(d->ctx, YSF_DEMOD_LOST);
    }
    hunt(d);
    track(d);
    hunt(d); // after a loss, or once no sync came for a while
    if (d->x.n >= REBASE_AT) {
        fsk4_rebase(&d->x, REBASE_BY);
        int32_t by = (int32_t)(REBASE_BY << Q);
        d->hunt_t -= by;
        d->t -= by;
        d->cand_t -= by;
    }
}
