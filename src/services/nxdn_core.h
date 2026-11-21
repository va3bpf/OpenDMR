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

// NXDN48 receive from FM discriminator audio (8 kHz), our own implementation of the public NXDN TS 1-A
// (nxdn/README.md): no OS, no hardware; also built on the PC (nxdn/c) against the radio's recordings.
#ifndef NXDN_CORE_H
#define NXDN_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "fsk4.h"

extern volatile int nxdn_rx_opt; // the receiver's options (nxdn_core.c), tests
extern int nxdn_rx_lp_hz;

#define NXDN_FS          8000
#define NXDN_FRAME_SYMS  192
#define NXDN_RING        2048 // filtered samples kept (256 ms; a frame is 640)
#define NXDN_LP_TAPS     31
#define NXDN_CANDIDATES  3

// LICH (TS 1-A 4.5.1.1): the RF channel type (bits 6-5), the functional channel (4-3), the option (2-1), the direction
#define NXDN_LICH_RF(l)     (((l) >> 5) & 3)  // 2 RDCH (direct / conventional), 1 RTCH (trunked traffic)
#define NXDN_LICH_FUNC(l)   (((l) >> 3) & 3)  // 2 SACCH superframe, 0 SACCH non-superframe, 3 idle
#define NXDN_LICH_OPTION(l) (((l) >> 1) & 3)  // 3 voice in both halves, 0 FACCH1 in both, 1 / 2 one each

#define NXDN_MSG_VCALL   0x01
#define NXDN_MSG_TX_REL  0x08

typedef struct {
    uint8_t lich;         // 7 bits
    bool lich_ok;         // the parity
    uint8_t sacch_sr;     // structure (bits 7-6: 3 the first of 4 .. 0 the last) and RAN (5-0)
    uint32_t sacch_data;  // 18 bits
    bool sacch_ok;        // CRC6
    uint8_t halves;       // per half (bit 0 the first): 1 voice (two VCHs), else FACCH1 or nothing
    uint8_t facch_ok;     // per half: a FACCH1 with a good CRC12 in it
    uint8_t facch[2][10]; // its message
    uint8_t vch[4][9];    // the AMBE+2 frames (DMR's bit order), valid where halves says voice
    uint16_t fsw_corr;    // how well its FSW matched, Q12 (4096: perfectly)
    int32_t level;        // the +-3 deviation in sample units (the FSW fit)
} nxdn_frame_t;

typedef void (*nxdn_frame_cb)(void *ctx, const nxdn_frame_t *frame);

// All integer (the CPU has no FPU: a float multiply ~1.6 us): samples as int32, times in samples Q12
typedef struct {
    // the input chain
    int32_t x_prev;              // for the inverse of the de-emphasis
    int32_t hist[NXDN_LP_TAPS];  // the low-pass's input
    fsk4_fir lp;                 // the low-pass (fsk4.h)
    int32_t buf[NXDN_RING];      // filtered samples (within +-65535)
    fsk4_ring ring;
    // the frame timing (samples Q12)
    int state;                   // 0 hunting, 2 deciding between the candidates, 1 locked
    int32_t hunt_t;              // hunting: the next start to try
    int32_t hunt_first;          // the first over the threshold (-1: none yet)
    int32_t hunt_end;            // where the hunt stops looking for a better one
    int ncand;                   // the best peaks over the threshold, apart (state 2: told by decoding)
    int32_t cand_t[NXDN_CANDIDATES], cand_c[NXDN_CANDIDATES]; // their times, correlations (Q12, signed)
    int32_t t0;                  // locked: the next frame's FSW start
    int32_t period;              // per symbol (10 / 3 samples nominal)
    int misses;                  // locked: frames in a row without their FSW
    int unchecked;               //   ... with their FSW, but no CRC holding
    int polarity;                // +1 / -1
    nxdn_frame_cb cb;
    void *ctx;
    // settings and stats
    int32_t deemph_q4;           // the de-emphasis time constant undone, samples Q4 (600 us at 8 kHz: 4.8 -> 77)
    uint32_t frames, lost_sync, hunts, false_locks;
} nxdn_rx;

