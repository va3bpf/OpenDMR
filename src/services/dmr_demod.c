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
 * OpenDMR, service: DMR demodulation (dmr_demod.h) from the FM discriminator at 8 kHz, as worked out on the radio's
 * recordings (dmr_soft/README.md): at -116 dBm frames 2 and 3 of a burst 0 % bad (the HR_C7000 0.3 %).
 *
 * - the samples through a root raised cosine (alpha 0.2), into fsk4.c's ring; a symbol every 5/3 samples, read by
 *   cubic interpolation.
 * - hunting: every third of a sample, the 24 symbols of each SYNC pattern (voice: BS, MS, TDMA direct TS1, TS2; data
 *   their negatives) first by their signs at the nearest samples (each sample's sign kept as a bit: a pattern's 24 at
 *   one of the three phases is a mask over 40 bits), then a least squares fit of gain and DC; the best of those close
 *   together, refined to a sixth of a sample, starts the time line.
 * - locked: a burst every 30 ms (240 samples, 144 symbols: 12 of CACH or guard time, then the burst's 132). Each burst's
 *   sync looked for within a sample of where it is due (its timing, gain and DC then from it); the symbols scaled to
 *   levels, then per burst decision directed: the four levels' means from the decisions, the thresholds at their
 *   midpoints (the FM's non-linearity and DC moved them); then a 3-tap equaliser (-0.05, 0.95, -0.05: what least
 *   squares found on the recordings, the RRC's intersymbol interference at 1.67 samples a symbol); then the decisions.
 *   No sync for MAX_SLOTS bursts: the signal lost, hunting again.
 */

#include <string.h>

#include "dmr_demod.h"

#define Q          FSK4_Q
#define ONE        FSK4_ONE
#define SLOT_T     (240 * ONE)       // 30 ms
#define HUNT_STEP  1365              // a third of a sample, Q12
#define FINE_STEP  683               // a sixth
#define TRACK      6                 // a burst's sync looked for within +-TRACK sixths of a sample
#define G_MIN      (80 << 8)         // gain at least (Q8 a level; a strong signal's +-3 is ~10000)
#define HUNT_ERR   154               // the fit's mean squared error under 0.6 level^2 (Q8) hunting ...
#define TRACK_ERR  200               // ... 0.78 where a sync is due
#define HUNT_SIGNS 21                // of the 24 symbols' signs at least (or at most 24 - this: the data sync)
#define MAX_SLOTS  26                // bursts without a sync: lost (a voice superframe has one every 12 slots)
#define CACH_SYMS  12
#define BURST_SYMS 132
#define SYNC_AT    54                // the sync's first symbol in the burst
#define REBASE_AT  (1U << 18)        // samples: the Q12 times kept within int32
#define REBASE_BY  (1U << 17)        // a multiple of the ring


// the voice patterns' symbols (dibit 01 +3, 00 +1, 10 -1, 11 -3); the data ones are their negatives
static const uint8_t voice_kinds[4] = {DMR_SYNC_BS_VOICE, DMR_SYNC_MS_VOICE, DMR_SYNC_TS1_VOICE, DMR_SYNC_TS2_VOICE};
static int8_t patterns[4][24];      // the voice patterns' levels
static int8_t data_patterns[4][24]; // the data ones'
// hunting at a start of sample i + p / 3: symbol k's nearest sample i + (p + 5k + 1) / 3 (0 .. 39); mask[p] those
// samples' bits, positive[q][p] where pattern q is positive
static uint64_t sign_mask[3], sign_positive[4][3];
static bool patterns_ready;

static int8_t level_of(unsigned dibit)
{
    static const int8_t lv[4] = {1, 3, -1, -3};
    return lv[dibit & 3];
}

static void make_patterns(void)
{
    if (patterns_ready) {
        return;
    }
    for (int p = 0; p < 4; p++) {
        uint64_t w = dmr_sync_word[voice_kinds[p]];
        for (int k = 0; k < 24; k++) {
            patterns[p][k] = level_of((unsigned)(w >> (46 - 2 * k)));
            data_patterns[p][k] = (int8_t)-patterns[p][k];
        }
    }
    for (int ph = 0; ph < 3; ph++) {
        for (int k = 0; k < 24; k++) {
            uint64_t bit = 1ULL << ((ph + 5 * k + 1) / 3);
            sign_mask[ph] |= bit;
            for (int p = 0; p < 4; p++) {
                if (patterns[p][k] > 0) {
                    sign_positive[p][ph] |= bit;
                }
            }
        }
    }
    patterns_ready = true;
}

static int32_t sym_t(int32_t t, int k)
{
    return fsk4_sym_t(t, k, FSK4_SPS_4800_NUM, FSK4_SPS_DEN);
}

void dmr_demod_init(dmr_demod *d, dmr_demod_cb cb, void *ctx)
{
    make_patterns();
    memset(d, 0, sizeof(*d));
    fsk4_fir_init(&d->rrc, fsk4_rrc_4800, d->hist, FSK4_RRC_TAPS, 12);
    fsk4_ring_init(&d->x, d->buf, DMR_DEMOD_RING);
    d->cb = cb;
    d->ctx = ctx;
    d->hunt_t = 8 << Q;
}

// the 24 symbols of a sync starting at t
static void sync_values(const dmr_demod *d, int32_t t, int32_t v[24])
{
    for (int k = 0; k < 24; k++) {
        v[k] = fsk4_at(&d->x, sym_t(t, k));
    }
}

// The sync at t (its first symbol) fitted to pattern p (sign: +1 voice, -1 data); true: under max
static bool fit_sync(const dmr_demod *d, int32_t t, int p, int sign, int32_t max, fsk4_fit_t *f)
{
    int32_t v[24];
    sync_values(d, t, v);
    return fsk4_fit(v, (sign > 0) ? patterns[p] : data_patterns[p], 24, G_MIN, max, f);
}

// 64 of the samples' sign bits from sample i on (bit k: sample i + k)
static uint64_t sign_window(const dmr_demod *d, uint32_t i)
{
    const uint32_t words = DMR_DEMOD_RING / 32;
    uint32_t w = (i & d->x.mask) >> 5, b = i & 31;
    uint64_t lo = d->signs[w] | ((uint64_t)d->signs[(w + 1) % words] << 32);
    if (!b) {
        return lo;
    }
    return (lo >> b) | ((uint64_t)d->signs[(w + 2) % words] << (64 - b));
}

// a hunting start's sample and phase (0, 1/3, 2/3)
static void hunt_at(int32_t t, uint32_t *i, int *ph)
{
    *i = (uint32_t)(t >> Q);
    *ph = ((t & (ONE - 1)) + HUNT_STEP / 2) / HUNT_STEP;
    if (*ph >= 3) {
        (*i)++;
        *ph = 0;
    }
}

// the kind of pattern p with sign
static int kind_of(int p, int sign)
{
    return voice_kinds[p] + ((sign < 0) ? 1 : 0);
}

// how far t (a sync's start) is from the time line's syncs, Q12 (-120 .. 120 samples)
static int32_t off_line(const dmr_demod *d, int32_t t)
{
    int32_t off = (t - (d->t + sym_t(0, SYNC_AT))) % SLOT_T;
    if (off > SLOT_T / 2) {
        off -= SLOT_T;
    } else if (off < -SLOT_T / 2) {
        off += SLOT_T;
    }
    return off;
}

// hunting, also while locked (another transmission starting with another timing): the candidates whose symbols are
// all in; locked, those on the time line are the tracking's
static void hunt(dmr_demod *d)
{
    while (fsk4_have(&d->x, sym_t(d->hunt_t, 24) + FINE_STEP)) {
        int32_t t = d->hunt_t;
        if (d->locked && (t + 3 * ONE > d->t + sym_t(0, SYNC_AT))) {
            return; // not ahead of the tracking: what it hasn't looked at yet stays for it
        }
        uint32_t i0;
        int ph;
        hunt_at(t, &i0, &ph);
        d->hunt_t = (ph == 2) ? (int32_t)((i0 + 1) << Q) : (int32_t)((i0 << Q) + (uint32_t)(ph + 1) * HUNT_STEP);
        if (d->locked && (off_line(d, t) > -3 * ONE) && (off_line(d, t) < 3 * ONE)) {
            continue;
        }
        uint64_t win = sign_window(d, i0) & sign_mask[ph];
        for (int p = 0; p < 4; p++) {
            int same = 24 - __builtin_popcountll(win ^ sign_positive[p][ph]);
            int sign = (same >= HUNT_SIGNS) ? 1 : (same <= 24 - HUNT_SIGNS) ? -1 : 0;
            fsk4_fit_t f;
            if (sign && fit_sync(d, t, p, sign, HUNT_ERR, &f) && (!d->cand || (f.err < d->cand_fit.err))) {
                d->cand = true;
                d->cand_t = t;
                d->cand_kind = kind_of(p, sign);
                d->cand_fit = f;
            }
        }
        if (d->cand && ((t - d->cand_t) > 2 * ONE)) { // the best of those close together, refined
            int p = 0, voice = (d->cand_kind & 1) ? d->cand_kind : (d->cand_kind - 1);
            while (voice_kinds[p] != voice) {
                p++;
            }
            int sign = (d->cand_kind & 1) ? 1 : -1;
            int32_t c = d->cand_t;
            for (int s = -2; s <= 2; s++) {
                fsk4_fit_t f;
                if (s && fit_sync(d, c + s * FINE_STEP / 2, p, sign, d->cand_fit.err, &f)) {
                    d->cand_t = c + s * FINE_STEP / 2;
                    d->cand_fit = f;
                }
            }
            int32_t t0 = d->cand_t - sym_t(0, SYNC_AT);
            if (d->locked) { // a new time line: its first burst not before the next one due on the old line
                d->relocks++;
                while (t0 < d->t - SLOT_T / 2) {
                    t0 += SLOT_T;
                }
            }
            d->locked = true;
            d->cand = false;
            d->t = t0;
            d->g = d->cand_fit.g;
            d->dc = d->cand_fit.dc;
            d->since_sync = 0;
            d->syncs++;
        }
    }
}

static unsigned dibit_of(int32_t z)
{
    return (z >= 512) ? 1 : (z >= 0) ? 0 : (z >= -512) ? 2 : 3;
}

// the burst at d->t: its sync looked for, its symbols out
static void burst(dmr_demod *d)
{
    dmr_burst_t b;
    memset(&b, 0, sizeof(b));
    // the sync where it is due: timing, gain and DC from it
    int32_t due = d->t + sym_t(0, SYNC_AT);
    int32_t best_err = TRACK_ERR, best_t = 0;
    fsk4_fit_t best = {0, 0, 0};
    for (int s = -TRACK; s <= TRACK; s++) {
        int32_t tt = due + s * FINE_STEP;
        int32_t v[24];
        sync_values(d, tt, v);
        for (int p = 0; p < 4; p++) {
            for (int sign = 1; sign >= -1; sign -= 2) {
                fsk4_fit_t f;
                if (fsk4_fit(v, (sign > 0) ? patterns[p] : data_patterns[p], 24, G_MIN, best_err, &f)) {
                    best_err = f.err;
                    best = f;
                    best_t = tt;
                    b.sync = kind_of(p, sign);
                }
            }
        }
    }
    if (b.sync != DMR_SYNC_NONE) {
        d->t = best_t - sym_t(0, SYNC_AT);
        d->g = best.g;
        d->dc = best.dc;
        d->since_sync = 0;
    } else {
        d->since_sync++;
    }
    b.since_sync = d->since_sync;
    b.g = d->g;
    // the CACH and the burst as levels (Q8)
    int32_t z[CACH_SYMS + BURST_SYMS];
    int32_t gs = (d->g >> 8) ? (d->g >> 8) : 1;
    int32_t t0 = d->t - sym_t(0, CACH_SYMS);
    for (int k = 0; k < CACH_SYMS + BURST_SYMS; k++) {
        z[k] = (fsk4_at(&d->x, sym_t(t0, k)) - d->dc) * 256 / gs;
    }
    fsk4_relevel(&z[CACH_SYMS], BURST_SYMS);
    fsk4_relevel(z, CACH_SYMS);
    // the equaliser (the burst's symbols but its first and last)
    int32_t *y = &z[CACH_SYMS];
    fsk4_eq3(y, BURST_SYMS);
    int64_t e2 = 0;
    for (int k = 0; k < BURST_SYMS; k++) {
        unsigned db = dibit_of(y[k]);
        b.bits[k >> 2] |= (uint8_t)(db << (6 - 2 * (k & 3)));
        int32_t e = y[k] - level_of(db) * 256;
        e2 += (int64_t)e * e;
    }
    b.mse = (int32_t)((e2 / BURST_SYMS) >> 8);
    for (int k = 0; k < CACH_SYMS; k++) {
        b.cach = (b.cach << 2) | dibit_of(z[k]);
    }
    d->bursts++;
    d->cb(d->ctx, &b);
}

static void track(dmr_demod *d)
{
    // a burst needs its samples up to its end (and the sync search's margin)
    while (d->locked && fsk4_have(&d->x, d->t + sym_t(0, BURST_SYMS + 1) + TRACK * FINE_STEP)) {
        burst(d);
        d->t += SLOT_T;
        if (d->since_sync >= MAX_SLOTS) {
            d->locked = false;
            d->lost++;
            d->hunt_t = d->t;
            return;
        }
    }
}

void dmr_demod_samples(dmr_demod *d, const int16_t *x, int count)
{
    for (int k = 0; k < count; k++) {
        int32_t s = fsk4_fir_step(&d->rrc, x[k]);
        s = (s > 65535) ? 65535 : (s < -65535) ? -65535 : s;
        uint32_t i = d->x.n & d->x.mask;
        if (s > (d->dc_slow >> 8)) {
            d->signs[i >> 5] |= 1U << (i & 31);
        } else {
            d->signs[i >> 5] &= ~(1U << (i & 31));
        }
        fsk4_put(&d->x, s);
        d->dc_slow += ((s << 8) - d->dc_slow) >> 10;
    }
    // never further back than the ring holds
    int32_t oldest = (d->x.n > DMR_DEMOD_RING - 16) ? (int32_t)((d->x.n - DMR_DEMOD_RING + 16) << Q) : 0;
    if (d->hunt_t < oldest) {
        d->hunt_t = oldest;
        d->cand = false;
    }
    if (d->locked && ((d->t - sym_t(0, CACH_SYMS)) < oldest)) { // fell behind (the task late): hunting again
        d->locked = false;
        d->lost++;
        d->hunt_t = oldest;
    }
    hunt(d);
    track(d);
    hunt(d); // after a loss
    if (d->x.n >= REBASE_AT) {
        fsk4_rebase(&d->x, REBASE_BY);
        int32_t by = (int32_t)(REBASE_BY << Q);
        d->hunt_t -= by;
        d->t -= by;
        d->cand_t -= by;
    }
}
