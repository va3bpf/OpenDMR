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
 * OpenDMR, service: the battery voltage. After OpenDM32 user/SOURCE/interfaces/adc.c
 * (HAL_ADC_ConvCpltCallback(), once a second) and user_interface/uiUtilities.c getBatteryPercentage().
 *
 * The voltage is averaged over 1000 samples (about a quarter of an hour), after the first 1.5 s that take each
 * sample as it is (the ADC settles). The calibration of the settings shifts it by -5 to +10 x 0.1 V.
 */

#include <stdint.h>
#include "hw.h"
#include "adc.h"
#include "settings.h"
#include "systime.h"
#include "hal.h"
#include "battery.h"

#define ADC_PER_VOLT_X100 82 // ADC counts per 0.1 V on the DM-32, x 10 (8.2)
#define AVERAGE_SAMPLES   1000
#define SETTLE_MS         1500
#define FULL_X10          82 // 8.2 V
#define EMPTY_X10         64 // 6.4 V: the warning threshold (upper hysteresis of the main firmware)
#define LOW_X10           62

static volatile int32_t average_x10_x1000 = 0; // the average x 1000: no float in the interrupt, no truncation

// The history of the radio information's graph (menuRadioInfos.c batteryVoltageHistory): the average, every
// HISTORY_S seconds, the oldest dropped
#define HISTORY_S 40
static uint8_t history[BATTERY_HISTORY_MAX];
static volatile uint16_t history_count = 0; // all the samples so far
static uint16_t history_seconds = 0;

void RAM_CODE battery_sample_1s(void)
{
    int voltage = ((adc_read_battery() * 10) / ADC_PER_VOLT_X100) + ((settings.battery_calibration & 0x0F) - 5);

    if (systime_get_ms() < SETTLE_MS) {
        average_x10_x1000 = voltage * 1000;
    } else {
        // average = (average * (N - 1) + voltage) / N, N = 1000
        average_x10_x1000 += voltage - (average_x10_x1000 / AVERAGE_SAMPLES);
    }
    if (++history_seconds >= HISTORY_S) {
        history_seconds = 0;
        history[history_count % BATTERY_HISTORY_MAX] = (uint8_t)battery_voltage_x10();
        history_count++;
    }
}

int battery_history(uint8_t *out, int max)
{
    uint32_t psr = irq_save();
    int total = history_count;
    int count = (total < BATTERY_HISTORY_MAX) ? total : BATTERY_HISTORY_MAX;

    count = (count < max) ? count : max;
    for (int i = 0; i < count; i++) {
        out[i] = history[(total - count + i) % BATTERY_HISTORY_MAX];
    }
    irq_restore(psr);
    return count;
}

int battery_voltage_x10(void)
{
    return (int)((average_x10_x1000 + 500) / 1000);
}

int battery_percent(void)
{
    int percent = (int)(((average_x10_x1000 / 10) - (EMPTY_X10 * 100)) / (FULL_X10 - EMPTY_X10));

    return (percent < 0) ? 0 : ((percent > 100) ? 100 : percent);
}

bool battery_is_low(void)
{
    return (average_x10_x1000 < (LOW_X10 * 1000));
}
