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
 * OpenDMR, service: NXDN48 receive from FM discriminator audio (nxdn_core.h).
 *
 * Demodulation (nxdn/README.md, the radio's FM recording): the de-emphasis undone (x + tau dx/dt), a low-pass at
 * 0.6 x the symbol rate, the FSW (TS 1-A Table 4.4-2) searched every 1/3 sample (0.1 symbol) by normalised
 * correlation, the best over a whole frame taken; once found, each frame is timed on its own FSW (searched within half
 * a symbol of where it is due), scaled and offset by a fit of its 10 FSW symbols, and its 192 symbols read by cubic
 * interpolation. Frames (TS 1-A 4.4 - 4.6): the 182 symbols after the FSW descrambled (X^9 + X^4 + 1 from 0x0E4), LICH
 * (7 bits and parity), SACCH (soft Viterbi K = 5, CRC6), each half voice (2 VCH of 72 bits: AMBE+2 frames in DMR's bit
 * order) or FACCH1 (soft Viterbi, CRC12).
 *
 * All in integers (the DM-32's CPU has no FPU): samples int32 (kept within +-65535), times in samples Q12, symbols
 * after the level fit Q8 (+-3 -> +-768). Only the low-pass's taps are worked out in floats, once.
 */

#include <math.h>
#include <string.h>

#include "nxdn_core.h"

#define Q             12
#define ONE           (1 << Q)               // a sample
#define SPS_NOMINAL   13653                  // 10 / 3 samples a symbol, Q12
#define HUNT_STEP     1365                   // 1/3 sample
#define HUNT_FINE     171                    // 1/24 sample
#define TRACK_STEP    683                    // 1/6 sample
#define HUNT_THRESH   3482                   // 0.85 (Q12): the correlation to take an FSW while hunting
#define TRACK_THRESH  2458                   // 0.60: ... and while locked (near where it is due)
#define HUNT_APART    (3 * SPS_NOMINAL)      // candidates this far apart are different peaks
#define HUNT_SPAN     (NXDN_FRAME_SYMS * SPS_NOMINAL) // looked at after the first over it, for the best ...
#define HUNT_FAST     3891                   // ... but one of 0.95 or more: only its peak (4 symbols), its frame tried
#define FAST_SPAN     (4 * SPS_NOMINAL)      // at once. A whole frame cost the call's first frame now and then; that
                                             // only, a frame's content looking like an FSW (0.88) was taken in a weak
                                             // signal (-115 dBm) where the FSW was 0.99 (2026-11-21)
#define MAX_MISSES    3      // frames in a row without their FSW: hunting again
#define MAX_UNCHECKED 2      // frames in a row with their FSW but no CRC holding (frame_decode()): hunting again
#define PAYLOAD       182
#define LICH_SYMS     8
#define SACCH_SYMS    30
#define VCH_SYMS      36
#define LP_SHIFT      13                     // the low-pass's taps Q13
#define SAMPLE_MAX    65535
#define REBASE_AT     (1u << 16)
#define REBASE_BY     (1u << 15) // a multiple of NXDN_RING

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

static const int8_t fsw[10] = {-3, 1, -3, 3, -3, -3, 3, 3, -1, 3}; // zero mean, sum of squares 74
#define FSW_NORM2 74

static int16_t lp[NXDN_LP_TAPS]; // Q13
static uint8_t pn[PAYLOAD]; // the scrambler's bits: 1 inverts the symbol
static bool tables_ready;

// tests (nxdn/c/test_core.c from the environment): 1 the levels from each frame's decisions (fsk4_relevel(): the FM's
// non-linearity and DC; DMR's and M17's 3-tap equaliser made it worse here); the input low-pass's cut-off
volatile int nxdn_rx_opt = 1;
int nxdn_rx_lp_hz = 2000;

static void tables(void)
{
    if (tables_ready) {
        return;
    }
    // a Hamming-windowed sinc at 2000 Hz for 8 kHz, unit gain at DC (1440 Hz before: the symbols' intersymbol
    // interference cost more than the noise let in, 2026-11-21: -110 dBm 0.67 % -> 0.06 %, -115 dBm 0.82 % -> 0.29 %
    // with the levels from the decisions, nxdn/py/tap_ber.py)
    float h[NXDN_LP_TAPS], sum = 0.0f;
    for (int i = 0; i < NXDN_LP_TAPS; i++) {
        float t = (float)i - (NXDN_LP_TAPS - 1) / 2.0f;
        float fc = (float)nxdn_rx_lp_hz / 8000.0f;
        h[i] = (t == 0.0f) ? 2.0f * fc : sinf(2.0f * (float)M_PI * fc * t) / ((float)M_PI * t);
        h[i] *= 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)i / (NXDN_LP_TAPS - 1));
        sum += h[i];
    }
    for (int i = 0; i < NXDN_LP_TAPS; i++) {
        float q = h[i] / sum * (float)(1 << LP_SHIFT);
        lp[i] = (int16_t)((q >= 0.0f) ? (q + 0.5f) : (q - 0.5f));
    }
    // TS 1-A 4.6: S8 .. S0 from 0 1 1 1 0 0 1 0 0; out S0, S0 ^ S4 shifted in at S8
    uint16_t s = 0x0E4; // bit k = S_k
    for (int i = 0; i < PAYLOAD; i++) {
        uint8_t o = s & 1;
        uint8_t fb = (uint8_t)(o ^ ((s >> 4) & 1));
        pn[i] = o;
        s = (uint16_t)((s >> 1) | (fb << 8));
    }
    tables_ready = true;
}

