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

/*
 * OpenDMR, service: the factory calibration (functions/calibration.c of the main firmware: dm32ReadFactory(),
 * calibrationGetRxDacGain(), calibrationGetModBiasForFrequency(), dm32GetPowerForFrequency()).
 */

#include <string.h>
#include "util.h"
#include "flash.h"
#include "factory_cal.h"

#define SEARCH_START  0x1000
#define SEARCH_END    0xC9000
#define SECTOR_SIZE   4096
#define SECTOR_TAG    0x02
#define AT_OSC_TUNE   0x016
#define AT_UHF_BIAS   0x017
#define AT_RX_ADJUST  0x027
#define AT_UHF_POWER  0x040
#define AT_VHF_BIAS   0x0A6
#define AT_VHF_POWER  0x0D0
#define AT_RX_LEVELS  0x20A
#define AT_UHF_FREQS  0x300
#define AT_VHF_FREQS  0x328
#define UHF_FIRST_MIN 38000000UL // 10 Hz units: the sanity check of the main firmware
#define UHF_FIRST_MAX 48000000UL
#define VHF_FIRST_MIN 13000000UL
#define VHF_FIRST_MAX 18000000UL

#define DEFAULT_ANALOG_RX  13 // the stock defaults
#define DEFAULT_DIGITAL_RX 39
#define DEFAULT_MOD_BIAS   0x4D4D
#define FALLBACK_PA_DRIVE  1408 // about 1 W, without a table
#define PA_DRIVE_MAX       4095
#define RX_ADJUST_FROM_TX  384 // RX Freq Adjust is near TX Freq Adjust + 384 (both radios here: exactly)

static factory_cal_t cal;

static uint32_t bcd_le(const uint8_t *bytes)
{
    uint32_t bcd = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    uint32_t value = 0;

    for (int i = 0; i < 8; i++) {
        value = (value * 10) + (bcd >> 28);
        bcd <<= 4;
    }
    return value;
}

static bool read_freqs(uint32_t sector, uint32_t at, uint32_t *freqs)
{
    uint8_t raw[FACTORY_CAL_POINTS * 4];

    if (!flash_read_raw(sector + at, raw, sizeof(raw))) {
        return false;
    }
    for (int i = 0; i < FACTORY_CAL_POINTS; i++) {
        freqs[i] = bcd_le(&raw[i * 4]);
    }
    return true;
}

static bool read_sector(uint32_t sector)
{
    if (!read_freqs(sector, AT_UHF_FREQS, cal.freqs[FACTORY_CAL_UHF]) ||
        !read_freqs(sector, AT_VHF_FREQS, cal.freqs[FACTORY_CAL_VHF])) {
        return false;
    }
    if ((cal.freqs[FACTORY_CAL_UHF][0] < UHF_FIRST_MIN) || (cal.freqs[FACTORY_CAL_UHF][0] > UHF_FIRST_MAX) ||
        (cal.freqs[FACTORY_CAL_VHF][0] < VHF_FIRST_MIN) || (cal.freqs[FACTORY_CAL_VHF][0] > VHF_FIRST_MAX)) {
        return false;
    }
    uint8_t rx_adjust[2];

    if (!flash_read_raw(sector + AT_RX_ADJUST, rx_adjust, sizeof(rx_adjust))) {
        return false;
    }
    cal.rx_adjust = (uint16_t)(rx_adjust[0] | (rx_adjust[1] << 8));

    return flash_read_raw(sector + AT_OSC_TUNE, &cal.osc_tune, 1) &&
           flash_read_raw(sector + AT_RX_LEVELS, cal.rx_levels, sizeof(cal.rx_levels)) &&
           flash_read_raw(sector + AT_UHF_POWER, cal.power[FACTORY_CAL_UHF], sizeof(cal.power[FACTORY_CAL_UHF])) &&
           flash_read_raw(sector + AT_VHF_POWER, cal.power[FACTORY_CAL_VHF], sizeof(cal.power[FACTORY_CAL_VHF])) &&
           flash_read_raw(sector + AT_UHF_BIAS, cal.bias[FACTORY_CAL_UHF], sizeof(cal.bias[FACTORY_CAL_UHF])) &&
           flash_read_raw(sector + AT_VHF_BIAS, cal.bias[FACTORY_CAL_VHF], sizeof(cal.bias[FACTORY_CAL_VHF]));
}

