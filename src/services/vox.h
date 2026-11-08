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

#ifndef DM32_VOX_H
#define DM32_VOX_H

#include <stdbool.h>
#include <stdint.h>

// The VOX decision of the stock firmware (DM32.01.L01.048 0x305fabc), one sample of the VOX ADC channel
// (adc_read_vox()) each VOX_SAMPLE_MS, its main loop pass: over the threshold of the level two samples in a row keys
// the transmitter; while keyed one sample over 2/3 of it starts the tail again (ours: the stock firmware asks two over
// the threshold and cut the speech too often); each other sample counts the tail, which then releases it. The radio
// task keys the transmitter with it (FM and DMR: the ADC hears the mic whichever chip has it).

#define VOX_SAMPLE_MS 100
#define VOX_LEVELS    13 // settings.vox_threshold 1 .. 13 (0: off): 5 .. 13 are the stock firmware's 1 .. 9

void vox_reset(uint32_t settle_ms); // starts again: no sample taken for settle_ms, not triggered
// A sample; vox_level: settings.vox_threshold, tail_units: 500 ms. true while triggered
bool vox_sample(uint16_t level, uint8_t vox_level, uint8_t tail_units);

#endif