void nxdn_rx_init(nxdn_rx *r, nxdn_frame_cb cb, void *ctx)
{
    tables();
    memset(r, 0, sizeof(*r));
    fsk4_ring_init(&r->ring, r->buf, NXDN_RING);
    fsk4_fir_init(&r->lp, lp, r->hist, NXDN_LP_TAPS, LP_SHIFT);
    r->cb = cb;
    r->ctx = ctx;
    r->period = SPS_NOMINAL;
    r->polarity = 1;
    r->deemph_q4 = 77; // 4.8 samples
    r->hunt_first = -1;
}

/* ---- sample access ---- */

// the filtered signal at time t (samples Q12), cubic (fsk4.c)
static int32_t at(const nxdn_rx *r, int32_t t)
{
    return fsk4_at(&r->ring, t);
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t res = 0, bit = 1ull << 62;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)res;
}

// the FSW's normalised correlation, Q12 (-4096 .. 4096, the window's mean removed) for a frame starting at t;
// 0 when its magnitude is under min_q12 (the square root only for the ones over it)
static int32_t corr(const nxdn_rx *r, int32_t t, int32_t period, int32_t min_q12)
{
    int32_t v[10], mean = 0;
    for (int k = 0; k < 10; k++) {
        v[k] = at(r, t + k * period);
        mean += v[k];
    }
    mean /= 10;
    int32_t dot = 0;
    int64_t e = 0;
    for (int k = 0; k < 10; k++) {
        int32_t d = v[k] - mean;
        dot += d * fsw[k];
        e += (int64_t)d * d;
    }
    if (e <= 0) {
        return 0;
    }
    // |c| >= min: dot^2 * 2^24 >= min^2 * e * 74 (as dot^2 * 2^4 >= (min^2 >> 20) * e * 74)
    int64_t d2 = (int64_t)dot * dot;
    if ((d2 << 4) < ((((int64_t)min_q12 * min_q12) >> 20) * e * FSW_NORM2)) {
        return 0;
    }
    uint32_t root = isqrt64((uint64_t)e * FSW_NORM2);
    if (root == 0) {
        return 0;
    }
    return (int32_t)(((int64_t)dot << Q) / root);
}

/* ---- channel coding ---- */

#define G1 0x13 // 1 + D^3 + D^4, taps on (input, D, D^2, D^3, D^4) as bits 4 .. 0
#define G2 0x1D // 1 + D + D^2 + D^4

static int parity5(int x)
{
    x &= 0x1F;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return x & 1;
}