void nxdn_rx_init(nxdn_rx *r, nxdn_frame_cb cb, void *ctx);
// 8 kHz discriminator samples in; every frame found goes to the callback (from within this call)
void nxdn_rx_samples(nxdn_rx *r, const int16_t *x, int count);

// A layer 3 message (TS 1-A 6.4.1): VCALL / TX_REL's call type (0 broadcast, 1 group, 4 individual ...), source and
// destination; false for other messages
bool nxdn_msg_call(const uint8_t *msg, int len, uint8_t *type, uint16_t *src, uint16_t *dst);

// The SACCH superframe: the 4 SACCH parts of 18 bits gathered into a 9-byte message (true when the 4th came)
typedef struct {
    uint8_t got;     // bit per part (0 the first)
    uint32_t parts[4];
} nxdn_sacch_super;
bool nxdn_sacch_add(nxdn_sacch_super *s, const nxdn_frame_t *f, uint8_t msg[9]);

/* ---- transmit (TS 1-A 4.4 - 4.6; nxdn/py/tx.py the same in Python): symbols as dibits, +3 1, +1 0, -1 2, -3 3 ---- */

#define NXDN_PREAMBLE_SYMS 12   // 0x5775FD, before the first frame only
#define NXDN_LICH_FIRST    0x40 // RDCH, non-superframe SACCH, two FACCH1, SU: the VCALL in both
#define NXDN_LICH_VOICE    0x56 // RDCH, superframe SACCH, VCH only, SU
// the last frame as the first, the TX_REL in its place: MMDVMHost takes a VCALL / TX_REL from frames with the single
// SACCH only (with 0x50, superframe SACCH and two FACCH1, it saw no end: "transmission lost", 2026-11-17)

void nxdn_tx_preamble(uint8_t out[NXDN_PREAMBLE_SYMS]);
// a frame: the FSW, then LICH, the SACCH (SR + 18 bits) and the 144 middle symbols, scrambled
void nxdn_tx_frame(uint8_t lich, uint8_t sr, uint32_t sacch18, const uint8_t middle[144], uint8_t out[NXDN_FRAME_SYMS]);
void nxdn_tx_facch1(const uint8_t msg[10], uint8_t out[72]); // a message of 10 bytes -> 72 symbols
void nxdn_tx_vch(const uint8_t ambe[9], uint8_t out[36]);     // an AMBE+2 frame (DMR's bit order) -> 36 symbols
// VCALL (6.4.1.1) and TX_REL (6.4.1.6), 10 bytes (zeros after): the call type (group, individual), mode 0 (4800
// EHR), clear
#define NXDN_CALL_GROUP      1
#define NXDN_CALL_INDIVIDUAL 4
void nxdn_msg_vcall(uint8_t msg[10], uint8_t type, uint16_t src, uint16_t dst);
void nxdn_msg_tx_rel(uint8_t msg[10], uint8_t type, uint16_t src, uint16_t dst);

// A whole call as it goes on: nxdn_tx_begin() the preamble and the first frame, nxdn_tx_voice() a frame of 4 voice
// frames (its SACCH a quarter of the VCALL), nxdn_tx_end() the last frame (TX_REL); each returns the symbols written
typedef struct {
    uint16_t src, dst;
    uint8_t ran, type;
    uint32_t frames; // voice frames sent: the SACCH part is frames % 4
    uint8_t vcall[10];
} nxdn_tx;
int nxdn_tx_begin(nxdn_tx *t, uint16_t src, uint16_t dst, uint8_t ran, uint8_t type, uint8_t *out); // 12 + 192
int nxdn_tx_voice(nxdn_tx *t, const uint8_t ambe[4][9], uint8_t *out);                // 192
int nxdn_tx_end(nxdn_tx *t, uint8_t *out);                                           // 192

#endif
