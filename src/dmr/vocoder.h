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

// Our own AMBE+2 vocoder (vocoder/c/ambe), run from the RAM (ckcpu.ld .vocoder_overlay)
#ifndef VOCODER_H
#define VOCODER_H

#include <stdbool.h>
#include <stdint.h>

// Copies the vocoder into its RAM if it isn't there yet; the HR_C7000 task only
void vocoder_sync(void);

// The overlay RAM (ckcpu.ld): our AMBE vocoder (DMR) or the P25 receiver's IMBE decoder, one at a time. Loads the one
// asked for (zeroing its variables) if the other one is there.
enum { OVERLAY_NONE, OVERLAY_AMBE, OVERLAY_P25 };
void overlay_use(int which);
// Copies it into the RAM again (after a test overwrote it: vocoder/py/ram_test.py); its state starts afresh
void vocoder_load(void);

// One 9-byte AMBE frame -> 160 samples / 160 samples -> one frame (one frame of delay), the vocoder loaded
void vocoderDecodeFrame(const uint8_t *in9, int16_t *pcm);
void vocoderEncodeFrame(const int16_t *pcm, uint8_t *out9);
// A new call: our encoder and decoder start afresh at their next frame (codecInit())
void vocoderReset(void);

// Timing over the cable (vocoder/py/vocoder_bench.py, called through the stack trampoline): what_n = what << 16 | n,
// returns the milliseconds for n runs of
//   0: 1000 float multiplies, 1: 1000 float adds, 2: 1000 integer multiply-adds,
//   3: our decoder on a frame of data (n frames, data: n x 9 bytes)
//   4: our encoder on n frames (data: 4 frames of 160 samples, used over and over)
// 3 and 4 need the vocoder loaded, after the first DMR call (0xFFFFFFFF if not).
uint32_t vocoder_bench(uint32_t what_n, const uint8_t *data);

#endif