// soft pairs (G1, G2; +: bit 0, -: bit 1, 0: unknown; +-256) -> the n input bits; the tail brings the coder back to 0
static void viterbi(const int16_t *soft, int n, uint8_t *bits)
{
    static uint8_t lost[96][16]; // the survivor's bit shifted out of the state (its previous state's bit 0)
    int32_t metric[16], next[16];
    for (int s = 0; s < 16; s++) {
        metric[s] = (s == 0) ? 0 : 0x3FFFFFFF;
    }
    for (int i = 0; i < n; i++) {
        int32_t y1 = soft[2 * i], y2 = soft[2 * i + 1];
        for (int t = 0; t < 16; t++) {
            next[t] = 0x7FFFFFFF;
        }
        for (int s = 0; s < 16; s++) {
            if (metric[s] >= 0x3FFFFFFF) {
                continue;
            }
            for (int b = 0; b < 2; b++) {
                int reg = (b << 4) | s;
                int32_t m = metric[s] - (parity5(reg & G1) ? -y1 : y1) - (parity5(reg & G2) ? -y2 : y2);
                int t = reg >> 1;
                if (m < next[t]) {
                    next[t] = m;
                    lost[i][t] = (uint8_t)(s & 1);
                }
            }
        }
        memcpy(metric, next, sizeof(metric));
    }
    int s = 0;
    for (int i = n - 1; i >= 0; i--) {
        bits[i] = (uint8_t)(s >> 3);
        s = ((s << 1) & 0xF) | lost[i][s];
    }
}

// CRC over bits (register all ones at the start), compared with the check bits that follow them
static bool crc_ok(const uint8_t *bits, int n, int width, uint32_t poly)
{
    uint32_t mask = (1u << width) - 1, reg = mask, top = 1u << (width - 1);
    for (int i = 0; i < n; i++) {
        uint32_t fb = ((reg & top) ? 1u : 0u) ^ bits[i];
        reg = (reg << 1) & mask;
        if (fb) {
            reg ^= poly;
        }
    }
    for (int i = 0; i < width; i++) {
        if (((reg >> (width - 1 - i)) & 1u) != bits[n + i]) {
            return false;
        }
    }
    return true;
}

static int16_t clip256(int32_t v)
{
    return (int16_t)((v > 256) ? 256 : ((v < -256) ? -256 : v));
}

// symbols (Q8) -> deinterleaved (rows x cols, sent in columns), depunctured soft bits -> Viterbi
static void decode(const int16_t *sym, int nsym, int rows, int cols, const uint8_t *keep, int keep_n, int ninputs,
                   uint8_t *bits)
{
    int16_t soft[2 * 72], ordered[2 * 72], full[2 * 96];
    for (int i = 0; i < nsym; i++) { // first bit: the sign (+: 0); second: the magnitude (outer: 1)
        int32_t v = sym[i];
        soft[2 * i] = clip256(v / 2);
        soft[2 * i + 1] = (int16_t)-clip256(((v < 0) ? -v : v) - 512);
    }
    int k = 0;
    for (int c = 0; c < cols; c++) {
        for (int r = 0; r < rows; r++) {
            ordered[r * cols + c] = soft[k++];
        }
    }
    int in = 0;
    for (int o = 0; o < 2 * ninputs; o++) {
        full[o] = keep[o % keep_n] ? ordered[in++] : 0;
    }
    viterbi(full, ninputs, bits);
}

static const uint8_t SACCH_KEEP[12] = {1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0};
static const uint8_t FACCH1_KEEP[4] = {1, 0, 1, 1};

static bool sacch(const int16_t *sym, uint8_t *sr, uint32_t *data)
{
    uint8_t bits[36];
    decode(sym, SACCH_SYMS, 5, 12, SACCH_KEEP, 12, 36, bits);
    uint32_t v = 0;
    for (int i = 0; i < 26; i++) {
        v = (v << 1) | bits[i];
    }
    *sr = (uint8_t)(v >> 18);
    *data = v & 0x3FFFF;
    return crc_ok(bits, 26, 6, 0x27);
}

static bool facch1(const int16_t *sym, uint8_t msg[10])
{
    uint8_t bits[96];
    decode(sym, 72, 9, 16, FACCH1_KEEP, 4, 96, bits);
    memset(msg, 0, 10);
    for (int i = 0; i < 80; i++) {
        msg[i >> 3] |= (uint8_t)(bits[i] << (7 - (i & 7)));
    }
    return crc_ok(bits, 80, 12, 0x80F);
}

// 36 symbols -> 72 bits, 9 bytes (+3 01, +1 00, -1 10, -3 11, Table 3.3-1)
static void vch(const int16_t *sym, uint8_t out[9])
{
    memset(out, 0, 9);
    for (int i = 0; i < VCH_SYMS; i++) {
        int32_t v = sym[i];
        int d = (v >= 512) ? 1 : ((v >= 0) ? 0 : ((v >= -512) ? 2 : 3));
        out[i >> 2] |= (uint8_t)(d << (6 - 2 * (i & 3)));
    }
}

/* ---- a frame ---- */

