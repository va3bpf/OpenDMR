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

// The modem's prompt FIFO and path, one task writing them: the decoded voice of the receivers that record the
// discriminator (NXDN, M17, DMR: a queue of 20 ms frames at 8 kHz, its RAM the receiver's overlay, one receiver at a
// time) and the beeps (sound.c), mixed.
#ifndef VOICE_PLAY_H
#define VOICE_PLAY_H

#include <stdbool.h>
#include <stdint.h>

#define VOICE_PLAY_SAMPLES 160 // a frame: 20 ms

typedef struct {
    int16_t (*frames)[VOICE_PLAY_SAMPLES]; // the queue
    uint32_t count;                         // its frames
    uint32_t start;                         // frames queued before the playback starts (the decoder's bursts)
    uint32_t start_ms;                      //   and this long since the call's first frame (the bursts' jitter)
    volatile uint32_t *underruns;           // counters (or NULL): out of voice while playing (silence played)
    volatile uint32_t *writes;              //   voice blocks written
    void (*tap)(const int16_t *x, int n);   // the voice written (debug taps), or NULL
} voice_play_cfg_t;

// The queue empty, the playback allowed (a receiver starting, or back from transmitting)
void voice_play_start(const voice_play_cfg_t *cfg);
// The voice off at once, the queue emptied; returns once the task is done with the queue's frames (a receiver stopping
// or about to transmit; from a task)
void voice_play_stop(void);
// A call begins (its header, before the voice): the speaker turned on now, its start (PATH_SETTLE_MS + AMP_START_MS)
// while the first frames come, silence until they are enough
void voice_play_prepare(void);
// The next frame to fill, NULL: the queue full (the frame to be dropped)
int16_t *voice_play_frame(void);
// That frame filled: queued (the playback starts once cfg.start frames are, cfg.start_ms after the call's first)
void voice_play_put(void);
// The call over: what is queued played out, then the speaker off
void voice_play_end(void);
// The beeps (sound.c), mixed into the voice: the source on (the speaker turned on if it is off); ready: playing (the
// amplifier started); a block of 16 samples at a time (false: the last not taken yet; the FIFO paces them); off (the
// speaker off once no source is left)
void voice_play_beep_begin(void);
bool voice_play_beep_ready(void);
bool voice_play_beep_put(const int16_t x[16]);
void voice_play_beep_end(void);

#endif
