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
 * OpenDMR, service: the VOX decision (vox.h), as the stock firmware (DM32.01.L01.048 0x305fabc, run once a
 * main loop pass, 100 ms): the threshold of the level from its table (0x30b5238, levels 5 - 13 here; 1 - 4 are more
 * sensitive ones of ours, the stock level 1 was not enough); a sample over it marks the next one, a second one over it
 * in a row keys the transmitter and clears the tail count; a sample under it clears the mark and counts the tail up to
 * the VOX delay (0.1 s units there, its menu 0.3 - 5.0 s), the next one releases it. Ours while keyed: one sample over
 * 2/3 of the threshold clears the tail (the stock rule cut the speech at its quieter syllables). The level is the
 * loudest of the ADC readings of the window (radio.c, each 25 ms pass), not one reading as the stock firmware: that one
 * often fell between the syllables. While transmitting the mic reaches the ADC weaker (the readings step down by about
 * a third when the transmitter is keyed, then mostly decay): the window and the lower hold level keep it keyed.
 */

#include "vox.h"

#define TAIL_UNIT_MS 500 // settings.vox_tail_units

static const uint16_t thresholds[VOX_LEVELS] = {30, 50, 70, 90, 114, 134, 152, 159, 162, 165, 168, 171, 174};

static struct {
    uint32_t settle_samples;
    uint32_t tail;  // samples under the threshold since the transmitter was keyed or kept
    bool over;      // the last sample was over the threshold
    bool triggered;
} vox;

void vox_reset(uint32_t settle_ms)
{
    vox.triggered = false;
    vox.over = false;
    vox.tail = 0;
    vox.settle_samples = settle_ms / VOX_SAMPLE_MS;
}

bool vox_sample(uint16_t level, uint8_t vox_level, uint8_t tail_units)
{
    uint16_t threshold = thresholds[((vox_level < 1) ? 1 : ((vox_level > VOX_LEVELS) ? VOX_LEVELS : vox_level)) - 1];
    uint32_t tail_samples = ((uint32_t)tail_units * TAIL_UNIT_MS) / VOX_SAMPLE_MS;

    if (vox.settle_samples > 0) {
        vox.settle_samples--;
        return false;
    }
    if (vox.triggered && (level > ((threshold * 2) / 3))) {
        vox.over = (level > threshold);
        vox.tail = 0;
    } else if (level > threshold) {
        if (vox.over) {
            vox.triggered = true;
            vox.tail = 0;
        }
        vox.over = true;
    } else if (level < threshold) {
        vox.over = false;
        if (vox.tail < tail_samples) {
            vox.tail++;
        } else {
            vox.triggered = false;
        }
    }
    return vox.triggered;
}