static int32_t iabs(int32_t v)
{
    return (v < 0) ? -v : v;
}

// The frame at t0 decoded into out; true when it looks real (LICH parity, and the SACCH's CRC on a traffic channel)
static bool frame_decode(const nxdn_rx *r, int32_t t0, int polarity, int32_t c, nxdn_frame_t *out)
{
    int32_t v[NXDN_FRAME_SYMS];
    int16_t p[PAYLOAD];
    nxdn_frame_t f;

    memset(out, 0, sizeof(*out));
    for (int k = 0; k < NXDN_FRAME_SYMS; k++) {
        v[k] = polarity * at(r, t0 + k * r->period);
    }
    // the levels from the FSW (zero mean): v = a * symbol + b
    int32_t a = 0, b = 0;
    for (int k = 0; k < 10; k++) {
        a += v[k] * fsw[k];
        b += v[k];
    }
    a /= FSW_NORM2;
    b /= 10;
    if (a <= 0) {
        return false;
    }
    int32_t z[NXDN_FRAME_SYMS];
    for (int k = 0; k < NXDN_FRAME_SYMS; k++) {
        z[k] = ((v[k] - b) * 256) / a; // Q8 levels (+3: 768)
    }
    if (nxdn_rx_opt & 1) { // the four levels from the frame's own decisions, thresholds at their midpoints
        fsk4_relevel(z, NXDN_FRAME_SYMS);
    }
    for (int k = 0; k < PAYLOAD; k++) {
        int32_t s = z[10 + k];
        s = (s > 2047) ? 2047 : ((s < -2047) ? -2047 : s);
        p[k] = (int16_t)(pn[k] ? -s : s);
    }
    memset(&f, 0, sizeof(f));
    f.fsw_corr = (uint16_t)((c < 0) ? 0 : c);
    f.level = 3 * a;
    uint8_t lb[8];
    for (int k = 0; k < LICH_SYMS; k++) {
        lb[k] = (p[k] < 0) ? 1 : 0;
    }
    for (int k = 0; k < 7; k++) {
        f.lich = (uint8_t)((f.lich << 1) | lb[k]);
    }
    f.lich_ok = ((lb[0] ^ lb[1] ^ lb[2] ^ lb[3]) == lb[7]);
    int rf = NXDN_LICH_RF(f.lich);
    if (f.lich_ok && ((rf == 1) || (rf == 2))) { // traffic: SACCH + two halves
        f.sacch_ok = sacch(&p[LICH_SYMS], &f.sacch_sr, &f.sacch_data);
        int option = NXDN_LICH_OPTION(f.lich);
        for (int h = 0; h < 2; h++) {
            const int16_t *half = &p[LICH_SYMS + SACCH_SYMS + h * 2 * VCH_SYMS];
            bool is_facch = false;
            if (option != 3) { // FACCH1 in both (0) or in one (1, 2: which one told by its CRC)
                is_facch = facch1(half, f.facch[h]);
                if (is_facch) {
                    f.facch_ok |= (uint8_t)(1u << h);
                }
            }
            if ((option == 3) || ((option != 0) && !is_facch)) {
                f.halves |= (uint8_t)(1u << h);
                vch(half, f.vch[2 * h]);
                vch(half + VCH_SYMS, f.vch[2 * h + 1]);
            }
        }
    }
    *out = f;
    // a frame: a traffic one whose SACCH or a FACCH1 CRC holds (the LICH's parity alone passes noise one time in two,
    // and a control channel's had no check: idle noise taken for frames held the lock on it when a call began, its
    // first frames then lost while the lock ran out, 2026-11-20)
    return f.lich_ok && ((rf == 1) || (rf == 2)) && (f.sacch_ok || f.facch_ok);
}

static void deliver(nxdn_rx *r, const nxdn_frame_t *f)
{
    r->frames++;
    if (r->cb) {
        r->cb(r->ctx, f);
    }
}

// hunting: a correlation over the threshold at t into the best few, HUNT_APART apart at least
static void candidate(nxdn_rx *r, int32_t t, int32_t c)
{
    int worst = 0;
    for (int i = 0; i < r->ncand; i++) {
        if ((t - r->cand_t[i]) < HUNT_APART) { // the same peak: the better of the two
            if (iabs(c) > iabs(r->cand_c[i])) {
                r->cand_t[i] = t;
                r->cand_c[i] = c;
            }
            return;
        }
        if (iabs(r->cand_c[i]) < iabs(r->cand_c[worst])) {
            worst = i;
        }
    }
    if (r->ncand < NXDN_CANDIDATES) {
        worst = r->ncand++;
    } else if (iabs(c) <= iabs(r->cand_c[worst])) {
        return;
    }
    r->cand_t[worst] = t;
    r->cand_c[worst] = c;
}

