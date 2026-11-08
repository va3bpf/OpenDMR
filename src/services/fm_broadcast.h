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

#ifndef DM32_FM_BROADCAST_H
#define DM32_FM_BROADCAST_H

#include <stdbool.h>
#include <stdint.h>

// The FM broadcast receiver (RDA5807M), 87.0 - 108.0 MHz in 100 kHz steps: frequencies in 100 kHz units (999 is
// 99.9 MHz).
#define FM_BROADCAST_FREQ_MIN 870
#define FM_BROADCAST_FREQ_MAX 1080
#define FM_BROADCAST_RDS_NAME 8  // programme service name, characters
#define FM_BROADCAST_RDS_TEXT 64 // radio text, characters

typedef struct {
    uint16_t frequency; // 100 kHz units
    uint8_t rssi;       // dBuV, 0 - 127
    bool stereo;
    bool station; // the receiver takes it for a station
    bool seeking;
} fm_broadcast_status_t;

bool fm_broadcast_start(uint16_t frequency); // powers the receiver up, tunes, the speaker amplifier on; false: no chip
void fm_broadcast_stop(void);
bool fm_broadcast_is_on(void);
void fm_broadcast_tune(uint16_t frequency); // wraps at the band edges
void fm_broadcast_seek(bool up);            // returns at once; the status tells when it has finished
void fm_broadcast_get_status(fm_broadcast_status_t *status);
void fm_broadcast_mute(bool mute); // the squelch of the screen: muted off a station

// RDS as received so far ("" when none); fm_broadcast_poll_rds() true: one of them changed
bool fm_broadcast_poll_rds(void);
const char *fm_broadcast_rds_name(void);
const char *fm_broadcast_rds_text(void);

#endif
