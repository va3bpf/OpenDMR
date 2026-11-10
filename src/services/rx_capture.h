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

#ifndef DM32_RX_CAPTURE_H
#define DM32_RX_CAPTURE_H

#include <stdint.h>

// Debug: every AMBE frame the decoder gets, written to the data flash (RX_CAPTURE_BASE, a ring of RX_CAPTURE_SECTORS
// 4 KB sectors, below the window; erased on the two DM-32s checked, it may hold stock data on others: back up the data
// flash first) for tools/debug/rx_capture.py. 16 bytes a frame:
// seq (3, LE, continued after a restart), the low 16 bits of the tick in ms (2, LE), the 9 AMBE bytes, the decoder's
// kind ('V' voice, 'R' repeated, 'E' erasure, 'T' tone, 'M' muted: ambe_dec_frame()), the call number (1).
#define RX_CAPTURE_BASE    0x68C000
#define RX_CAPTURE_SECTORS 80 // to 0x6DC000: 20480 frames, 6.8 minutes of voice

// Built only with make RX_CAPTURE=1 (Makefile); otherwise these do nothing
#ifdef RX_CAPTURE
void rx_capture_start(void);                            // the writer task
void rx_capture_frame(const uint8_t ambe[9], int kind); // from the decoder (HR_C7000 task)
void rx_capture_new_call(void);                         // codec reset: the next frames are a new call
#else
static inline void rx_capture_start(void)
{
}
static inline void rx_capture_frame(const uint8_t ambe[9], int kind)
{
    (void)ambe;
    (void)kind;
}
static inline void rx_capture_new_call(void)
{
}
#endif

#endif