/* ---- the timing ---- */

static void process(nxdn_rx *r)
{
    int32_t have = (int32_t)(r->ring.n << Q);
    int32_t span = 9 * r->period + 3 * ONE; // what an FSW needs from its start
    int32_t oldest = have - ((NXDN_RING - 8) << Q);

    for (;;) {
        if (r->state == 0) {
            // hunting: every 1/3 sample, the best within HUNT_SPAN of the first over the threshold (a frame's content
            // can look like an FSW: the test signal's last 10 symbols did, 10 symbols before the real one; its frame
            // does not decode, and the hunt goes on). It goes on from where it stopped when more samples come.
            if (r->hunt_t < oldest) {
                r->hunt_t = oldest;
                r->hunt_first = -1;
            }
            while ((r->hunt_t + span + 2 * SPS_NOMINAL) < have) {
                if ((r->hunt_first >= 0) && (r->hunt_t > r->hunt_end)) {
                    break;
                }
                int32_t c = corr(r, r->hunt_t, SPS_NOMINAL, HUNT_THRESH);
                if (iabs(c) >= HUNT_THRESH) {
                    if (r->hunt_first < 0) {
                        r->hunt_first = r->hunt_t;
                        r->hunt_end = r->hunt_t + HUNT_SPAN;
                        r->ncand = 0;
                    }
                    if ((iabs(c) >= HUNT_FAST) && (r->hunt_end > r->hunt_t + FAST_SPAN)) {
                        r->hunt_end = r->hunt_t + FAST_SPAN;
                    }
                    candidate(r, r->hunt_t, c);
                }
                r->hunt_t += HUNT_STEP;
            }
            if ((r->hunt_first < 0) || (r->hunt_t <= r->hunt_end)) {
                return; // more samples first
            }
            // the candidates refined to 1/24 sample; which one is the frame: told by decoding (state 2)
            for (int i = 0; i < r->ncand; i++) {
                int32_t t = r->cand_t[i], best = iabs(r->cand_c[i]);
                for (int32_t d = -HUNT_STEP; d <= HUNT_STEP; d += HUNT_FINE) {
                    int32_t c = corr(r, t + d, SPS_NOMINAL, 0);
                    if (iabs(c) > best) {
                        best = iabs(c);
                        r->cand_t[i] = t + d;
                        r->cand_c[i] = c;
                    }
                }
            }
            r->state = 2;
            r->period = SPS_NOMINAL;
            r->misses = 0;
            r->hunts++;
            r->hunt_first = -1;
            continue;
        }
        if (r->state == 2) {
            // each candidate's frame once all of them are in, the best correlated first: the first that decodes
            // (LICH, SACCH) is the frame; none: no frame, hunting again
            int32_t last = r->cand_t[0];
            for (int i = 1; i < r->ncand; i++) {
                last = (r->cand_t[i] > last) ? r->cand_t[i] : last;
            }
            if ((last + (NXDN_FRAME_SYMS + 1) * SPS_NOMINAL + 4 * ONE) >= have) {
                return;
            }
            int order[NXDN_CANDIDATES] = {0}, n = r->ncand;
            for (int i = 0; i < n; i++) {
                order[i] = i;
            }
            for (int i = 0; i < n; i++) {
                for (int j = i + 1; j < n; j++) {
                    if (iabs(r->cand_c[order[j]]) > iabs(r->cand_c[order[i]])) {
                        int x = order[i];
                        order[i] = order[j];
                        order[j] = x;
                    }
                }
            }
            nxdn_frame_t f;
            int pick = order[0];
            bool good = false;
            for (int i = 0; (i < n) && !good; i++) {
                int k = order[i];
                if (frame_decode(r, r->cand_t[k], (r->cand_c[k] > 0) ? 1 : -1, iabs(r->cand_c[k]), &f)) {
                    pick = k;
                    good = true;
                }
            }
            if (!good) { // noise looking like an FSW (often enough over 10 symbols): no lock, hunting on
                r->state = 0;
                r->false_locks++;
                continue;
            }
            r->polarity = (r->cand_c[pick] > 0) ? 1 : -1;
            r->t0 = r->cand_t[pick];
            r->state = 1;
            r->unchecked = 0;
            deliver(r, &f);
            r->t0 += NXDN_FRAME_SYMS * r->period;
            continue;
        }
        // locked: the frame at t0 once all of it is in
        if ((r->t0 + (NXDN_FRAME_SYMS + 1) * r->period + 4 * ONE) >= have) {
            return;
        }
        if (r->t0 < oldest) { // fell behind the ring (the task late): hunting from the newest samples
            r->state = 0;
            r->hunt_t = oldest;
            r->hunt_first = -1;
            r->lost_sync++;
            continue;
        }
        int32_t bt = r->t0, bc = -ONE;
        for (int32_t d = -r->period / 2; d <= r->period / 2; d += TRACK_STEP) {
            int32_t c = r->polarity * corr(r, r->t0 + d, r->period, 0);
            if (c > bc) {
                bc = c;
                bt = r->t0 + d;
            }
        }
        if (bc >= TRACK_THRESH) {
            // the symbol period follows where the FSWs are found (the transmitter's clock against ours)
            int32_t err = bt - r->t0;
            r->period += err / 640; // 0.3 / 192
            if (r->period < SPS_NOMINAL - SPS_NOMINAL / 100) {
                r->period = SPS_NOMINAL - SPS_NOMINAL / 100;
            } else if (r->period > SPS_NOMINAL + SPS_NOMINAL / 100) {
                r->period = SPS_NOMINAL + SPS_NOMINAL / 100;
            }
            r->t0 = bt;
            r->misses = 0;
            nxdn_frame_t f;
            if (frame_decode(r, r->t0, r->polarity, bc, &f)) {
                r->unchecked = 0;
            } else if (++r->unchecked >= MAX_UNCHECKED) { // an FSW, nothing in the frame checked: noise
                r->state = 0;
                r->hunt_t = r->t0 + NXDN_FRAME_SYMS * r->period;
                r->hunt_first = -1;
                r->lost_sync++;
                deliver(r, &f);
                continue;
            }
            deliver(r, &f);
        } else if (++r->misses >= MAX_MISSES) {
            r->state = 0;
            r->hunt_t = r->t0;
            r->hunt_first = -1;
            r->lost_sync++;
            continue;
        }
        r->t0 += NXDN_FRAME_SYMS * r->period;
    }
}