void factory_cal_init(void)
{
    memset(&cal, 0, sizeof(cal));
    for (uint32_t sector = SEARCH_START; sector < SEARCH_END; sector += SECTOR_SIZE) {
        uint8_t tag;

        if (flash_read_raw(sector + SECTOR_SIZE - 1, &tag, 1) && (tag == SECTOR_TAG) && read_sector(sector)) {
            cal.valid = true;
            cal.sector = sector;
            break;
        }
    }

    // A table without the field (never written): the value it nearly always has
    if (cal.valid && ((cal.rx_adjust == 0x0000) || (cal.rx_adjust == 0xFFFF))) {
        cal.rx_adjust = (uint16_t)(cal.osc_tune + RX_ADJUST_FROM_TX);
    }
}

// The calibration point at or below the frequency, and how far above it (the stock points: UHF 400 - 470 MHz in
// 17.5 MHz steps, VHF 136 - 174 MHz in 9.5 MHz steps); outside them the first or last pair, not extrapolated
static int point_of(uint32_t frequency, int32_t *remainder, int32_t *step)
{
    bool uhf = (frequency >= 28000000);
    int32_t base = uhf ? 40000000 : 13600000;
    int32_t offset = (int32_t)frequency - base;

    *step = uhf ? 1750000 : 950000;
    if (offset <= 0) {
        *remainder = 0;
        return 0;
    }
    if (offset >= (*step * (FACTORY_CAL_POINTS - 1))) {
        *remainder = *step;
        return FACTORY_CAL_POINTS - 2;
    }
    *remainder = offset % *step;
    return offset / *step;
}

static int band_of(uint32_t frequency)
{
    return (frequency >= 28000000) ? FACTORY_CAL_UHF : FACTORY_CAL_VHF;
}

const factory_cal_t *factory_cal_get(void)
{
    return &cal;
}

uint8_t factory_cal_analog_rx_volume(void)
{
    uint8_t level = cal.rx_levels[0];

    return (cal.valid && (level >= 1) && (level <= 15)) ? level : DEFAULT_ANALOG_RX;
}

// level - 31 dB (0..31: -31..0 dB, 32..63: +1..+32 dB), as the stock firmware; 39 gives +8 dB
uint8_t factory_cal_rx_dac_gain(void)
{
    uint8_t level = cal.rx_levels[1];

    if (!cal.valid || (level < 1) || (level > 63)) {
        level = DEFAULT_DIGITAL_RX;
    }
    return 0x80 | ((level < 32) ? (31 - level) : (95 - level));
}

// 4FSK (Test Mode "4FSK(400)" / "4FSK(136)"), interpolated between the calibration points as the power and rounded:
// exactly the table's value at each point; the same byte in both halves
uint16_t factory_cal_mod_bias(uint32_t frequency)
{
    int32_t remainder;
    int32_t step;

    if (!cal.valid) {
        return DEFAULT_MOD_BIAS;
    }

    int index = point_of(frequency, &remainder, &step);
    const uint8_t *bias = cal.bias[band_of(frequency)];
    int32_t sum = (bias[index] * (step - remainder)) + (bias[index + 1] * remainder);
    uint8_t value = (uint8_t)((sum + (step / 2)) / step);

    return (uint16_t)((value << 8) | value);
}

void factory_cal_pa_drive(uint32_t frequency, uint16_t drive[FACTORY_CAL_LEVELS])
{
    int32_t remainder;
    int32_t step;

    if (!cal.valid) {
        for (int level = 0; level < FACTORY_CAL_LEVELS; level++) {
            drive[level] = FALLBACK_PA_DRIVE;
        }
        return;
    }

    int index = point_of(frequency, &remainder, &step);
    const uint8_t *power = cal.power[band_of(frequency)];

    for (int level = 0; level < FACTORY_CAL_LEVELS; level++) {
        int32_t lower = power[(level * FACTORY_CAL_POINTS) + index] << 4;
        int32_t upper = power[(level * FACTORY_CAL_POINTS) + index + 1] << 4;

        drive[level] =
            (uint16_t)CLAMP(lower + (int32_t)(((int64_t)(upper - lower) * remainder) / step), 0, PA_DRIVE_MAX);
    }
}
