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

#ifndef DM32_RSSI_SCREEN_H
#define DM32_RSSI_SCREEN_H

#include <stdbool.h>
#include <stdint.h>
#include "ui.h"

// The signal strength screen (main menu "RSSI")
extern const ui_screen_t rssi_screen;

// What the receiver measures (radio.h), read every RSSI_SCREEN_REFRESH_MS
#define RSSI_SCREEN_REFRESH_MS 200

typedef struct {
    bool valid;         // the receiver is running
    int16_t dbm;        // trxGetRSSIdBm() of the main firmware
    uint8_t raw_signal; // the receiver's raw signal and noise values (shown with *)
    uint8_t raw_noise;
    uint8_t shown;    // the S meter scale of the main firmware (radioGetRxSignalShown()): S1 at 81, 6 a unit, S9 at 129
    bool open;        // squelch open (audio)
    uint32_t rx_freq; // 10 Hz units; 0: the channel shown on the channel screen
    bool digital;     // DMR, else FM
} rssi_signal_t;

#endif