void nxdn_rx_samples(nxdn_rx *r, const int16_t *x, int count)
{
    for (int i = 0; i < count; i++) {
        // a quarter: the recording comes near full scale (an rms of 11000 on the radio), the de-emphasis undone
        // makes that some 4 times more, and the low-pass's sums need it under 2^17 to stay within 32 bits
        int32_t s = x[i] >> 2;
        int32_t y = s + ((r->deemph_q4 * (s - r->x_prev)) >> 4); // the receive de-emphasis undone
        r->x_prev = s;
        y = (y > 131071) ? 131071 : ((y < -131071) ? -131071 : y);
        int32_t acc = fsk4_fir_step(&r->lp, y);
        fsk4_put(&r->ring, (acc > SAMPLE_MAX) ? SAMPLE_MAX : ((acc < -SAMPLE_MAX) ? -SAMPLE_MAX : acc));
    }
    process(r);
    // the times kept small (32-bit Q12 times need them under 2^19 samples), moved back by a multiple of the ring
    if (r->ring.n >= REBASE_AT) {
        fsk4_rebase(&r->ring, REBASE_BY);
        r->t0 -= (int32_t)(REBASE_BY << Q);
        r->hunt_t -= (int32_t)(REBASE_BY << Q);
        if (r->hunt_first >= 0) {
            r->hunt_first -= (int32_t)(REBASE_BY << Q);
            r->hunt_end -= (int32_t)(REBASE_BY << Q);
        }
        for (int i = 0; i < r->ncand; i++) {
            r->cand_t[i] -= (int32_t)(REBASE_BY << Q);
        }
    }
}

/* ---- messages ---- */

bool nxdn_msg_call(const uint8_t *msg, int len, uint8_t *type, uint16_t *src, uint16_t *dst)
{
    int kind = msg[0] & 0x3F;
    if (((kind != NXDN_MSG_VCALL) && (kind != NXDN_MSG_TX_REL)) || (len < 7)) {
        return false;
    }
    *type = (uint8_t)(msg[2] >> 5);
    *src = (uint16_t)((msg[3] << 8) | msg[4]);
    *dst = (uint16_t)((msg[5] << 8) | msg[6]);
    return true;
}

