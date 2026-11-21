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
 * OpenDMR, service: the 4FSK parts the software receivers share (fsk4.h).
 */

#include <string.h>

#include "fsk4.h"

void fsk4_ring_init(fsk4_ring *r, int32_t *buf, uint32_t size)
{
    memset(buf, 0, size * sizeof(int32_t));
    r->x = buf;
    r->mask = size - 1;
    r->n = 0;
}

// Catmull-Rom between p1 and p2, the fraction in Q8: with the samples within +-65535 every product stays in 32 bits
int32_t fsk4_at(const fsk4_ring *r, int32_t t)
{
    int32_t i = t >> FSK4_Q;
    int32_t u = (t & (FSK4_ONE - 1)) >> (FSK4_Q - 8);
    int32_t p0 = r->x[(uint32_t)(i - 1) & r->mask];
    int32_t p1 = r->x[(uint32_t)i & r->mask];
    int32_t p2 = r->x[(uint32_t)(i + 1) & r->mask];
    int32_t p3 = r->x[(uint32_t)(i + 2) & r->mask];
    int32_t c3 = 3 * (p1 - p2) + p3 - p0;
    int32_t c2 = 2 * p0 - 5 * p1 + 4 * p2 - p3 + ((u * c3) >> 8);
    int32_t c1 = p2 - p0 + ((u * c2) >> 8);
    return p1 + ((u * c1) >> 9);
}

void fsk4_rebase(fsk4_ring *r, uint32_t by)
{
    r->n -= by;
}

// Q12, sum 4096: dmr_soft/py/dmr_tone.py rrc(0.2), 8 kHz, 4800 symbols/s
const int16_t fsk4_rrc_4800[FSK4_RRC_TAPS] = {19,  -9,   -27, 76,   -57, -92,  246, -113, -451, 1167, 2578,
                                               1167, -451, -113, 246, -92, -57, 76,   -27,  -9,   19};

void fsk4_fir_init(fsk4_fir *f, const int16_t *taps, int32_t *hist, int n, int shift)
{
    memset(hist, 0, (size_t)n * sizeof(int32_t));
    f->taps = taps;
    f->hist = hist;
    f->n = n;
    f->pos = 0;
    f->shift = shift;
}

int32_t fsk4_fir_step(fsk4_fir *f, int32_t x)
{
    f->hist[f->pos] = x;
    int32_t acc = 0;
    int k = f->pos;
    for (int j = 0; j < f->n; j++) {
        acc += f->taps[j] * f->hist[k];
        k = (k == 0) ? (f->n - 1) : (k - 1);
    }
    f->pos = (f->pos + 1) % f->n;
    return acc >> f->shift;
}

bool fsk4_fit(const int32_t *v, const int8_t *p, int n, int32_t g_min, int32_t err_max, fsk4_fit_t *out)
{
    int32_t sp = 0, spp = 0;
    int64_t sv = 0, spv = 0;
    for (int k = 0; k < n; k++) {
        sp += p[k];
        spp += p[k] * p[k];
        sv += v[k];
        spv += (int64_t)p[k] * v[k];
    }
    int64_t det = (int64_t)n * spp - (int64_t)sp * sp;
    if (det <= 0) {
        return false;
    }
    int32_t g = (int32_t)((((int64_t)n * spv - (int64_t)sp * sv) << 8) / det);
    if (g < g_min) {
        return false;
    }
    int32_t dc = (int32_t)((sv * 256 - (int64_t)g * sp) / ((int64_t)n * 256));
    int64_t e2 = 0;
    for (int k = 0; k < n; k++) {
        int32_t e = v[k] - ((g * p[k]) >> 8) - dc;
        e2 += (int64_t)e * e;
    }
    // (e2 / n) / (g / 256)^2, Q8
    int64_t gg = ((int64_t)g * g) >> 8;
    int32_t err = (int32_t)((e2 * 256 * 256) / ((gg ? gg : 1) * n));
    if (err >= err_max) {
        return false;
    }
    out->g = g;
    out->dc = dc;
    out->err = err;
    return true;
}

void fsk4_relevel(int32_t *z, int n)
{
    int32_t sum[4] = {0}, cnt[4] = {0};
    for (int k = 0; k < n; k++) {
        int i = (z[k] >= 512) ? 3 : (z[k] >= 0) ? 2 : (z[k] >= -512) ? 1 : 0;
        sum[i] += z[k];
        cnt[i]++;
    }
    int32_t m[4];
    for (int i = 0; i < 4; i++) {
        m[i] = cnt[i] ? (sum[i] / cnt[i]) : (2 * i - 3) * 256;
    }
    int32_t tn = (m[0] + m[1]) / 2, t0 = (m[1] + m[2]) / 2, tp = (m[2] + m[3]) / 2;
    int32_t a = (m[3] - tp > 1) ? (m[3] - tp) : 1, b = (tp - t0 > 1) ? (tp - t0) : 1;
    int32_t c = (t0 - tn > 1) ? (t0 - tn) : 1, e = (tn - m[0] > 1) ? (tn - m[0]) : 1;
    for (int k = 0; k < n; k++) {
        int32_t v = z[k];
        if (v >= tp) {
            z[k] = 512 + (v - tp) * 256 / a;
        } else if (v >= t0) {
            z[k] = 512 * (v - t0) / b;
        } else if (v >= tn) {
            z[k] = -512 * (t0 - v) / c;
        } else {
            z[k] = -512 - (tn - v) * 256 / e;
        }
    }
}

void fsk4_eq3(int32_t *z, int n)
{
    int32_t prev = z[0];
    for (int k = 1; k < n - 1; k++) {
        int32_t cur = z[k];
        z[k] = (243 * cur - 13 * (prev + z[k + 1])) >> 8;
        prev = cur;
    }
}

int fsk4_soft(int32_t v, int32_t g, int32_t dc, int one)
{
    int32_t gs = g >> 8;
    int32_t z = ((v - dc) * one) / (gs ? gs : 1);
    return (z > 127) ? 127 : (z < -127) ? -127 : (int)z;
}
