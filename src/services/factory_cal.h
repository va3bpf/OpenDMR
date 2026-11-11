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

#ifndef DM32_FACTORY_CAL_H
#define DM32_FACTORY_CAL_H

#include <stdbool.h>
#include <stdint.h>

// The factory calibration of the stock firmware, read only (functions/calibration.c dm32ReadFactory()). It is in the
// 25Q128 below the window of this firmware: the 4 KB sector between 0x1000 and 0xC9000 whose last byte is 0x02 and
// whose frequency tables start in the UHF and VHF bands (codeplug data can end with the same tag). Layout (reverse
// engineered from OpenGD77_DM32_20260711):
//   0x016          reference oscillator tune
//   0x017 - 0x01B  UHF modulation bias at the 5 UHF calibration frequencies
//   0x040 - 0x04E  UHF PA DAC / 16: the stock Low (1 W), Middle (4 W), High (10 W), 5 points each
//   0x0A6 - 0x0AA  VHF modulation bias
//   0x0D0 - 0x0DE  VHF PA DAC / 16
//   0x20A, 0x20B   receive levels: analog (1 - 15), digital (1 - 63)
//   0x300, 0x328   UHF, VHF calibration frequencies: 5 little endian BCD uint32 each, 10 Hz units
// NOTHING IS WRITTEN: a wrong table would break the tuning of the radio.

#define FACTORY_CAL_POINTS 5
#define FACTORY_CAL_LEVELS 3 // the stock Low, Middle, High: rated 1 W, 4 W, 10 W

enum {
    FACTORY_CAL_VHF,
    FACTORY_CAL_UHF,
    FACTORY_CAL_BANDS
};

typedef struct {
    bool valid;
    uint32_t sector;
    uint8_t osc_tune;
    uint8_t rx_levels[2];                                                      // analog, digital
    uint32_t freqs[FACTORY_CAL_BANDS][FACTORY_CAL_POINTS];                     // 10 Hz units
    uint8_t power[FACTORY_CAL_BANDS][FACTORY_CAL_LEVELS * FACTORY_CAL_POINTS]; // PA DAC / 16, level after level
    uint8_t bias[FACTORY_CAL_BANDS][FACTORY_CAL_POINTS];
} factory_cal_t;

void factory_cal_init(void); // finds and reads the table (about 200 short reads)
const factory_cal_t *factory_cal_get(void);

uint8_t factory_cal_analog_rx_volume(void);        // FD6818 0x48 low bits in FM (1 - 15, default 13)
uint8_t factory_cal_rx_dac_gain(void);             // GCR_DACL, from the digital receive level
uint16_t factory_cal_mod_bias(uint32_t frequency); // low half of RF_MOD_BIAS_CTRL
// PA DAC values (0 - 4095) for 1 W, 4 W, 10 W at a frequency, interpolated between the calibration points
void factory_cal_pa_drive(uint32_t frequency, uint16_t drive[FACTORY_CAL_LEVELS]);

#endif