bool nxdn_sacch_add(nxdn_sacch_super *s, const nxdn_frame_t *f, uint8_t msg[9])
{
    if (!f->sacch_ok || (NXDN_LICH_FUNC(f->lich) != 2)) {
        return false;
    }
    int part = 3 - (f->sacch_sr >> 6); // structure 3: the first part
    if (part == 0) {
        s->got = 0;
    }
    s->parts[part] = f->sacch_data;
    s->got |= (uint8_t)(1u << part);
    if ((part != 3) || (s->got != 0xF)) {
        return false;
    }
    memset(msg, 0, 9);
    for (int i = 0; i < 72; i++) {
        uint32_t bit = (s->parts[i / 18] >> (17 - (i % 18))) & 1u;
        msg[i >> 3] |= (uint8_t)(bit << (7 - (i & 7)));
    }
    s->got = 0;
    return true;
}

/* ---- transmit (TS 1-A 4.4 - 4.6; nxdn/py/tx.py) ---- */

static const uint8_t fsw_dibits[10] = {3, 0, 3, 1, 3, 3, 1, 1, 2, 1}; // -3 +1 -3 +3 -3 -3 +3 +3 -1 +3

void nxdn_tx_preamble(uint8_t out[NXDN_PREAMBLE_SYMS])
{
    for (int i = 0; i < NXDN_PREAMBLE_SYMS; i++) {
        out[i] = (uint8_t)((0x5775FDu >> (22 - 2 * i)) & 3u);
    }
}

// CRC check bits appended after n bits (the register all ones at the start)
static void crc_append(uint8_t *bits, int n, int width, uint32_t poly)
{
    uint32_t mask = (1u << width) - 1, reg = mask, top = 1u << (width - 1);
    for (int i = 0; i < n; i++) {
        uint32_t fb = ((reg & top) ? 1u : 0u) ^ bits[i];
        reg = (reg << 1) & mask;
        if (fb) {
            reg ^= poly;
        }
    }
    for (int i = 0; i < width; i++) {
        bits[n + i] = (uint8_t)((reg >> (width - 1 - i)) & 1u);
    }
}

// data bits (n, CRC and the tail added here) -> convolutional code, punctured, interleaved (rows x cols written in
// rows, sent in columns) -> dibits
static void encode(uint8_t *bits, int n, int width, uint32_t poly, const uint8_t *keep, int keep_n, int rows, int cols,
                   uint8_t *out)
{
    uint8_t sent[144], inter[144];
    int ninputs = n + width + 4;
    crc_append(bits, n, width, poly);
    memset(&bits[n + width], 0, 4);
    int s = 0, k = 0;
    for (int i = 0; i < ninputs; i++) {
        int reg = (bits[i] << 4) | s;
        uint8_t c[2] = {(uint8_t)parity5(reg & G1), (uint8_t)parity5(reg & G2)};
        for (int j = 0; j < 2; j++) {
            if (keep[(2 * i + j) % keep_n]) {
                sent[k++] = c[j];
            }
        }
        s = reg >> 1;
    }
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            inter[c * rows + r] = sent[r * cols + c];
        }
    }
    for (int i = 0; i < k / 2; i++) {
        out[i] = (uint8_t)((inter[2 * i] << 1) | inter[2 * i + 1]);
    }
}

static void sacch_encode(uint8_t sr, uint32_t data18, uint8_t out[SACCH_SYMS])
{
    uint8_t bits[36];
    for (int i = 0; i < 8; i++) {
        bits[i] = (uint8_t)((sr >> (7 - i)) & 1u);
    }
    for (int i = 0; i < 18; i++) {
        bits[8 + i] = (uint8_t)((data18 >> (17 - i)) & 1u);
    }
    encode(bits, 26, 6, 0x27, SACCH_KEEP, 12, 5, 12, out);
}

void nxdn_tx_facch1(const uint8_t msg[10], uint8_t out[72])
{
    uint8_t bits[96];
    for (int i = 0; i < 80; i++) {
        bits[i] = (uint8_t)((msg[i >> 3] >> (7 - (i & 7))) & 1u);
    }
    encode(bits, 80, 12, 0x80F, FACCH1_KEEP, 4, 9, 16, out);
}

