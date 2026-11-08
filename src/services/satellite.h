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

#ifndef DM32_SATELLITE_H
#define DM32_SATELLITE_H

#include <stdbool.h>
#include <stdint.h>

// Amateur satellites: the keps the CPS writes into the codeplug, their position seen from the location of the
// settings (Plan 13, as the main firmware), and the coming passes, predicted a few steps at a time (satellite.c).
#define SATELLITE_MAX           25 // in the codeplug
#define SATELLITE_PASSES        4  // kept per satellite, within SATELLITE_PREDICT_HOURS
#define SATELLITE_PREDICT_HOURS 24
#define SATELLITE_NAME_LENGTH   8

typedef enum {
    SATELLITE_FREQ_VOICE = 0,
    SATELLITE_FREQ_APRS,
    SATELLITE_FREQ_OTHER,
    SATELLITE_FREQS
} satellite_freq_kind_t;

typedef struct {
    uint32_t rx_hz; // 0: none
    uint32_t tx_hz;
    uint16_t tx_ctcss; // Hz * 10; voice only, 0: none
    uint16_t arm_ctcss;
} satellite_freq_t;

typedef struct {
    uint32_t aos; // UTC seconds
    uint32_t los;
    int16_t max_elevation; // degrees; -1: not computed yet (satellite_max_elevation())
} satellite_pass_t;

typedef struct {
    int16_t elevation; // degrees, rounded
    int16_t azimuth;
    uint32_t rx_hz[SATELLITE_FREQS]; // corrected for the Doppler shift; 0: none
    uint32_t tx_hz[SATELLITE_FREQS];
} satellite_look_t;

int satellite_load(void); // reads the keps from the codeplug; the number of satellites (0: none)
int satellite_count(void);
const char *satellite_name(int sat);
const satellite_freq_t *satellite_freq(int sat, satellite_freq_kind_t kind);

// The location of the settings (settings.location_lat / lon); false: not set
bool satellite_set_observer_from_settings(void);
void satellite_look(int sat, uint32_t utc, satellite_look_t *look);

// Predictions: start, then step until done (each step computes at most 'budget' positions, a few tens of µs each).
// Passes that are over are dropped and the next ones predicted by the following steps.
void satellite_predict_start(uint32_t now);
bool satellite_predict_step(uint32_t now, int budget); // true: all the satellites predicted
int satellite_predicted_count(void);                   // satellites done, for a progress bar
int satellite_pass_count(int sat);
const satellite_pass_t *satellite_pass(int sat, int n);
int satellite_max_elevation(int sat, int n); // computed on the first call

#endif
