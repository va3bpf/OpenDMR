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

// The software receivers' common running (NXDN, M17, DMR): the modem records the FM discriminator (afrec_start(),
// p25_phy.c), one task reads each half (256 samples, every 32 ms) into the receiver running; the PTT held, the
// receiver's transmission with the playback and the recording off, then receive again. One receiver at a time; its
// state lives in its overlay (claimed while it runs), the task and its stack are shared.
#ifndef AFRX_H
#define AFRX_H

#include <stdbool.h>
#include <stdint.h>

#include "voice_play.h"

// counters kept in the receiver's own (read over the cable), each NULL if it keeps none
typedef struct {
    volatile uint32_t *halves;      // read
    volatile uint32_t *missed;      // halves that went by unread (the task late)
    volatile uint32_t *busy_us_max; // the longest a half took
    volatile uint32_t *busy_us_sum;
} afrx_stats_t;

typedef struct {
    uint32_t bw;                     // the recording's FM_BANDWIDTH (AFREC_BW_...)
    int overlay;                     // OVERLAY_AMBE, OVERLAY_M17 (vocoder.h)
    const voice_play_cfg_t *play;    // its playback
    void (*rx_init)(void);           // the receiver's state from scratch (starting, and after transmitting)
    void (*samples)(int16_t *x, int n); // a recorded half
    void (*poll)(void);              // every few ms after the halves: timeouts (or NULL)
    void (*end_call)(void);          // a call shown ended now (stopping, transmitting; or NULL)
    void (*transmit)(void);          // the PTT held: the transmission, until the PTT is released (afrx_ptt())
    void (*data)(void);              // a data transmission asked (afrx_data()): sent, then back (or NULL)
    afrx_stats_t stats;
} afrx_ops_t;

// Start the receiver (the recording, its overlay, its state, the playback); false: the modem is another's
bool afrx_start(const afrx_ops_t *ops);
// Stop it (from the radio task): returns once the task has left the receiver alone
void afrx_stop(void);
bool afrx_running(const afrx_ops_t *ops);
// The PTT for the receiver running: held, its transmit() runs in the task
void afrx_ptt(bool on);
bool afrx_ptt_held(void);        // for transmit(): go on while true
bool afrx_transmitting(void);    // the PTT held or the transmission still ending
void afrx_data(void);            // the receiver's data() to run in the task (as a transmission: the recording off)

// For the transmit() of the receivers that send 4FSK through the physical layer's FIFO (NXDN, M17): the mic on, the
// FIFO emptied, the modem's level scaled (MODEM_MOD_SCALE_ONE: as calibrated) ...
void afrx_fifo_prepare(uint32_t mod_scale);
// ... then, the first symbols put, the transmitter keyed (false: it could not be) ...
bool afrx_fifo_key(bool low_power);
// ... and at the end the last block sent, the transmitter off, the level and the mic as before; returns the FIFO's
// underruns (keyed false: only the level and the mic)
uint32_t afrx_fifo_finish(bool keyed);

#endif