void nxdn_tx_vch(const uint8_t ambe[9], uint8_t out[36])
{
    for (int i = 0; i < VCH_SYMS; i++) {
        out[i] = (uint8_t)((ambe[i >> 2] >> (6 - 2 * (i & 3))) & 3u);
    }
}

void nxdn_tx_frame(uint8_t lich, uint8_t sr, uint32_t sacch18, const uint8_t middle[144], uint8_t out[NXDN_FRAME_SYMS])
{
    tables();
    uint8_t *p = &out[10];
    memcpy(out, fsw_dibits, 10);
    int par = 0;
    for (int i = 0; i < 7; i++) {
        int b = (lich >> (6 - i)) & 1;
        par ^= (i < 4) ? b : 0;
        p[i] = b ? 3 : 1; // 1: -3, 0: +3
    }
    p[7] = par ? 3 : 1;
    sacch_encode(sr, sacch18, &p[LICH_SYMS]);
    memcpy(&p[LICH_SYMS + SACCH_SYMS], middle, 144);
    for (int i = 0; i < PAYLOAD; i++) {
        if (pn[i]) {
            p[i] ^= 2; // the symbol inverted: the sign bit
        }
    }
}

static void msg_call(uint8_t msg[10], uint8_t kind, uint8_t type, uint8_t mode, uint16_t src, uint16_t dst)
{
    memset(msg, 0, 10);
    msg[0] = kind;
    msg[2] = (uint8_t)(((type & 7u) << 5) | mode);
    msg[3] = (uint8_t)(src >> 8);
    msg[4] = (uint8_t)src;
    msg[5] = (uint8_t)(dst >> 8);
    msg[6] = (uint8_t)dst;
}

void nxdn_msg_vcall(uint8_t msg[10], uint8_t type, uint16_t src, uint16_t dst)
{
    msg_call(msg, NXDN_MSG_VCALL, type, 0, src, dst);
}

void nxdn_msg_tx_rel(uint8_t msg[10], uint8_t type, uint16_t src, uint16_t dst)
{
    msg_call(msg, NXDN_MSG_TX_REL, type, 0, src, dst);
}

// the VCALL's 18 bits of SACCH part k (0 .. 3) of 72 (its 8 bytes and one of zeros)
static uint32_t vcall_part(const uint8_t vcall[10], int k)
{
    uint32_t v = 0;
    for (int i = 18 * k; i < 18 * k + 18; i++) {
        v = (v << 1) | ((vcall[i >> 3] >> (7 - (i & 7))) & 1u);
    }
    return v;
}

// a frame of the message in both FACCH1s, the single SACCH (structure 00) with its first 18 bits
static int tx_facch_frame(nxdn_tx *t, const uint8_t msg[10], uint8_t *out)
{
    uint8_t mid[144];
    nxdn_tx_facch1(msg, mid);
    memcpy(&mid[72], mid, 72);
    nxdn_tx_frame(NXDN_LICH_FIRST, t->ran, vcall_part(msg, 0), mid, out);
    return NXDN_FRAME_SYMS;
}

int nxdn_tx_begin(nxdn_tx *t, uint16_t src, uint16_t dst, uint8_t ran, uint8_t type, uint8_t *out)
{
    memset(t, 0, sizeof(*t));
    t->src = src;
    t->dst = dst;
    t->ran = ran & 0x3F;
    t->type = type;
    nxdn_msg_vcall(t->vcall, type, src, dst);
    nxdn_tx_preamble(out);
    return NXDN_PREAMBLE_SYMS + tx_facch_frame(t, t->vcall, &out[NXDN_PREAMBLE_SYMS]);
}

int nxdn_tx_voice(nxdn_tx *t, const uint8_t ambe[4][9], uint8_t *out)
{
    uint8_t mid[144];
    int part = (int)(t->frames % 4);
    for (int i = 0; i < 4; i++) {
        nxdn_tx_vch(ambe[i], &mid[VCH_SYMS * i]);
    }
    nxdn_tx_frame(NXDN_LICH_VOICE, (uint8_t)(((3 - part) << 6) | t->ran), vcall_part(t->vcall, part), mid, out);
    t->frames++;
    return NXDN_FRAME_SYMS;
}

int nxdn_tx_end(nxdn_tx *t, uint8_t *out)
{
    uint8_t rel[10];
    nxdn_msg_tx_rel(rel, t->type, t->src, t->dst);
    return tx_facch_frame(t, rel, out);
}
